#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * temp_pin — a single OTP-style guest PIN.
 *
 * Set over MQTT (set_temp_pin command, from the MQTT task); consulted by the
 * main loop's keypad handler. Dies on FIRST successful use OR on timeout,
 * whichever comes first — separate from the persistent UNLOCK_PIN.
 *
 * Single slot: one live guest PIN at a time (a guest lock needs no more).
 * Same short-critical-section spinlock pattern as enroll_request.
 */

// Arm a temp PIN valid for ttl_s seconds from now. Empty/NULL pin clears the
// slot (revoke). Overwrites any existing temp PIN.
void temp_pin_set(const char *pin, int ttl_s);

// If `entered` matches the live, unexpired, unused temp PIN, mark it used and
// return true (the caller should unlock). Otherwise false. A match that is
// expired is treated as a miss (and the stale slot is cleared).
bool temp_pin_try(const char *entered);

#ifdef __cplusplus
}
#endif
