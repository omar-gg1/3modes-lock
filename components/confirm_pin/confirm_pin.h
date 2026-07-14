#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * confirm_pin — the liveness-confirmation code (anti-spoof second factor).
 *
 * Typed on the keypad AFTER a face+liveness pass. A photo can fool the camera
 * but can't type the code, so this is the real spoof gate. Mirrors door_pin
 * (persistent NVS PIN, 4-8 digits, never blanks itself out) but adds an
 * enable/disable toggle so the owner can turn the whole requirement on or off
 * from the app.
 *
 * Both the code and the toggle are runtime + NVS-persisted, replacing the old
 * compile-time CONFIRM_PIN "0000" / CONFIRM_PIN_ENABLED 1. Set over MQTT
 * (set_confirm_pin / set_confirm_enabled, from the MQTT task); consulted by the
 * main loop's FACE_CONFIRM_PIN handler. Same short-critical-section spinlock as
 * door_pin / temp_pin.
 */

// Load code + toggle from NVS into RAM. Call once at boot after nvs_flash_init.
// Seeds the compiled factory defaults ("0000", enabled) when NVS holds nothing.
void confirm_pin_load(void);

// True if `entered` equals the live confirm code.
bool confirm_pin_matches(const char *entered);

// Set a new confirm code: validates 4-8 digits, persists to NVS, updates RAM.
// Returns false (and changes nothing) on an invalid pin — can't blank it out.
bool confirm_pin_set(const char *pin);

// True if the confirm-code requirement is currently enabled.
bool confirm_pin_enabled(void);

// Enable/disable the confirm-code requirement; persists to NVS + adopts in RAM.
void confirm_pin_set_enabled(bool on);

#ifdef __cplusplus
}
#endif
