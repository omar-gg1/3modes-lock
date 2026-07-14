#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * mode_ctrl — the runtime operating mode (1=Local, 2=Hybrid, 3=Cloud-Assisted).
 *
 * Replaces the compile-time MODE2_REPORTING_ENABLED / MODE3_ENABLED #defines.
 * Modes are cumulative (3 > 2 > 1): the main loop derives two behaviors from the
 * one integer — report to cloud (mode>=2) and cloud-verify murky scores (mode==3).
 * NVS-persisted; set over MQTT (set_mode, from the MQTT task); read every main
 * loop iteration. Same short-critical-section spinlock as door_pin / confirm_pin.
 */

// Load the mode from NVS into RAM, seeding the factory default (3) on first boot.
// Call once at boot after nvs_flash_init.
void mode_ctrl_load(void);

// Current mode, 1..3. Spinlock-guarded (MQTT task writes, main loop reads).
uint8_t mode_ctrl_get(void);

// Set a new mode: validates 1..3, persists to NVS, adopts in RAM. Returns false
// (and changes nothing) on an out-of-range value or an NVS write failure.
bool mode_ctrl_set(uint8_t m);

// Boot self-test: exercises set/get across 1..3 + an out-of-range reject, then
// restores the pre-test mode. Logs PASS/FAIL. Returns true on PASS.
bool mode_ctrl_selftest(void);

#ifdef __cplusplus
}
#endif
