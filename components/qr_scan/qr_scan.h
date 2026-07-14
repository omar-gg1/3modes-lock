#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_camera.h"
#include "wifi_creds.h"
#include "qr_parse.h"

// Decode a QR code out of a camera frame and, if it is a WIFI: payload, extract
// the SSID and password. The camera stays in its normal JPEG/VGA config; this
// decodes the JPEG to grayscale in software and hands it to quirc.
//
// Returns true only when a QR was found AND parsed as a WIFI: credential.
// ssid_out must be WIFI_CREDS_SSID_MAX+1, pass_out WIFI_CREDS_PASS_MAX+1.
bool qr_scan_wifi(camera_fb_t *fb, char *ssid_out, char *pass_out);
