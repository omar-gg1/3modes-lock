#pragma once
#include <stdbool.h>
#include "esp_err.h"

/**
 * Initialize Wi-Fi in STA mode and connect to the given network.
 * BLOCKS until connected, timeout reached, or max retries exhausted.
 * @param ssid Network name
 * @param password Network password (WPA2)
 * @param timeout_ms Maximum time to wait for connection
 * @return ESP_OK if connected, ESP_FAIL otherwise
 */
esp_err_t wifi_ctrl_connect(const char *ssid, const char *password, uint32_t timeout_ms);

/**
 * Switch to a new network at RUNTIME without re-initialising the stack.
 * If the stack is already up (wifi_ctrl_connect ran at boot) this reconfigures
 * and reconnects in place; otherwise it falls back to wifi_ctrl_connect() so a
 * first-ever provision still works when Mode 2 was off at boot.
 * BLOCKS until connected or timeout.
 * @return ESP_OK if connected, ESP_FAIL otherwise.
 */
esp_err_t wifi_ctrl_reconnect(const char *ssid, const char *password, uint32_t timeout_ms);

/**
 * Report the live association state. ssid_out (>=33 bytes) receives the SSID of
 * the currently-associated AP, or "" if not connected. Either arg may be NULL.
 * @return true if connected (got IP / associated), false otherwise.
 */
bool wifi_ctrl_status(char *ssid_out, size_t ssid_cap);