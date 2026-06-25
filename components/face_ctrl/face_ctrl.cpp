#include "face_ctrl.h"

#include <cstring>
#include <cstdlib>
#include <list>
#include <vector>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_camera.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_jpeg_dec.h"
#include "human_face_detect.hpp"
#include "human_face_recognition.hpp"
#include "dl_image_define.hpp"
#include "esp_spiffs.h"
#include "crypto_ctrl.h"

static const char *TAG = "face_ctrl";

// esp-dl model instances.
static HumanFaceDetect *s_detector = nullptr;
static HumanFaceRecognizer *s_recognizer = nullptr;

// Stash the most recent detection results so enroll/recognize can use them.
static std::list<dl::detect::result_t> s_last_results;
static bool s_have_last_results = false;

// Why did the last detect_once() return false? Lets callers (and the enrollment
// diagnostics) tell apart "camera/decode failed" from "frame was fine but no
// face in it" — two very different problems with different fixes.
typedef enum {
    DETECT_FAIL_NONE = 0,    // a face WAS found
    DETECT_FAIL_NO_FRAME,    // camera returned null
    DETECT_FAIL_BAD_JPEG,    // frame was corrupt / failed to decode
    DETECT_FAIL_NO_FACE,     // frame decoded fine, detector found nothing
} detect_fail_t;
static detect_fail_t s_last_detect_fail = DETECT_FAIL_NONE;

// ---- Feature-ID -> User-ID mapping ----
// The esp-dl recognizer assigns every enrolled FEATURE its own sequential id
// (1,2,3,...) and has NO notion of "these N features are the same person". So
// when we store 5 templates for one user, the recognizer sees ids 1..5 and
// recognition returns whichever template matched — giving a meaningless,
// changing id for the same face. We fix that here: map each feature id to the
// real user id it was enrolled under. s_feat_user_map[feature_id] = user_id.
// Index 0 is unused (recognizer ids start at 1). Persisted next to the DB so it
// survives reboots, and encrypted along with everything else at rest.
#define MAX_FEATURES 64
static int16_t s_feat_user_map[MAX_FEATURES + 1];
static int     s_feat_count = 0;   // number of features enrolled so far
static const char *FEATMAP_PATH     = "/spiffs/featmap.bin";
static const char *FEATMAP_ENC_PATH = "/spiffs/featmap.bin.enc";

static void featmap_reset(void)
{
    for (int i = 0; i <= MAX_FEATURES; i++) s_feat_user_map[i] = -1;
    s_feat_count = 0;
}

// Persist the map (plaintext temp -> encrypted at rest), mirroring how the face
// DB itself is handled.
static void featmap_save(void)
{
    FILE *f = fopen(FEATMAP_PATH, "wb");
    if (!f) {
        ESP_LOGE(TAG, "featmap: cannot open for write");
        return;
    }
    fwrite(&s_feat_count, sizeof(s_feat_count), 1, f);
    fwrite(s_feat_user_map, sizeof(int16_t), MAX_FEATURES + 1, f);
    fclose(f);
    crypto_ctrl_encrypt_file(FEATMAP_PATH, FEATMAP_ENC_PATH);
}

// Load the map from the encrypted companion file. Resets to empty if missing or
// if authentication fails (mirrors the face-DB tamper handling).
static void featmap_load(void)
{
    featmap_reset();
    esp_err_t dec = crypto_ctrl_decrypt_file(FEATMAP_ENC_PATH, FEATMAP_PATH);
    if (dec != ESP_OK) {
        if (dec != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "featmap failed to decrypt (%s) — starting empty",
                     esp_err_to_name(dec));
        }
        return;
    }
    FILE *f = fopen(FEATMAP_PATH, "rb");
    if (!f) return;
    if (fread(&s_feat_count, sizeof(s_feat_count), 1, f) != 1) {
        s_feat_count = 0;
    }
    if (fread(s_feat_user_map, sizeof(int16_t), MAX_FEATURES + 1, f) != (size_t)(MAX_FEATURES + 1)) {
        featmap_reset();
    }
    fclose(f);
    ESP_LOGI(TAG, "featmap loaded: %d features mapped", s_feat_count);
}

