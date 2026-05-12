#pragma once
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