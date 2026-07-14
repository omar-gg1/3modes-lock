#include "qr_scan.h"

#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "img_converters.h"
#include "quirc.h"

static const char *TAG = "qr_scan";

// One quirc instance, lazily sized to the frame. Reused across scan iterations
// so we don't churn PSRAM every frame. The scan runs only in the armed
// PROVISION window, single-threaded from the main loop, so no locking needed.
static struct quirc *s_qr = NULL;
static int s_qr_w = 0, s_qr_h = 0;

// Decode the JPEG frame to grayscale straight into quirc's image buffer, then
// run the recognizer. Returns the decoded text length (0 = nothing found).
static int decode_to_text(camera_fb_t *fb, char *out, size_t out_cap)
{
    if (fb->format != PIXFORMAT_JPEG) return 0;

    if (s_qr == NULL) {
        s_qr = quirc_new();
        if (s_qr == NULL) { ESP_LOGE(TAG, "quirc_new failed"); return 0; }
    }
    if (fb->width != s_qr_w || fb->height != s_qr_h) {
        if (quirc_resize(s_qr, fb->width, fb->height) < 0) {
            ESP_LOGE(TAG, "quirc_resize %dx%d failed (heap?)", fb->width, fb->height);
            return 0;
        }
        s_qr_w = fb->width; s_qr_h = fb->height;
    }

    // RGB888 scratch in PSRAM (VGA ~900KB); freed each call — this path runs a
    // few times/sec only while provisioning, not in the face loop.
    uint8_t *rgb = heap_caps_malloc(fb->width * fb->height * 3, MALLOC_CAP_SPIRAM);
    if (rgb == NULL) { ESP_LOGE(TAG, "rgb alloc failed"); return 0; }
    bool ok = fmt2rgb888(fb->buf, fb->len, PIXFORMAT_JPEG, rgb);
    if (!ok) { free(rgb); return 0; }

    int qw, qh;
    uint8_t *gray = quirc_begin(s_qr, &qw, &qh);
    // Luma from RGB (BT.601 integer approx). quirc buffer is qw*qh bytes.
    for (int i = 0; i < qw * qh; i++) {
        const uint8_t *px = rgb + i * 3;
        gray[i] = (uint8_t)((px[0] * 77 + px[1] * 150 + px[2] * 29) >> 8);
    }
    free(rgb);
    quirc_end(s_qr);

    int count = quirc_count(s_qr);
    for (int i = 0; i < count; i++) {
        struct quirc_code code;
        struct quirc_data data;
        quirc_extract(s_qr, i, &code);
        if (quirc_decode(&code, &data) != QUIRC_SUCCESS) continue;
        size_t n = data.payload_len;
        if (n >= out_cap) n = out_cap - 1;
        memcpy(out, data.payload, n);
        out[n] = '\0';
        return (int) n;
    }
    return 0;
}

bool qr_scan_wifi(camera_fb_t *fb, char *ssid_out, char *pass_out)
{
    if (fb == NULL || ssid_out == NULL || pass_out == NULL) return false;
    char text[256];
    if (decode_to_text(fb, text, sizeof(text)) <= 0) return false;
    ESP_LOGI(TAG, "QR decoded (%d bytes)", (int) strlen(text));
    return qr_parse_wifi(text, ssid_out, WIFI_CREDS_SSID_MAX + 1,
                         pass_out, WIFI_CREDS_PASS_MAX + 1);
}
