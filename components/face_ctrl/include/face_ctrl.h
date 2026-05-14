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
 * @brief Cleanup. Not strictly needed but exposed for completeness.
 */
void face_ctrl_deinit(void);

#ifdef __cplusplus
}
#endif