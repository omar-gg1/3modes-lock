#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the 4x4 matrix keypad.
 *        Rows on GPIO 38-41 (outputs), columns on GPIO 42, 47, 1, 2 (inputs).
 */
esp_err_t keypad_ctrl_init(void);

/**
 * @brief Poll the keypad for a key press.
 *        Returns the character of the pressed key on a fresh press (edge-detected).
 *        Returns 0 ('\0') if no new key press this poll cycle.
 *        Designed to be called from the main loop every ~20-50ms.
 *
 *        Keypad layout:
 *            1  2  3  A
 *            4  5  6  B
 *            7  8  9  C
 *            *  0  #  D
 */
char keypad_ctrl_scan(void);

/**
 * @brief Diagnostic: drives all rows LOW and dumps raw column states.
 *        Used to verify wiring. Call periodically from main loop while debugging.
 */
void keypad_ctrl_debug_dump(void);

#ifdef __cplusplus
}
#endif