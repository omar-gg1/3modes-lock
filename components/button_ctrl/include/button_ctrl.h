#pragma once
#include "esp_err.h"
#include <stdbool.h>

/**
 * Initialize BOOT button (GPIO 0) as input with pull-up.
 */
esp_err_t button_ctrl_init(void);

/**
 * Poll button for a short press. Returns true exactly once per press,
 * on release, IF the press was shorter than the long-press threshold.
 * Long presses do NOT count as short presses.
 * Call from your main loop every ~50ms.
 */
bool button_ctrl_was_pressed(void);

/**
 * Poll button for a long press. Returns true exactly once per hold,
 * the moment the hold duration crosses the threshold.
 * After firing, won't fire again until button is released and re-held.
 * Call from your main loop every ~50ms.
 */
bool button_ctrl_long_press_fired(void);