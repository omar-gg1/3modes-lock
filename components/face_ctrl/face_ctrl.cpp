#include "face_ctrl.h"

#include <cstring>
#include <cstdlib>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_camera.h"

#include "esp_jpeg_dec.h"
#include "human_face_detect.hpp"
#include "dl_image_define.hpp"

static const char *TAG = "face_ctrl";

// esp-dl detector instance. Heavy object, allocated once.
static HumanFaceDetect *s_detector = nullptr;

// Reusable RGB888 buffer in PSRAM. QVGA = 320*240*3 = 230400 bytes.
// Allocating once avoids fragmenting PSRAM on every detect call.
static uint8_t *s_rgb_buf = nullptr;
static const int FRAME_W = 320;
static const int FRAME_H = 240;
static const size_t RGB_BUF_SIZE = FRAME_W * FRAME_H * 3;

// JPEG decoder handle. Recreated per frame (esp_new_jpeg quirk).
// If this turns out to be slow we'll cache it.

extern "C" esp_err_t face_ctrl_init(void)
{
    if (s_detector != nullptr) {
        ESP_LOGW(TAG, "already initialized");
        return ESP_OK;
    }

    s_rgb_buf = (uint8_t *) jpeg_calloc_align(RGB_BUF_SIZE, 16);
    if (s_rgb_buf == nullptr) {
        ESP_LOGE(TAG, "failed to allocate %u bytes in PSRAM for RGB buffer", (unsigned) RGB_BUF_SIZE);
        return ESP_ERR_NO_MEM;
    }

    s_detector = new HumanFaceDetect();
    if (s_detector == nullptr) {
        ESP_LOGE(TAG, "failed to allocate HumanFaceDetect");
        heap_caps_free(s_rgb_buf);
        s_rgb_buf = nullptr;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "initialized. RGB buf @ %p, detector @ %p", s_rgb_buf, s_detector);
    ESP_LOGI(TAG, "free PSRAM after init: %u bytes", (unsigned) heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return ESP_OK;
}

// Decode a JPEG buffer into s_rgb_buf as RGB888.
// Returns ESP_OK on success.
static esp_err_t decode_jpeg_to_rgb(const uint8_t *jpeg_data, size_t jpeg_len)
{
    jpeg_dec_config_t cfg = {};
cfg.output_type = JPEG_PIXEL_FORMAT_RGB888;
cfg.rotate = JPEG_ROTATE_0D;

    jpeg_dec_handle_t dec = nullptr;
    jpeg_error_t err = jpeg_dec_open(&cfg, &dec);
    if (err != JPEG_ERR_OK || dec == nullptr) {
        ESP_LOGE(TAG, "jpeg_dec_open failed: %d", err);
        return ESP_FAIL;
    }

    jpeg_dec_io_t io = {};
io.inbuf = (uint8_t *) jpeg_data;
io.inbuf_len = (int) jpeg_len;
io.outbuf = s_rgb_buf;

    jpeg_dec_header_info_t header = {};
    err = jpeg_dec_parse_header(dec, &io, &header);
    if (err != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "jpeg_dec_parse_header failed: %d", err);
        jpeg_dec_close(dec);
        return ESP_FAIL;
    }

    if (header.width != FRAME_W || header.height != FRAME_H) {
        ESP_LOGW(TAG, "unexpected JPEG dims %dx%d, expected %dx%d",
                 header.width, header.height, FRAME_W, FRAME_H);
        // Not fatal but detection will be off. Bail for safety.
        jpeg_dec_close(dec);
        return ESP_FAIL;
    }

    err = jpeg_dec_process(dec, &io);
    if (err != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "jpeg_dec_process failed: %d", err);
        jpeg_dec_close(dec);
        return ESP_FAIL;
    }

    jpeg_dec_close(dec);
    return ESP_OK;
}

extern "C" bool face_ctrl_detect_once(void)
{
    if (s_detector == nullptr || s_rgb_buf == nullptr) {
        ESP_LOGE(TAG, "not initialized");
        return false;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == nullptr) {
        ESP_LOGW(TAG, "no frame from camera");
        return false;
    }
    if (fb->format != PIXFORMAT_JPEG) {
        ESP_LOGE(TAG, "expected JPEG format, got %d", fb->format);
        esp_camera_fb_return(fb);
        return false;
    }

    esp_err_t decode_err = decode_jpeg_to_rgb(fb->buf, fb->len);
    esp_camera_fb_return(fb);

    if (decode_err != ESP_OK) {
        return false;
    }

    // Build the esp-dl image descriptor.
    dl::image::img_t img = {};
    img.data = s_rgb_buf;
    img.width = FRAME_W;
    img.height = FRAME_H;
    img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888;

    auto &results = s_detector->run(img);
    if (results.empty()) {
        return false;
    }

    for (const auto &r : results) {
        // r.box is typically [x1, y1, x2, y2]. r.score is float 0..1.
        ESP_LOGI(TAG, "Face detected: bbox=[%d,%d,%d,%d] score=%.2f",
                 r.box[0], r.box[1], r.box[2], r.box[3],
                 r.score);
    }
    return true;
}

extern "C" void face_ctrl_deinit(void)
{
    if (s_detector) {
        delete s_detector;
        s_detector = nullptr;
    }
    if (s_rgb_buf) {
    jpeg_free_align(s_rgb_buf);
    s_rgb_buf = nullptr;
}
}