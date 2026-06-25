#include "liveness_ctrl.h"

#include <math.h>
#include "esp_log.h"

static const char *TAG = "liveness_ctrl";

// ---- Tunable thresholds (CALIBRATE with real people + a photo attack) ----
//
// These are starting points. Like the recognition threshold, they MUST be tuned
// against real data: too strict and legitimate users get locked out; too loose
// and a photo passes. Record the tuning in the thesis test section.
//
// DESIGN NOTE — why this is a "frontal micro-motion" challenge, not "head-turn".
// The original design asked the user to TURN THEIR HEAD and looked for a large
// nose-vs-eyes pose swing. Hardware testing proved this is self-defeating: the
// ESP-WHO 5-keypoint detector is trained on near-FRONTAL faces, so the instant
// the head turns far enough to register, the detector LOSES the face and emits
// no keypoints. Result: the liveness check was fed ~0 frames out of ~93 and
// always timed out (logged as no_face=93). The fix is to keep the user roughly
// frontal — prompt "lean in / move a little" — and detect liveness from the
// internal MICRO-MOTION of the facial keypoints, which the detector CAN see
// because the face never leaves frontal. A flat printed photo held to the camera
// cannot reproduce this internal, non-rigid jitter.

// Max frames to gather before giving up on the challenge. Detections during the
// frontal-motion challenge are now dense (~10-20/s) because the face stays in
// view, so this cap is reached quickly under normal use.
#define LIVENESS_MAX_FRAMES        60

// Min frames before any verdict, so a single noisy frame can't pass or fail.
#define LIVENESS_MIN_FRAMES        6

// Pose wobble: nose horizontal offset from the eye-midpoint, normalized by
// inter-eye distance. Staying frontal this stays small, but natural movement
// (a slight lean, a small nod) still wobbles it a little. This is now only a
// CORROBORATING signal with a SMALL threshold — we are NOT asking for a big
// turn anymore (that broke detection). A rigid photo produces ~0 wobble.
#define POSE_OFFSET_RANGE_MIN      0.020f

// Micro-motion is now the PRIMARY liveness signal: average per-frame nose
// movement in pixels. A live frontal face is never perfectly still — keypoints
// jitter from breathing, micro head-sway, and detector noise on real skin. A
// printed photo held up is far more rigid. Below MIN = suspiciously rigid.
// The MAX guard rejects implausibly large jumps (e.g. detector flicking between
// two faces / a video being waved around), which are not natural frontal motion.
#define MOTION_PIXELS_MIN          1.2f
#define MOTION_PIXELS_MAX          40.0f

// Keypoint index layout (ESP-WHO 5-point order), in ints:
//   [0,1] left eye   [2,3] right eye   [4,5] nose   [6,7] mouthL   [8,9] mouthR
#define KP_LEFT_EYE_X   0
#define KP_LEFT_EYE_Y   1
#define KP_RIGHT_EYE_X  2
#define KP_RIGHT_EYE_Y  3
#define KP_NOSE_X       4
#define KP_NOSE_Y       5
#define KP_EXPECTED_COUNT 10

// ---- Accumulated state for the current attempt ----
typedef struct {
    int   frames;                 // frames fed so far
    float pose_min;               // min normalized nose offset seen
    float pose_max;               // max normalized nose offset seen
    float motion_accum;           // sum of per-frame nose movement (pixels)
    int   motion_samples;         // number of movements summed
    bool  have_prev_nose;
    float prev_nose_x;
    float prev_nose_y;
} liveness_state_t;

static liveness_state_t s;

void liveness_ctrl_begin(void)
{
    s.frames = 0;
    s.pose_min = 1e9f;
    s.pose_max = -1e9f;
    s.motion_accum = 0.0f;
    s.motion_samples = 0;
    s.have_prev_nose = false;
    s.prev_nose_x = 0.0f;
    s.prev_nose_y = 0.0f;
    ESP_LOGD(TAG, "liveness attempt started");
}

bool liveness_ctrl_is_timed_out(void)
{
    return s.frames >= LIVENESS_MAX_FRAMES;
}

// Compute the normalized nose-vs-eyes horizontal offset for one frame.
//   offset = (nose_x - eye_midpoint_x) / inter_eye_distance
// Normalizing by inter-eye distance makes it scale-invariant (works whether the
// face is near or far). Returns false if the geometry is degenerate.
static bool compute_pose_offset(const int *kp, float *out_offset,
                                float *out_nose_x, float *out_nose_y)
{
    float lx = (float) kp[KP_LEFT_EYE_X];
    float ly = (float) kp[KP_LEFT_EYE_Y];
    float rx = (float) kp[KP_RIGHT_EYE_X];
    float ry = (float) kp[KP_RIGHT_EYE_Y];
    float nx = (float) kp[KP_NOSE_X];
    float ny = (float) kp[KP_NOSE_Y];

    float eye_mid_x = (lx + rx) * 0.5f;
    float dx = rx - lx;
    float dy = ry - ly;
    float inter_eye = sqrtf(dx * dx + dy * dy);

    if (inter_eye < 1.0f) {
        // Eyes on top of each other — bad/degenerate detection, skip.
        return false;
    }

    *out_offset = (nx - eye_mid_x) / inter_eye;
    *out_nose_x = nx;
    *out_nose_y = ny;
    return true;
}

