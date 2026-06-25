#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * liveness_ctrl — presentation-attack mitigation against static 2D images.
 *
 * IMPORTANT (and what to say in the thesis): this is NOT full liveness
 * detection. It defeats a *printed photo / static image* held up to the camera.
 * It does NOT defend against video-replay (a phone playing a video of you) or
 * 3D masks — those need IR/depth sensing and are documented as future work.
 *
 * Why this design and not "blink detection": the face detector emits only 5
 * keypoints (one center point per eye), which physically cannot measure eyelid
 * closure. So instead we use two geometric signals accumulated over a short
 * window of frames:
 *
 *   1. POSE CHALLENGE (the strong signal). The user is asked to turn their head.
 *      We measure the nose's horizontal offset from the eye-midpoint, normalized
 *      by the inter-eye distance. Turning the head moves the nose off-centre
 *      relative to the eyes — a flat photo cannot reproduce this. We require the
 *      normalized offset to swing past a threshold during the window.
 *
 *   2. MICRO-MOTION (the supporting signal). A live face never holds perfectly
 *      still; keypoints jitter a few pixels frame to frame. A photo held up is
 *      suspiciously rigid. Near-zero motion across the window is a red flag.
 *
 * Usage (called from the main loop while a matched face is present):
 *   liveness_ctrl_begin();                       // start a fresh attempt
 *   ... each frame with a detected face ...
 *   liveness_ctrl_feed(keypoints, n);            // returns the running verdict
 *   ... until result != LIVENESS_PENDING or the caller times out ...
 */

typedef enum {
    LIVENESS_PENDING = 0,   // not enough evidence yet — keep feeding frames
    LIVENESS_PASS,          // pose challenge met + natural motion seen → live
    LIVENESS_FAIL_STATIC,   // geometry too rigid across the window → likely photo
    LIVENESS_FAIL_TIMEOUT,  // ran out of frames/time without meeting the challenge
} liveness_result_t;

/**
 * @brief Start a fresh liveness attempt. Clears all accumulated frame history.
 *        Call once when a recognized face first appears.
 */
void liveness_ctrl_begin(void);

/**
 * @brief Feed one frame's facial keypoints into the running check.
 *
 * @param keypoints Pointer to the detector's keypoint array, in the standard
 *                  5-point order: [Lx,Ly, Rx,Ry, Nx,Ny, MLx,MLy, MRx,MRy]
 *                  (L/R eye, Nose, mouth-Left, mouth-Right).
 * @param count     Number of ints in @p keypoints (expected 10). If the detector
 *                  ever returns fewer, the frame is ignored.
 * @return The running verdict. LIVENESS_PENDING means "need more frames".
 */
liveness_result_t liveness_ctrl_feed(const int *keypoints, int count);

/**
 * @brief Whether the current attempt has timed out based on frames fed.
 *        The caller can also enforce a wall-clock timeout itself.
 */
bool liveness_ctrl_is_timed_out(void);

#ifdef __cplusplus
}
#endif