// Reusable RGB888 buffer in PSRAM. QVGA = 320*240*3 = 230400 bytes.
static uint8_t *s_rgb_buf = nullptr;
static const int FRAME_W = 320;
static const int FRAME_H = 240;
static const size_t RGB_BUF_SIZE = FRAME_W * FRAME_H * 3;

// ---- Recognition quality gate (CALIBRATE with real data) ----
// Only recognize/enroll from detections that clear these bars. Gating out small
// or low-confidence faces is what keeps similarity scores high and consistent.
// Enrollment uses a slightly RELAXED bar: the user is cooperating and holding
// still, and we'd rather gather all the samples than reject good-enough frames
// and time out (which is what happened on the first hardware test).
static const float FACE_MIN_DETECT_SCORE      = 0.80f;  // live recognition
static const float FACE_MIN_BOX_FRAC          = 0.25f;
static const float FACE_ENROLL_MIN_SCORE      = 0.70f;  // enrollment (relaxed)
static const float FACE_ENROLL_MIN_BOX_FRAC   = 0.18f;

// Pace the enrollment capture loop. 50ms = ~20 attempts/sec: enough to gather
// samples quickly, but not the 30ms hammer that out-ran the camera, nor the
// 120ms pace that starved detection (0/5). Tune from the diagnostic counts.
#define ENROLL_FRAME_INTERVAL_MS 50

// Shared quality check, parameterized so live recognition and enrollment can use
// different strictness.
static bool last_detection_passes(float min_score, float min_box_frac)
{
    if (!s_have_last_results || s_last_results.empty()) {
        return false;
    }
    const auto &f = s_last_results.front();
    if (f.score < min_score) {
        return false;
    }
    int w = f.box[2] - f.box[0];
    int h = f.box[3] - f.box[1];
    if (w < (FRAME_W * min_box_frac) || h < (FRAME_H * min_box_frac)) {
        return false;
    }
    return true;
}

// The recognizer reads/writes a plaintext working file (it does raw fopen/fread
// and has no encryption hook). We keep that working file as the in-use DB, and
// keep the *encrypted* copy as the at-rest artifact:
//
//   RECOGNIZER_DB_PATH      <- plaintext, what HumanFaceRecognizer opens
//   RECOGNIZER_DB_ENC_PATH  <- AES-256-GCM ciphertext, what survives at rest
//
// On init we decrypt .enc -> plaintext; after each enroll we re-encrypt
// plaintext -> .enc. See crypto_ctrl.h for the container format. The brief
// existence of the plaintext working file is the documented tradeoff of doing
// this above esp-dl; the eFuse/flash-encryption hardening stage covers it.
static const char *RECOGNIZER_DB_PATH     = "/spiffs/faces.db";
static const char *RECOGNIZER_DB_ENC_PATH = "/spiffs/faces.db.enc";

