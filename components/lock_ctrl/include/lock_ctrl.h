#pragma once
#include "esp_err.h"

/**
 * Initialize lock GPIO. Lock starts in LOCKED state.
 * Call once at boot.
 */
esp_err_t lock_ctrl_init(void);

/**
 * Trigger an unlock. Lock opens for UNLOCK_DURATION_MS then auto-relocks.
 * Safe to call concurrently — debounced via internal flag.
 * @param reason Human-readable trigger source (logged). e.g. "BOOT button", "face match user_0"
 */
void lock_ctrl_trigger_unlock(const char *reason);