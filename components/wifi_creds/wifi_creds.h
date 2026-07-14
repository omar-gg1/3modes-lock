#pragma once
#include <stdbool.h>
#include <stddef.h>

// 802.11 limits: SSID up to 32 bytes, WPA2 passphrase up to 63.
#define WIFI_CREDS_SSID_MAX 32
#define WIFI_CREDS_PASS_MAX 63

// Runtime, NVS-persisted WiFi credentials. Replaces the compile-time
// WIFI_SSID/WIFI_PASSWORD constants so the network can be re-provisioned at
// runtime (QR scan). Mirrors the door_pin/confirm_pin NVS pattern.

// Load SSID+password from NVS into RAM; keeps the wifi_config.h factory
// defaults if NVS is empty (first boot). Call once at startup.
void wifi_creds_load(void);

// Copy the current credentials out. Buffers must be WIFI_CREDS_SSID_MAX+1 and
// WIFI_CREDS_PASS_MAX+1. Either out pointer may be NULL to skip it.
void wifi_creds_get(char *ssid_out, char *pass_out);

// Validate (non-empty SSID within limits; empty password allowed for open
// networks), persist-then-adopt. Returns false on invalid input or NVS failure
// (leaving the existing credentials untouched).
bool wifi_creds_set(const char *ssid, const char *pass);
