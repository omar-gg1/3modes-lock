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

// Min good frames before any verdict, so a single noisy frame can't pass or
// fail. Lowered 6 -> 4 from hardware data: detection is slow and many frames
// come back no-face (corrupt JPEG / FB-OVF), so a 12s window only harvests
// ~3-6 GOOD frames. Requiring 6 starved ~half of all live attempts (they died
// at frames=3). 4 still demands sustained motion across multiple frames (a lone
// noise spike can't pass) but fits the real good-frame budget. The motion and
// pose thresholds are unchanged — live motion clears them by a wide margin, so
// they are not the bottleneck; the frame SUPPLY was.
#define LIVENESS_MIN_FRAMES        4

// ---- THE ANTI-SPOOF SIGNAL: non-rigid deformation (rigid-vs-3D) ----
//
// WHY motion alone failed: a hand-held photo HAS micro-motion (a trembling hand
// jitters the keypoints just like a live face), so "average nose movement" can
// NOT separate a live face from a shaken photo. Hardware proof: a phone photo
// unlocked the lock because the user's hand-shake fed the motion signal.
//
// THE PHYSICS WE EXPLOIT INSTEAD: a photo is a RIGID PLANE. However you shake or
// tilt it, all keypoints translate/scale/rotate TOGETHER — the geometry BETWEEN
// them stays fixed. A real face is a NON-RIGID 3D object: the nose protrudes
// toward the camera, so the smallest movement causes PARALLAX — the nose's
// projected position shifts relative to the eyes/mouth behind it. So we measure
// shape descriptors that are INVARIANT to translation, scale and in-plane
// rotation, and watch whether they DEFORM:
//   r_along = nose's position along the eye-line, as a fraction between the eyes
//   r_perp  = nose's perpendicular distance from the eye-line / inter-eye dist
// Both are pure shape ratios. A rigid photo (even violently shaken) keeps them
// near-constant; a live 3D face wobbles them. We track the RANGE (max-min) of
// each across the window; their sum is the non-rigidity score. THIS is the
// signal a shaking photo cannot fake.
//
// CALIBRATE these against real live-vs-photo numbers printed in the serial log.
// Starting points; the live face should clear them, a (shaken) photo should not.
#define SHAPE_DEFORM_MIN           0.060f

// Micro-motion is now only a SANITY GATE, not the liveness proof. We still want
// SOME motion (a perfectly frozen frame stream is suspicious / the user walked
// off) and reject absurd jumps, but motion magnitude no longer grants a PASS on
// its own — only non-rigid DEFORMATION does. This is the whole fix.
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
    // Non-rigidity tracking: range of the two invariant shape ratios.
    float along_min, along_max;   // nose position ALONG the eye-line (fraction)
    float perp_min,  perp_max;    // nose distance PERP to the eye-line (norm.)
    // Micro-motion (sanity gate only).
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
    s.along_min = 1e9f;  s.along_max = -1e9f;
    s.perp_min  = 1e9f;  s.perp_max  = -1e9f;
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