liveness_result_t liveness_ctrl_feed(const int *keypoints, int count)
{
    if (keypoints == NULL || count < KP_EXPECTED_COUNT) {
        // Detector gave us too few points this frame; ignore it (don't count it).
        return LIVENESS_PENDING;
    }

    float offset, nose_x, nose_y;
    if (!compute_pose_offset(keypoints, &offset, &nose_x, &nose_y)) {
        return LIVENESS_PENDING;
    }

    s.frames++;

    // Track the range of the pose offset (how far the head turned).
    if (offset < s.pose_min) s.pose_min = offset;
    if (offset > s.pose_max) s.pose_max = offset;

    // Track per-frame nose movement for the micro-motion signal.
    if (s.have_prev_nose) {
        float mdx = nose_x - s.prev_nose_x;
        float mdy = nose_y - s.prev_nose_y;
        s.motion_accum += sqrtf(mdx * mdx + mdy * mdy);
        s.motion_samples++;
    }
    s.prev_nose_x = nose_x;
    s.prev_nose_y = nose_y;
    s.have_prev_nose = true;

    float pose_range = s.pose_max - s.pose_min;
    float avg_motion = (s.motion_samples > 0)
                       ? (s.motion_accum / (float) s.motion_samples)
                       : 0.0f;

    // Progress feedback every frame so the user/dev SEES the motion accumulating
    // toward the target instead of a silent pass/fail. Motion is the primary
    // signal now, so show it against its threshold; pose wobble is secondary.
    ESP_LOGI(TAG, "liveness... motion=%.1f/%.1fpx pose_wobble=%.3f/%.3f frames=%d",
             avg_motion, MOTION_PIXELS_MIN, pose_range, POSE_OFFSET_RANGE_MIN,
             s.frames);

    // Need a minimum number of frames before any verdict.
    if (s.frames < LIVENESS_MIN_FRAMES) {
        return LIVENESS_PENDING;
    }

    // ---- PASS: live frontal face showing natural internal micro-motion. ----
    // PRIMARY signal: the keypoints jitter within the natural range (not too
    // rigid, not implausibly large), AND there is at least a small pose wobble.
    // A live face staying frontal trips both; a rigid printed photo trips
    // neither (no internal motion, no nose-vs-eye wobble). We require enough
    // frames first so a couple of noisy frames can't pass on their own.
    if (s.frames >= LIVENESS_MIN_FRAMES &&
        avg_motion >= MOTION_PIXELS_MIN && avg_motion <= MOTION_PIXELS_MAX &&
        pose_range >= POSE_OFFSET_RANGE_MIN) {
        ESP_LOGI(TAG, "PASS: motion=%.2fpx (>=%.2f) pose_wobble=%.3f frames=%d",
                 avg_motion, MOTION_PIXELS_MIN, pose_range, s.frames);
        return LIVENESS_PASS;
    }

    // ---- Static-image rejection ----
    // Only conclude "photo" if, after enough frames, the geometry is BOTH rigid
    // (near-zero internal motion) AND showed essentially no pose wobble. This
    // avoids failing a real person who briefly held still — they stay PENDING
    // and pass once they move. A held photo trips both and is rejected early.
    if (s.frames >= LIVENESS_MIN_FRAMES + 4 &&
        avg_motion < MOTION_PIXELS_MIN && pose_range < POSE_OFFSET_RANGE_MIN) {
        ESP_LOGW(TAG, "FAIL_STATIC: rigid (motion=%.2fpx pose_wobble=%.3f frames=%d)",
                 avg_motion, pose_range, s.frames);
        return LIVENESS_FAIL_STATIC;
    }

    // ---- Out of frames without meeting the challenge. ----
    if (s.frames >= LIVENESS_MAX_FRAMES) {
        ESP_LOGW(TAG, "FAIL_TIMEOUT: pose_range=%.3f (<%.3f) motion=%.2fpx frames=%d",
                 pose_range, POSE_OFFSET_RANGE_MIN, avg_motion, s.frames);
        return LIVENESS_FAIL_TIMEOUT;
    }

    return LIVENESS_PENDING;
}
