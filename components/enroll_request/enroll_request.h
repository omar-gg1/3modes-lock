#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Post an append-enroll request from the MQTT task. Overwrites any
 *        request not yet taken (last-writer-wins; a fresh enroll intent
 *        supersedes a stale one).
 */
void enroll_request_set(int user_id, int samples);

/**
 * @brief Atomically read-and-clear a pending request. Call from the main loop.
 * @return true if a request was pending (out params filled), false otherwise.
 */
bool enroll_request_take(int *user_id, int *samples);

/**
 * @brief Post a "start WiFi QR scan" request from the MQTT task. Single-slot
 *        latch, same as the enroll request above.
 */
void wifi_scan_request_set(void);

/**
 * @brief Atomically read-and-clear a pending WiFi-scan request. Main loop only.
 * @return true if a scan was requested since the last take.
 */
bool wifi_scan_request_take(void);

#ifdef __cplusplus
}
#endif
