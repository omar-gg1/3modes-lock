#include "camera_ctrl.h"
#include "esp_log.h"

// Camera pin map — Gooouuu Tech ESP32-S3-CAM (OV3660)
#define CAM_PIN_PWDN    -1
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK    15
#define CAM_PIN_SIOD    4
#define CAM_PIN_SIOC    5
#define CAM_PIN_D7      16
#define CAM_PIN_D6      17
#define CAM_PIN_D5      18
#define CAM_PIN_D4      12
#define CAM_PIN_D3      10
#define CAM_PIN_D2      8
#define CAM_PIN_D1      9
#define CAM_PIN_D0      11
#define CAM_PIN_VSYNC   6
#define CAM_PIN_HREF    7
#define CAM_PIN_PCLK    13

static const char *TAG = "camera_ctrl";

esp_err_t camera_ctrl_init(void) {
    camera_config_t config = {
        .pin_pwdn = CAM_PIN_PWDN,
        .pin_reset = CAM_PIN_RESET,
        .pin_xclk = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d7 = CAM_PIN_D7,
        .pin_d6 = CAM_PIN_D6,
        .pin_d5 = CAM_PIN_D5,
        .pin_d4 = CAM_PIN_D4,
        .pin_d3 = CAM_PIN_D3,
        .pin_d2 = CAM_PIN_D2,
        .pin_d1 = CAM_PIN_D1,
        .pin_d0 = CAM_PIN_D0,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href = CAM_PIN_HREF,
        .pin_pclk = CAM_PIN_PCLK,
        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = FRAMESIZE_QVGA,    // 320x240 — matches the recognizer input
        // jpeg_quality: LOWER number = BETTER quality (less compression). 10 gives
        // the recognizer cleaner pixels than the old 15, which raises and steadies
        // similarity scores. The recognizer is the consumer, not a human eye, so
        // we trade a little bandwidth for fidelity.
        .jpeg_quality = 10,
        // fb_count = 2: double-buffering. With 1 buffer the camera DMA and the
        // JPEG decoder fight over the same memory — the decoder reads a frame the
        // camera is mid-writing, giving corrupt JPEGs (NO-SOI / jpeg_dec -3) and
        // occasional crashes. With 3+ buffers stale frames pile up (FB-OVF). Two
        // buffers + GRAB_LATEST is the sweet spot: one fills while one is read,
        // and we always get the freshest completed frame.
        .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init FAILED with error 0x%x", err);
        return err;
    }

    // Fix upside-down sensor mounting on this clone
    sensor_t *s = esp_camera_sensor_get();
    if (s != NULL) {
        s->set_vflip(s, 1);
        s->set_hmirror(s, 1);
        ESP_LOGI(TAG, "Sensor orientation: vflip=1, hmirror=1");
    }

    ESP_LOGI(TAG, "Camera initialized (QVGA JPEG)");
    return ESP_OK;
}

camera_fb_t *camera_ctrl_get_frame(void) {
    return esp_camera_fb_get();
}

void camera_ctrl_return_frame(camera_fb_t *fb) {
    if (fb) esp_camera_fb_return(fb);
}