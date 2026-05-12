#pragma once
#include "esp_err.h"
#include "esp_camera.h"

/**
 * Initialize camera with pin map for Gooouuu Tech ESP32-S3-CAM (OV3660).
 * Vertical flip applied to correct upside-down sensor mounting.
 * Resolution: QVGA (320x240) JPEG for fast streaming.
 */
esp_err_t camera_ctrl_init(void);

/**
 * Get a camera frame. Caller MUST call camera_ctrl_return_frame() when done.
 */
camera_fb_t *camera_ctrl_get_frame(void);

/**
 * Return a frame buffer back to the driver. ALWAYS call after get_frame.
 */
void camera_ctrl_return_frame(camera_fb_t *fb);