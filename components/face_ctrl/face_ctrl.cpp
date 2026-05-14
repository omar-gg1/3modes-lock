#include "face_ctrl.h"

#include <cstring>
#include <cstdlib>
#include <list>
#include <vector>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_camera.h"

#include "esp_jpeg_dec.h"
#include "human_face_detect.hpp"
#include "human_face_recognition.hpp"
#include "dl_image_define.hpp"
#include "esp_spiffs.h"

static const char *TAG = "face_ctrl";

// esp-dl model instances.
static HumanFaceDetect *s_detector = nullptr;
static HumanFaceRecognizer *s_recognizer = nullptr;

// Stash the most recent detection results so enroll/recognize can use them.
static std::list<dl::detect::result_t> s_last_results;
static bool s_have_last_results = false;

// Reusable RGB888 buffer in PSRAM. QVGA = 320*240*3 = 230400 bytes.
static uint8_t *s_rgb_buf = nullptr;
static const int FRAME_W = 320;
static const int FRAME_H = 240;
static const size_t RGB_BUF_SIZE = FRAME_W * FRAME_H * 3;

// Path where the recognizer persists enrolled face features.
// Stored as a file in the spiffs "models" partition we already have.
// HumanFaceRecognizer handles the file format internally.
static const char *RECOGNIZER_DB_PATH = "/spiffs/faces.db";

// Mount the "models" SPIFFS partition at /spiffs.
// HumanFaceRecognizer will create its database file at /spiffs/faces.db.
static esp_err_t mount_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {};
    conf.base_path = "/spiffs";
    conf.partition_label = "models";
    conf.max_files = 5;
    conf.format_if_mount_failed = true;

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0, used = 0;
    if (esp_spiffs_info("models", &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS mounted: %u total, %u used", (unsigned) total, (unsigned) used);
    }
    return ESP_OK;
}

extern "C" bool face_ctrl_has_enrolled(void)
{
    if (s_recognizer == nullptr) {
        return false;
    }
    // HumanFaceRecognizer has a method to query enrolled face count.
    // The exact name may vary; if compiler complains, try:
    //   get_enrolled_id_num(), get_enroll_num(), or similar.
    return s_recognizer->get_num_feats() > 0;
}

extern "C" esp_err_t face_ctrl_init(void)
{
    if (s_detector != nullptr) {
        ESP_LOGW(TAG, "already initialized");
        return ESP_OK;
    }
    // Mount SPIFFS before HumanFaceRecognizer tries to open its db file.
    if (mount_spiffs() != ESP_OK) {
        return ESP_FAIL;
    }

    s_rgb_buf = (uint8_t *) jpeg_calloc_align(RGB_BUF_SIZE, 16);
    if (s_rgb_buf == nullptr) {
        ESP_LOGE(TAG, "failed to allocate %u bytes for RGB buffer",
                 (unsigned) RGB_BUF_SIZE);
        return ESP_ERR_NO_MEM;
    }

    s_detector = new HumanFaceDetect();
    if (s_detector == nullptr) {
        ESP_LOGE(TAG, "failed to allocate HumanFaceDetect");
        jpeg_free_align(s_rgb_buf);
        s_rgb_buf = nullptr;
        return ESP_ERR_NO_MEM;
    }

    s_recognizer = new HumanFaceRecognizer(RECOGNIZER_DB_PATH);
    if (s_recognizer == nullptr) {
        ESP_LOGE(TAG, "failed to allocate HumanFaceRecognizer");
        delete s_detector;
        s_detector = nullptr;
        jpeg_free_align(s_rgb_buf);
        s_rgb_buf = nullptr;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "initialized. RGB buf @ %p, detector @ %p, recognizer @ %p",
             s_rgb_buf, s_detector, s_recognizer);
    ESP_LOGI(TAG, "free PSRAM after init: %u bytes",
             (unsigned) heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return ESP_OK;
}

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

    s_have_last_results = false;

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
        ESP_LOGI(TAG, "Face detected: bbox=[%d,%d,%d,%d] score=%.2f",
                 r.box[0], r.box[1], r.box[2], r.box[3],
                 r.score);
    }

    s_last_results = results;
    s_have_last_results = true;
    return true;
}

// Build the dl::image::img_t descriptor for the current frame buffer.
// Helper to avoid duplication.
static dl::image::img_t build_img(void)
{
    dl::image::img_t img = {};
    img.data = s_rgb_buf;
    img.width = FRAME_W;
    img.height = FRAME_H;
    img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888;
    return img;
}

extern "C" esp_err_t face_ctrl_enroll(int id)
{
    if (s_recognizer == nullptr) {
        ESP_LOGE(TAG, "not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_have_last_results) {
        ESP_LOGW(TAG, "no recent detection to enroll");
        return ESP_ERR_INVALID_STATE;
    }

    dl::image::img_t img = build_img();
    // enroll() takes the image and detection results.
    // It internally extracts features and stores them with the given id.
    s_recognizer->enroll(img, s_last_results);

    ESP_LOGI(TAG, "enrolled face as id=%d", id);
    return ESP_OK;
}

extern "C" esp_err_t face_ctrl_recognize(int *out_id, float *out_similarity)
{
    if (s_recognizer == nullptr) {
        ESP_LOGE(TAG, "not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (out_id == nullptr || out_similarity == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_have_last_results) {
        return ESP_ERR_NOT_FOUND;
    }

    dl::image::img_t img = build_img();
    auto results = s_recognizer->recognize(img, s_last_results);

    if (results.empty()) {
        return ESP_ERR_NOT_FOUND;
    }

    // We only care about the first detected face.
    auto &top = results.front();
    *out_id = top.id;
    *out_similarity = top.similarity;
    return ESP_OK;
}

extern "C" void face_ctrl_deinit(void)
{
    if (s_recognizer) {
        delete s_recognizer;
        s_recognizer = nullptr;
    }
    if (s_detector) {
        delete s_detector;
        s_detector = nullptr;
    }
    if (s_rgb_buf) {
        jpeg_free_align(s_rgb_buf);
        s_rgb_buf = nullptr;
    }
}