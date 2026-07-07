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

// The camera runs QVGA-only, permanently. Runtime resolution switching for the
// Mode 3 cloud grab was tried twice and abandoned:
//   1. sensor->set_framesize(VGA): cam_hal's frame buffers are sized ONCE at
//      esp_camera_init() (QVGA -> 15360 B/buffer), so VGA JPEGs (40-80KB at
//      q10) truncated into them -> FB-OVF floods + corrupt frames the cloud
//      called "no_face".
//   2. Full esp_camera_deinit()/init() at VGA: correct buffers, but ~2s of
//      camera teardown per cloud call, and by then the user had moved anyway.
// The real fix lives in face_ctrl: it RETAINS the JPEG of the frame the local
// recognizer actually scored, and cloud_verify sends that — same scene, no
// reconfig. A QVGA face crop (~110px) is already ArcFace's native input size.

static void apply_orientation_fix(void) {
    sensor_t *s = esp_camera_sensor_get();
    if (s != NULL) {
        s->set_vflip(s, 1);
        s->set_hmirror(s, 1);
    }
}

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
        // VGA (640x480), captured permanently. Two consumers, decoupled:
        //   - LOCAL recognizer: face_ctrl downscales this VGA JPEG to its
        //     320x240 working buffer, so the local pipeline is UNCHANGED (same
        //     scores, no re-enrollment) — it just feeds off a bigger source.
        //   - Mode 3 CLOUD verify: gets the full VGA JPEG (retained by
        //     face_ctrl), giving ArcFace's detector a ~200px face it can lock
        //     onto (QVGA's ~90px face was below its detection floor).
        // No runtime resolution switching (that thrashed the camera); one size.
        .frame_size = FRAMESIZE_VGA,
        // Slightly less compression than QVGA's 10: a VGA frame has the bandwidth
        // budget, and more detail helps the cloud detector. Lower = better.
        .jpeg_quality = 12,
        .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init FAILED with error 0x%x", err);
        return err;
    }

    apply_orientation_fix();
    ESP_LOGI(TAG, "Sensor orientation: vflip=1, hmirror=1");
    ESP_LOGI(TAG, "Camera initialized (VGA JPEG)");
    return ESP_OK;
}

camera_fb_t *camera_ctrl_get_frame(void) {
    return esp_camera_fb_get();
}

void camera_ctrl_return_frame(camera_fb_t *fb) {
    if (fb) esp_camera_fb_return(fb);
}