// Re-encrypt the plaintext working DB back to the at-rest ciphertext. Called
// after any operation that mutates the DB (enroll). Logs but does not abort the
// caller on failure — the in-memory/plaintext state is still valid for this
// session; we just failed to persist the encrypted copy.
static void persist_encrypted_db(void)
{
    esp_err_t err = crypto_ctrl_encrypt_file(RECOGNIZER_DB_PATH, RECOGNIZER_DB_ENC_PATH);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to re-encrypt face DB: %s", esp_err_to_name(err));
    }
}

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

    // Restore the plaintext working DB from the encrypted copy at rest, so the
    // recognizer (which only understands a plaintext file) has something to open.
    //   - ESP_OK              : decrypted an existing DB, ready to use.
    //   - ESP_ERR_NOT_FOUND   : no encrypted DB yet (first ever boot) — fine,
    //                           the recognizer will create an empty one.
    //   - ESP_ERR_INVALID_CRC : the encrypted DB failed authentication (tampered
    //                           or key changed). We refuse to fall back to a
    //                           stale plaintext file: delete it so the recognizer
    //                           starts clean rather than trusting unverified data.
    esp_err_t dec = crypto_ctrl_decrypt_file(RECOGNIZER_DB_ENC_PATH, RECOGNIZER_DB_PATH);
    if (dec == ESP_OK) {
        ESP_LOGI(TAG, "face DB decrypted from %s", RECOGNIZER_DB_ENC_PATH);
    } else if (dec == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "no encrypted face DB yet — starting fresh");
        remove(RECOGNIZER_DB_PATH);  // ensure no stale plaintext lingers
    } else {
        ESP_LOGE(TAG, "face DB failed authentication (%s) — discarding",
                 esp_err_to_name(dec));
        remove(RECOGNIZER_DB_PATH);
    }

    // Load the feature-id -> user-id map that accompanies the DB.
    featmap_load();

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
    s_last_detect_fail = DETECT_FAIL_NONE;

    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == nullptr) {
        s_last_detect_fail = DETECT_FAIL_NO_FRAME;
        return false;
    }
    if (fb->format != PIXFORMAT_JPEG) {
        ESP_LOGE(TAG, "expected JPEG format, got %d", fb->format);
        esp_camera_fb_return(fb);
        s_last_detect_fail = DETECT_FAIL_BAD_JPEG;
        return false;
    }

    esp_err_t decode_err = decode_jpeg_to_rgb(fb->buf, fb->len);
    esp_camera_fb_return(fb);

    if (decode_err != ESP_OK) {
        s_last_detect_fail = DETECT_FAIL_BAD_JPEG;
        return false;
    }

    dl::image::img_t img = {};
    img.data = s_rgb_buf;
    img.width = FRAME_W;
    img.height = FRAME_H;
    img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888;

    auto &results = s_detector->run(img);
    if (results.empty()) {
        s_last_detect_fail = DETECT_FAIL_NO_FACE;
        return false;
    }

    // Per-frame detection detail is DEBUG-level: at INFO it floods the terminal
    // (one line per frame) and buries the events that matter — match, liveness
    // pass, unlock. Raise the log level to see these again if needed.
    for (const auto &r : results) {
        ESP_LOGD(TAG, "Face detected: bbox=[%d,%d,%d,%d] score=%.2f",
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
    // It internally extracts features and writes them to the plaintext working
    // DB at RECOGNIZER_DB_PATH.
    s_recognizer->enroll(img, s_last_results);

    // The plaintext DB just changed on flash — refresh the encrypted copy so the
    // at-rest artifact stays in sync and survives the next reboot.
    persist_encrypted_db();

    ESP_LOGI(TAG, "enrolled face as id=%d", id);
    return ESP_OK;
}

extern "C" esp_err_t face_ctrl_enroll_multi(int id, int samples_wanted, int timeout_ms)
{
    if (s_recognizer == nullptr) {
        ESP_LOGE(TAG, "not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (samples_wanted < 1) samples_wanted = 1;

    // Capture several QUALITY-GATED templates for one person, each from a fresh
    // frame. Storing multiple feature vectors per user is the core robustness
    // fix: a later match only needs to be close to ONE stored look, so scores
    // rise and stop swinging with small angle/lighting changes. The recognizer
    // appends each enroll() as another feature and returns the best match.
    // Clear any existing templates first. Without this, re-enrolling APPENDS to
    // the old DB, leaving stale/bad templates (e.g. a previous single-sample
    // enrollment) mixed in to pollute matching. A fresh enroll = a clean DB.
    // NOTE: this is single-user behaviour. Multi-user enrollment will later
    // delete only the target user's features instead of clearing everything.
    s_recognizer->clear_all_feats();
    featmap_reset();  // feature ids restart at 1 after a clear

    int captured = 0;
    int64_t start_us = esp_timer_get_time();

    ESP_LOGI(TAG, "multi-enroll id=%d: want %d samples (hold still)...",
             id, samples_wanted);

    int bad_jpeg = 0, no_face = 0, no_frame = 0, gate_rejects = 0, iters = 0;

    while (captured < samples_wanted) {
        if ((esp_timer_get_time() - start_us) / 1000 > timeout_ms) {
            ESP_LOGW(TAG, "multi-enroll timed out with %d/%d samples "
                     "(iters=%d, bad_jpeg=%d, no_face=%d, no_frame=%d, gate_rejects=%d)",
                     captured, samples_wanted, iters,
                     bad_jpeg, no_face, no_frame, gate_rejects);
            break;
        }
        iters++;

        // Modest pace so we don't hammer the camera, but short enough to get many
        // attempts in the window. (A longer pace starved detection -> 0/5.)
        vTaskDelay(pdMS_TO_TICKS(ENROLL_FRAME_INTERVAL_MS));

        // Fresh frame + detection. Break down WHY it failed so we can fix the
        // right thing (corrupt frames vs genuinely no face vs no frame).
        if (!face_ctrl_detect_once()) {
            switch (s_last_detect_fail) {
                case DETECT_FAIL_BAD_JPEG: bad_jpeg++; break;
                case DETECT_FAIL_NO_FRAME: no_frame++; break;
                default:                   no_face++;  break;
            }
            continue;
        }
        // Enroll from good-enough detections (RELAXED bar — see constants). Log
        // WHY a detected face is rejected so we can tune the gate from real data
        // instead of guessing.
        if (!last_detection_passes(FACE_ENROLL_MIN_SCORE, FACE_ENROLL_MIN_BOX_FRAC)) {
            gate_rejects++;
            if (s_have_last_results && !s_last_results.empty()) {
                const auto &f = s_last_results.front();
                int w = f.box[2] - f.box[0], h = f.box[3] - f.box[1];
                ESP_LOGW(TAG, "  enroll gate reject: score=%.2f box=%dx%d "
                         "(need score>=%.2f, box>=%dx%d)",
                         f.score, w, h, FACE_ENROLL_MIN_SCORE,
                         (int)(FRAME_W * FACE_ENROLL_MIN_BOX_FRAC),
                         (int)(FRAME_H * FACE_ENROLL_MIN_BOX_FRAC));
            }
            continue;
        }

        dl::image::img_t img = build_img();
        s_recognizer->enroll(img, s_last_results);
        captured++;
        // The recognizer just assigned this feature the id == s_feat_count+1
        // (sequential after the clear). Record that it belongs to this user.
        s_feat_count++;
        if (s_feat_count <= MAX_FEATURES) {
            s_feat_user_map[s_feat_count] = (int16_t) id;
        }
        ESP_LOGI(TAG, "  captured sample %d/%d (feat id %d -> user %d)",
                 captured, samples_wanted, s_feat_count, id);
    }

    if (captured == 0) {
        ESP_LOGW(TAG, "multi-enroll captured nothing — no good face");
        return ESP_ERR_NOT_FOUND;
    }

    persist_encrypted_db();
    featmap_save();  // keep the id map in sync with the DB
    ESP_LOGI(TAG, "multi-enroll done: %d templates stored for user id=%d", captured, id);
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

    // We only care about the first detected face. Translate the recognizer's
    // internal FEATURE id back to the real USER id via the map, so the same
    // person always reports the same id regardless of which template matched.
    auto &top = results.front();
    int feat_id = top.id;
    int user_id = feat_id;  // fallback if unmapped
    if (feat_id >= 1 && feat_id <= MAX_FEATURES && s_feat_user_map[feat_id] >= 0) {
        user_id = s_feat_user_map[feat_id];
    }
    *out_id = user_id;
    *out_similarity = top.similarity;
    return ESP_OK;
}

extern "C" int face_ctrl_last_detect_fail(void)
{
    // Exposes why the last detect_once() returned false, so callers can log the
    // real reason (corrupt frame vs no face vs no frame) during scanning and
    // liveness — not just enrollment.
    return (int) s_last_detect_fail;
}

extern "C" bool face_ctrl_last_is_good_quality(void)
{
    // Live-recognition strictness: a detection is "good enough" when the face is
    // both confidently detected AND large enough. Small/low-score boxes give
    // noisy embeddings that drag similarity down.
    return last_detection_passes(FACE_MIN_DETECT_SCORE, FACE_MIN_BOX_FRAC);
}

extern "C" esp_err_t face_ctrl_get_keypoints(int *out_keypoints, int max, int *out_count)
{
    if (out_keypoints == nullptr || out_count == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_have_last_results || s_last_results.empty()) {
        return ESP_ERR_INVALID_STATE;
    }

    // Use the first (highest-scoring) detected face, same one recognize() uses.
    const auto &front = s_last_results.front();
    const std::vector<int> &kp = front.keypoint;
    if (kp.empty()) {
        return ESP_ERR_NOT_FOUND;
    }

    int n = (int) kp.size();
    if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        out_keypoints[i] = kp[i];
    }
    *out_count = n;
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