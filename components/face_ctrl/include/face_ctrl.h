#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_camera.h"

#ifdef __cplusplus
extern "C" {
#endif
/**
 * @brief Check whether anyone is currently enrolled in the recognizer.
 *        Used to gate auto-enrollment on first boot.
 *
 * @return true if at least one face is enrolled.
 */
bool face_ctrl_has_enrolled(void);
/**
 * @brief Initialize face detection + recognition pipeline.
 *        Allocates JPEG decoder, HumanFaceDetect, and HumanFaceRecognizer.
 *        Must be called after camera_ctrl_init().
 */
esp_err_t face_ctrl_init(void);

/**
 * @brief Grab one frame from camera, decode, run detection.
 *        Logs bbox + score to serial if face found.
 *        Designed to be called from main loop, throttled by caller.
 *
 * @return true if at least one face was detected this frame.
 */
bool face_ctrl_detect_once(void);

/**
 * @brief Enroll the most recently detected face under the given numeric ID.
 *        Must be called immediately after face_ctrl_detect_once() returned true.
 *        Persists features to flash; survives reboots.
 *
 * @param id Numeric ID to associate with this face (e.g., 0 for primary user).
 * @return ESP_OK on success.
 */
esp_err_t face_ctrl_enroll(int id);

/**
 * @brief Robust multi-sample enrollment. Captures several quality-gated
 *        templates for one person, each from a fresh frame, and stores them all.
 *        Storing multiple feature vectors per user is what makes later
 *        recognition score high and consistent (a match need only be close to
 *        one stored look). Blocks while capturing (~samples * 200ms).
 *
 * @param id             Numeric ID for this person.
 * @param samples_wanted How many templates to capture (e.g. 5).
 * @param timeout_ms     Give up after this long if good frames don't appear.
 * @return ESP_OK if at least one template was stored,
 *         ESP_ERR_NOT_FOUND if no good face was seen in time.
 */
esp_err_t face_ctrl_enroll_multi(int id, int samples_wanted, int timeout_ms);

/**
 * @brief Whether the most recent detection is good enough to recognize/enroll
 *        from (big enough + confident enough). Gate recognition on this to keep
 *        similarity scores high. Call after face_ctrl_detect_once().
 */
bool face_ctrl_last_is_good_quality(void);

/**
 * @brief Why the last face_ctrl_detect_once() returned false.
 *        0=face found, 1=no frame from camera, 2=bad/corrupt JPEG, 3=no face in
 *        a good frame. Lets callers log the real reason during liveness/scanning.
 */
int face_ctrl_last_detect_fail(void);

/**
 * @brief Check whether the most recently detected face matches an enrolled one.
 *        Must be called immediately after face_ctrl_detect_once() returned true.
 *
 * @param out_id          Output: the matched enrollment ID.
 * @param out_similarity  Output: cosine similarity score (0..1, higher = closer match).
 * @return ESP_OK if a match was found,
 *         ESP_ERR_NOT_FOUND if no face or no enrolled match,
 *         other errors on failure.
 */
esp_err_t face_ctrl_recognize(int *out_id, float *out_similarity);

/**
 * @brief Get the 5-point facial keypoints of the most recently detected face.
 *        Must be called after face_ctrl_detect_once() returned true.
 *
 *        Order (10 ints): [Lx,Ly, Rx,Ry, Nx,Ny, MLx,MLy, MRx,MRy]
 *        = left eye, right eye, nose, mouth-left, mouth-right.
 *        Used by the liveness check to measure head pose + micro-motion.
 *
 * @param out_keypoints Caller-provided buffer of at least @p max ints.
 * @param max           Capacity of @p out_keypoints.
 * @param out_count     Receives the number of ints written (typically 10).
 * @return ESP_OK on success,
 *         ESP_ERR_INVALID_STATE if there's no recent detection,
 *         ESP_ERR_NOT_FOUND if the detection carried no keypoints.
 */
esp_err_t face_ctrl_get_keypoints(int *out_keypoints, int max, int *out_count);

/**
 * @brief Cleanup. Not strictly needed but exposed for completeness.
 */
void face_ctrl_deinit(void);

#ifdef __cplusplus
}
#endif