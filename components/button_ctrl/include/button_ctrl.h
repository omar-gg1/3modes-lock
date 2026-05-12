#pragma once
#include "esp_err.h"
#include <stdbool.h>

/**
 * Initialize BOOT button (GPIO 0) as input with pull-up.
 */
esp_err_t button_ctrl_init(void);

/**
 * Poll button. Returns true exactly once per press (edge-detected).
 * Call from your main loop every ~50ms.
 */
bool button_ctrl_was_pressed(void);