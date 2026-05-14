#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_camera.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize face detection pipeline.
 *        Allocates JPEG decoder + HumanFaceDetect model.
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
 * @brief Cleanup. Not strictly needed but exposed for completeness.
 */
void face_ctrl_deinit(void);

#ifdef __cplusplus
}
#endif