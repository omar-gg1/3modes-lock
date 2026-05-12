#pragma once
#include "esp_err.h"

/**
 * Start the HTTP debug server on port 80.
 * Routes:
 *   GET /          → HTML page with live stream
 *   GET /stream    → MJPEG live stream
 *   GET /capture   → single JPEG snapshot
 *
 * Requires camera_ctrl to be initialized first.
 * Optional — comment out the call site to remove the debug interface.
 */
esp_err_t debug_server_start(void);