// Compute two SHAPE ratios for the nose relative to the eye-line, both invariant
// to translation, scale and in-plane rotation — so a rigid photo that is shaken,
// moved or tilted produces near-CONSTANT values, while a live 3D face deforms
// them via parallax. We express the nose in the coordinate frame of the eye
// vector (L_eye -> R_eye):
//   r_along = projection of (nose - L_eye) onto the unit eye vector, / inter_eye
//             => where the nose sits BETWEEN the eyes (0 at L, 1 at R), fractional
//   r_perp  = perpendicular distance of the nose from the eye-line, / inter_eye
//             => how far the nose is OFF the eye-line (depth/tilt cue)
// Dividing by inter_eye makes both scale-free; projecting onto the eye vector
// makes both rotation-free; using differences makes both translation-free.
// Returns false if the geometry is degenerate (eyes coincident).
static bool compute_shape_ratios(const int *kp, float *out_along, float *out_perp,
                                 float *out_nose_x, float *out_nose_y)
{
    float lx = (float) kp[KP_LEFT_EYE_X];
    float ly = (float) kp[KP_LEFT_EYE_Y];
    float rx = (float) kp[KP_RIGHT_EYE_X];
    float ry = (float) kp[KP_RIGHT_EYE_Y];
    float nx = (float) kp[KP_NOSE_X];
    float ny = (float) kp[KP_NOSE_Y];

    float ex = rx - lx;            // eye vector (L -> R)
    float ey = ry - ly;
    float inter_eye = sqrtf(ex * ex + ey * ey);
    if (inter_eye < 1.0f) {
        // Eyes on top of each other — bad/degenerate detection, skip.
        return false;
    }

    // Unit eye vector and its perpendicular.
    float ux = ex / inter_eye, uy = ey / inter_eye;
    // Nose relative to the left eye.
    float vx = nx - lx, vy = ny - ly;

    // Projection along the eye vector (signed), and perpendicular component.
    float along = (vx * ux + vy * uy) / inter_eye;   // fraction between eyes
    float perp  = (vx * uy - vy * ux) / inter_eye;   // signed perp distance

    *out_along = along;
    *out_perp  = perp;
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

    float along, perp, nose_x, nose_y;
    if (!compute_shape_ratios(keypoints, &along, &perp, &nose_x, &nose_y)) {
        return LIVENESS_PENDING;
    }

    s.frames++;

    // Track the RANGE of each invariant shape ratio — this is the non-rigidity
    // signal. A rigid photo (even shaken/tilted) keeps these ratios constant, so
    // their range stays ~0; a live 3D face deforms them via parallax.
    if (along < s.along_min) s.along_min = along;
    if (along > s.along_max) s.along_max = along;
    if (perp  < s.perp_min)  s.perp_min  = perp;
    if (perp  > s.perp_max)  s.perp_max  = perp;

    // Track per-frame nose movement — now only a sanity gate (some motion must
    // exist; absurd jumps rejected), NOT proof of liveness on its own.
    if (s.have_prev_nose) {
        float mdx = nose_x - s.prev_nose_x;
        float mdy = nose_y - s.prev_nose_y;
        s.motion_accum += sqrtf(mdx * mdx + mdy * mdy);
        s.motion_samples++;
    }
    s.prev_nose_x = nose_x;
    s.prev_nose_y = nose_y;
    s.have_prev_nose = true;

    float along_range = s.along_max - s.along_min;
    float perp_range  = s.perp_max  - s.perp_min;
    // Non-rigidity score: total deformation of the nose's shape relative to the
    // eye-line. Rigid translation/scale/rotation of a photo cannot raise this.
    float shape_deform = along_range + perp_range;
    float avg_motion = (s.motion_samples > 0)
                       ? (s.motion_accum / (float) s.motion_samples)
                       : 0.0f;

    // Progress feedback every frame. shape_deform is THE liveness signal now;
    // motion is shown only as the sanity gate it has become.
    ESP_LOGI(TAG, "liveness... deform=%.3f/%.3f motion=%.1f/%.1fpx frames=%d",
             shape_deform, SHAPE_DEFORM_MIN, avg_motion, MOTION_PIXELS_MIN,
             s.frames);

    // Need a minimum number of frames before any verdict.
    if (s.frames < LIVENESS_MIN_FRAMES) {
        return LIVENESS_PENDING;
    }

    // ---- PASS: live 3D face shown by NON-RIGID deformation. ----
    // The nose's shape ratios deformed past the threshold (parallax from a real
    // 3D face) AND there is plausible motion (sanity gate). A shaken photo can
    // satisfy the motion gate but CANNOT deform the rigid shape — so it fails
    // here. This is the fix for the photo-unlock bug.
    if (shape_deform >= SHAPE_DEFORM_MIN &&
        avg_motion >= MOTION_PIXELS_MIN && avg_motion <= MOTION_PIXELS_MAX) {
        ESP_LOGI(TAG, "PASS: deform=%.3f (>=%.3f) motion=%.2fpx frames=%d",
                 shape_deform, SHAPE_DEFORM_MIN, avg_motion, s.frames);
        return LIVENESS_PASS;
    }

    // ---- Static / rigid-spoof rejection ----
    // After enough frames, if the shape never deformed past the threshold it is
    // a rigid plane (a photo) regardless of how much it moved. We reject on
    // RIGIDITY, not on stillness — this is what catches a SHAKEN photo (high
    // motion, ~zero deformation) that the old motion-only check let through.
    if (s.frames >= LIVENESS_MIN_FRAMES + 4 && shape_deform < SHAPE_DEFORM_MIN) {
        ESP_LOGW(TAG, "FAIL_STATIC: rigid (deform=%.3f<%.3f motion=%.2fpx frames=%d)",
                 shape_deform, SHAPE_DEFORM_MIN, avg_motion, s.frames);
        return LIVENESS_FAIL_STATIC;
    }

    // ---- Out of frames without meeting the challenge. ----
    if (s.frames >= LIVENESS_MAX_FRAMES) {
        ESP_LOGW(TAG, "FAIL_TIMEOUT: deform=%.3f (<%.3f) motion=%.2fpx frames=%d",
                 shape_deform, SHAPE_DEFORM_MIN, avg_motion, s.frames);
        return LIVENESS_FAIL_TIMEOUT;
    }

    return LIVENESS_PENDING;
}
