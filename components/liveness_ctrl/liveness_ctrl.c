#include "liveness_ctrl.h"

#include <math.h>
#include "esp_log.h"

static const char *TAG = "liveness_ctrl";

// ---- Tunable thresholds (CALIBRATE with real people + a photo attack) ----
//
// These are starting points. Like the recognition threshold, they MUST be tuned
// against real data: too strict and legitimate users get locked out; too loose
// and a photo passes. Record the tuning in the thesis test section.

// Max frames to gather before giving up on the pose challenge. Detections are
// sparse (~1/s), so a 12s window only yields ~10 frames — cap is generous.
#define LIVENESS_MAX_FRAMES        30

// Min frames before any verdict, so a single frame can't pass/fail. Lowered to 3
// because detections are sparse — demanding more just rejects real users who
// were already recognized. The pose-RANGE requirement still proves real motion.
#define LIVENESS_MIN_FRAMES        3

// Pose challenge: nose horizontal offset from the eye-midpoint, normalized by
// inter-eye distance. ~0 facing dead-on; grows as the head turns. We accumulate
// the min and max across the WHOLE window (frames need not be consecutive) and
// pass once their spread exceeds this — i.e. the head turned far enough at some
// point during the window. Lowered 0.18 -> 0.12: real turns measured 0.18-0.27,
// so 0.12 leaves comfortable margin for the user while a flat photo (which can't
// move the nose relative to the eyes) still won't reach it.
#define POSE_OFFSET_RANGE_MIN      0.12f

// Micro-motion: average per-frame nose movement (px). Below MIN = suspiciously
// rigid (held photo). The MAX guard is generous so natural movement isn't
// distrusted. NOTE: a PASS now needs the pose-turn OR enough motion — see below.
#define MOTION_PIXELS_MIN          0.8f
#define MOTION_PIXELS_MAX          60.0f

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

    // Progress feedback every frame so the user/dev SEES it accumulating toward
    // the target instead of a silent pass/fail. "turn your head" becomes visible.
    ESP_LOGI(TAG, "liveness... pose=%.3f/%.2f motion=%.1fpx frames=%d",
             pose_range, POSE_OFFSET_RANGE_MIN, avg_motion, s.frames);

    // Need a minimum number of frames before any verdict.
    if (s.frames < LIVENESS_MIN_FRAMES) {
        return LIVENESS_PENDING;
    }

    // ---- PASS: the head turned far enough at some point in the window. ----
    // This is the PRIMARY signal and the one a flat photo cannot fake (turning
    // the head moves the nose relative to the eye-line). We accept on the pose
    // turn alone — natural motion always accompanies a real turn anyway.
    if (pose_range >= POSE_OFFSET_RANGE_MIN && avg_motion <= MOTION_PIXELS_MAX) {
        ESP_LOGI(TAG, "PASS: pose_range=%.3f motion=%.2fpx frames=%d",
                 pose_range, avg_motion, s.frames);
        return LIVENESS_PASS;
    }

    // ---- Static-image rejection ----
    // Only conclude "photo" if, after enough frames, the geometry is BOTH rigid
    // (near-zero motion) AND showed no pose change at all. This avoids failing a
    // real person who briefly held still — they'll just stay PENDING and pass
    // once they turn. A held photo trips both and is rejected.
    if (s.frames >= LIVENESS_MIN_FRAMES + 2 &&
        avg_motion < MOTION_PIXELS_MIN && pose_range < 0.03f) {
        ESP_LOGW(TAG, "FAIL_STATIC: rigid (motion=%.2fpx pose_range=%.3f frames=%d)",
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
