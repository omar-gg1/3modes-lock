#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * door_pin — the persistent household unlock PIN.
 *
 * Unlike temp_pin (OTP-style, expires, single-use), this one persists in NVS,
 * survives reboots, never expires, and never one-shots. It replaces the old
 * compile-time UNLOCK_PIN "1234", which now serves only as the factory default
 * fed in at first boot when NVS holds nothing.
 *
 * Set over MQTT (set_door_pin command, from the MQTT task); consulted by the
 * main loop's keypad handler. Same short-critical-section spinlock pattern as
 * temp_pin / enroll_request.
 */

// Load the door PIN from NVS into RAM. Call once at boot after nvs_flash_init.
// If NVS holds nothing, seeds the RAM copy with the compiled factory default.
void door_pin_load(void);

// True if `entered` equals the live door PIN.
bool door_pin_matches(const char *entered);

// Set a new door PIN: validates 4-8 digits, persists to NVS, updates the RAM
// copy. Returns false (and changes nothing) on an invalid pin, so the owner can
// never blank it out and lock themselves out.
bool door_pin_set(const char *pin);

#ifdef __cplusplus
}
#endif
