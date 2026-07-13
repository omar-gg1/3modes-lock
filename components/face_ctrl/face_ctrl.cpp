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

// ---- Feature position -> User-ID map (see design phase 2) ----
// esp-dl's query_feat() returns the 1-BASED POSITION of the match in its
// in-memory list of VALID features, NOT the stored feature id. Deletes tombstone
// a feature and compact that list, so positions shift. We therefore shadow the
// recognizer's valid list here, in the SAME order:
//   s_feat_users[pos]      = the user_id that owns the feature at that position
//   s_feat_stored_ids[pos] = esp-dl's stored id for it (needed by delete_feat)
// pos is 1-based to match query_feat; index 0 is unused. Persisted encrypted
// next to the DB and kept in lockstep with it on every enroll/delete.
#define MAX_FEATURES 64
#define FEATMAP_MAGIC 0x4E464D32u   // "NFM2" — bump if the on-disk format changes
#define MAX_ENROLL_IMAGES 8         // cloud-sync JPEG samples kept per user
#define MAX_ENROLL_USERS  16        // distinct users whose images we scan for
static int16_t  s_feat_users[MAX_FEATURES + 1];
static uint16_t s_feat_stored_ids[MAX_FEATURES + 1];
static int      s_feat_count = 0;   // number of valid features shadowed
static const char *FEATMAP_PATH     = "/spiffs/featmap.bin";
static const char *FEATMAP_ENC_PATH = "/spiffs/featmap.bin.enc";

// esp-dl assigns each enrolled feature stored id = ++num_feats_total (never
// reused, even after tombstone deletes). We can't read num_feats_total back
// (no getter), so we track the next id ourselves: max stored id seen, + 1.
static uint16_t s_next_stored_id = 1;

static void featmap_reset(void)
{
    for (int i = 0; i <= MAX_FEATURES; i++) {
        s_feat_users[i] = -1;
        s_feat_stored_ids[i] = 0;
    }
    s_feat_count = 0;
}

static void featmap_recompute_next_stored_id(void)
{
    uint16_t mx = 0;
    for (int i = 1; i <= s_feat_count; i++)
        if (s_feat_stored_ids[i] > mx) mx = s_feat_stored_ids[i];
    s_next_stored_id = mx + 1;
}

// Append one feature at the next position (mirrors esp-dl enroll appending at
// the end of its list). stored_id is what enroll_feat assigned.
static void featmap_append(int user_id, uint16_t stored_id)
{
    if (s_feat_count >= MAX_FEATURES) return;
    s_feat_count++;
    s_feat_users[s_feat_count] = (int16_t) user_id;
    s_feat_stored_ids[s_feat_count] = stored_id;
}

// Remove the entry at 1-based position `pos` and shift the tail down by one,
// mirroring m_feats.erase() compacting the valid list.
static void featmap_remove_position(int pos)
{
    if (pos < 1 || pos > s_feat_count) return;
    for (int i = pos; i < s_feat_count; i++) {
        s_feat_users[i] = s_feat_users[i + 1];
        s_feat_stored_ids[i] = s_feat_stored_ids[i + 1];
    }
    s_feat_users[s_feat_count] = -1;
    s_feat_stored_ids[s_feat_count] = 0;
    s_feat_count--;
}

// Persist both parallel arrays + count, prefixed with a magic/version word so a
// pre-phase-2 map (which had a different layout) is detected and discarded.
static void featmap_save(void)
{
    FILE *f = fopen(FEATMAP_PATH, "wb");
    if (!f) {
        ESP_LOGE(TAG, "featmap: cannot open for write");
        return;
    }
    uint32_t magic = FEATMAP_MAGIC;
    fwrite(&magic, sizeof(magic), 1, f);
    fwrite(&s_feat_count, sizeof(s_feat_count), 1, f);
    fwrite(s_feat_users, sizeof(int16_t), MAX_FEATURES + 1, f);
    fwrite(s_feat_stored_ids, sizeof(uint16_t), MAX_FEATURES + 1, f);
    fclose(f);
    crypto_ctrl_encrypt_file(FEATMAP_PATH, FEATMAP_ENC_PATH);
}

// Load the map. Resets to empty (and logs) on any mismatch: missing file, failed
// decrypt, wrong magic (old format), or short read. Caller (init) reconciles
// s_feat_count against the DB's get_num_feats() afterwards.
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
    uint32_t magic = 0;
    bool ok = fread(&magic, sizeof(magic), 1, f) == 1 && magic == FEATMAP_MAGIC;
    if (ok) ok = fread(&s_feat_count, sizeof(s_feat_count), 1, f) == 1;
    if (ok) ok = fread(s_feat_users, sizeof(int16_t), MAX_FEATURES + 1, f)
                 == (size_t)(MAX_FEATURES + 1);
    if (ok) ok = fread(s_feat_stored_ids, sizeof(uint16_t), MAX_FEATURES + 1, f)
                 == (size_t)(MAX_FEATURES + 1);
    fclose(f);
    if (!ok) {
        ESP_LOGW(TAG, "featmap old/short/bad format — starting empty");
        featmap_reset();
        return;
    }
    featmap_recompute_next_stored_id();
    ESP_LOGI(TAG, "featmap loaded: %d features mapped", s_feat_count);
}

// Defined later (with the enroll-image helpers); forward-declared so the boot
// featmap rebuild below can use them.
static int enroll_imgs_count_user(int user_id);

// Rebuild the position->user map from the per-user enrollment images on flash.
// This is the durable per-feature record that survives the reset-on-mismatch:
// esp-dl's valid features are in enrollment order, and each user's images were
// written in that same order, so summing per-user image counts in id order
// reconstructs which user owns each position. Returns true iff the reconstructed
// total matches esp-dl's valid-feature count exactly (only then is it safe — a
// legacy user with features but NO images can't be reconstructed this way).
static bool featmap_rebuild_from_images(int db_feats)
{
    int total = 0;
    for (int u = 0; u < MAX_ENROLL_USERS; u++) total += enroll_imgs_count_user(u);
    if (total == 0 || total != db_feats) return false;

    featmap_reset();
    for (int u = 0; u < MAX_ENROLL_USERS; u++) {
        int cnt = enroll_imgs_count_user(u);
        for (int s = 0; s < cnt; s++) {
            // stored_id here is just the running position; delete_feat needs the
            // real esp-dl id, but after a rebuild we only need positions to be
            // stable and users correct. next_stored_id is recomputed below.
            featmap_append(u, (uint16_t)(s_feat_count + 1));
        }
    }
    featmap_recompute_next_stored_id();
    return true;
}

// Reusable RGB888 buffer in PSRAM. QVGA = 320*240*3 = 230400 bytes.
static uint8_t *s_rgb_buf = nullptr;
static const int FRAME_W = 320;
static const int FRAME_H = 240;
static const size_t RGB_BUF_SIZE = FRAME_W * FRAME_H * 3;

// ---- Retained JPEG of the last processed frame (Mode 3 cloud verify) ----
// When local recognition scores in the murky band, the cloud verifier must see
// the EXACT frame that produced that score — not a fresh frame grabbed seconds
// later (the user has moved by then; that temporal mismatch is why cloud verify
// kept returning no_face). We keep a copy of every good frame's JPEG here;
// detect_once() returning true guarantees this buffer holds the detected frame.
// VGA @ quality 12 is ~30-60KB, so a 96KB cap covers it with margin. PSRAM.
static uint8_t *s_last_jpeg = nullptr;
static size_t   s_last_jpeg_len = 0;
static const size_t LAST_JPEG_CAP = 96 * 1024;

// ---- Recognition quality gate (CALIBRATE with real data) ----
// Only recognize/enroll from detections that clear these bars. Gating out small
// or low-confidence faces is what keeps similarity scores high and consistent.
// Enrollment uses a slightly RELAXED bar: the user is cooperating and holding
// still, and we'd rather gather all the samples than reject good-enough frames
// and time out (which is what happened on the first hardware test).
static const float FACE_MIN_DETECT_SCORE      = 0.80f;  // live recognition
static const float FACE_MIN_BOX_FRAC          = 0.25f;
// Enrollment gate. Raised so every SAVED frame is one the cloud's ArcFace
// detector will also accept — the ESP's own detector is looser than RetinaFace,
// and marginal frames that passed here (box ~0.18) were being rejected by the
// cloud with 422 "no_face" during Mode 3 sync. box_frac 0.28 of the 320px local
// frame ≈ a 90px local face ≈ a ~180px face on the retained VGA JPEG the cloud
// sees — comfortably above RetinaFace's detection floor. score 0.75 filters
// blurry/angled grabs. Net effect: fewer, cleaner samples that BOTH sides accept.
static const float FACE_ENROLL_MIN_SCORE      = 0.75f;
static const float FACE_ENROLL_MIN_BOX_FRAC   = 0.28f;

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
    // Gate on the box's SHORTER side against a single pixel floor (local 320x240
    // frame). Using FRAME_W for width but FRAME_H for height made the width gate
    // stricter than the height gate — a face 87px wide / 111px tall (a clean,
    // score-0.98 detection!) failed on width by 2px while passing height easily.
    // A single min_box_frac * FRAME_W floor on min(w,h) treats both dimensions
    // the same. This local pixel count doubles on the retained VGA frame the
    // cloud sees, so a bigger floor here => a bigger face for ArcFace's detector.
    int shorter = (w < h) ? w : h;
    if (shorter < (int)(FRAME_W * min_box_frac)) {
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

// ---- Enrollment JPEGs, kept for Mode 3 cloud sync -------------------------
// The cloud verifier (ArcFace) cannot use the ESP's esp-dl embeddings — the two
// models are different, so their 512-d vectors are incompatible. To enroll the
// SAME face on the cloud, it must re-embed the actual enrollment IMAGES with its
// own ArcFace. So during local enrollment we also persist each gate-passed VGA
// JPEG (encrypted, like the DB). On a Mode 3 boot the sync layer decrypts these
// and POSTs them to /enroll, giving both sides one face from one camera.
//   /spiffs/enroll_uUUU_SS.jpg.enc  (UUU = user_id 0..999, SS = sample 0..MAX-1)
// These are ONLY written during enrollment and ONLY read when Mode 3 syncs;
// local-only modes never touch the cloud, honouring the privacy boundary.
// Per-user filenames (not a flat enroll_NN run) are what makes multi-user work:
// an append-enroll for user B must not overwrite or re-push user A's images, and
// the boot featmap can be rebuilt from each user's on-disk sample count.
// (MAX_ENROLL_IMAGES / MAX_ENROLL_USERS defined near MAX_FEATURES at the top.)

static void enroll_img_enc_path(int user_id, int sample, char *out, size_t out_sz)
{
    snprintf(out, out_sz, "/spiffs/enroll_u%03d_%02d.jpg.enc", user_id, sample);
}

// How many enrollment images exist on flash for one user (contiguous run from 0).
static int enroll_imgs_count_user(int user_id)
{
    int n = 0;
    char p[64];
    for (int s = 0; s < MAX_ENROLL_IMAGES; s++) {
        enroll_img_enc_path(user_id, s, p, sizeof(p));
        FILE *f = fopen(p, "rb");
        if (f == nullptr) break;
        fclose(f);
        n++;
    }
    return n;
}

// Wipe ONE user's enrollment images (append-enroll for a fresh capture of the
// same user replaces that user's set, never touching other users). Only remove()
// files that EXIST: an unconditional remove() on a missing path forces a SPIFFS
// object-table scan, and a burst of them starved the idle task -> task_wdt. A
// cheap fopen() check skips misses; we yield so the watchdog is serviced.
static void enroll_imgs_clear_user(int user_id)
{
    char p[64];
    for (int s = 0; s < MAX_ENROLL_IMAGES; s++) {
        enroll_img_enc_path(user_id, s, p, sizeof(p));
        FILE *f = fopen(p, "rb");
        if (f != nullptr) {
            fclose(f);
            remove(p);
        }
        vTaskDelay(1);
    }
}

// Persist one gate-passed enrollment JPEG at (user_id, sample). Writes plaintext
// to a temp path, encrypts to the per-user path, removes the plaintext.
// Best-effort: a failure here must not abort enrollment (the local DB is primary;
// cloud sync is a bonus), so we log and move on.
static void enroll_img_save(int user_id, int sample,
                            const uint8_t *jpeg, size_t len)
{
    if (jpeg == nullptr || len == 0 || sample < 0 || sample >= MAX_ENROLL_IMAGES) {
        return;
    }
    const char *tmp = "/spiffs/enroll_tmp.jpg";
    FILE *f = fopen(tmp, "wb");
    if (f == nullptr) {
        ESP_LOGW(TAG, "  enroll-img: cannot open temp file (skipping cloud copy)");
        return;
    }
    size_t wrote = fwrite(jpeg, 1, len, f);
    fclose(f);
    if (wrote != len) {
        ESP_LOGW(TAG, "  enroll-img: short write %u/%u (skipping cloud copy)",
                 (unsigned) wrote, (unsigned) len);
        remove(tmp);
        return;
    }
    char enc[64];
    enroll_img_enc_path(user_id, sample, enc, sizeof(enc));
    esp_err_t err = crypto_ctrl_encrypt_file(tmp, enc);
    remove(tmp);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "  enroll-img: encrypt failed: %s", esp_err_to_name(err));
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

    // Retained-JPEG buffer for Mode 3 cloud verify (PSRAM — plenty free there).
    s_last_jpeg = (uint8_t *) heap_caps_malloc(LAST_JPEG_CAP, MALLOC_CAP_SPIRAM);
    if (s_last_jpeg == nullptr) {
        // Non-fatal: local recognition still works; only cloud verify degrades.
        ESP_LOGW(TAG, "no PSRAM for retained JPEG — cloud verify unavailable");
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

    // Guard against map/DB drift (e.g. a crash between the DB write and the map
    // write): if the shadowed count disagrees with the recognizer's actual valid
    // count, the map is untrustworthy — reset it. Recognition then returns raw
    // positional ids until the next enroll rewrites the map.
    if (s_recognizer != nullptr && s_feat_count != s_recognizer->get_num_feats()) {
        int db_feats = s_recognizer->get_num_feats();
        // Before wiping, try to rebuild the map from the per-user enroll images on
        // flash — this is what stops a pre-existing user (whose templates predate
        // the featmap format) from being demoted to raw positional ids on every
        // boot. ponytail: assumes esp-dl feature order == on-disk image order,
        // which holds because both are written in enrollment order and deletes
        // remove images too; only mismatched totals fall back to the wipe.
        if (featmap_rebuild_from_images(db_feats)) {
            ESP_LOGI(TAG, "featmap rebuilt from images: %d features mapped",
                     s_feat_count);
        } else {
            ESP_LOGW(TAG, "featmap count %d != db valid feats %d, no image record "
                     "— resetting map", s_feat_count, db_feats);
            featmap_reset();
            s_next_stored_id = 1;
        }
    }

    // In-memory self-test of the positional map bookkeeping. Logs PASS/FAIL.
    face_ctrl_featmap_selftest();

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
    // The camera now captures VGA (640x480) so the cloud verifier gets a big
    // enough face, but the LOCAL recognizer still wants 320x240. Scale DURING
    // decode: VGA -> 320x240 (both multiples of 8, required by esp_jpeg). This
    // keeps the local pipeline byte-for-byte identical to the old QVGA path —
    // same buffer size, same detector input — while the retained JPEG stays
    // full VGA for the cloud. If a QVGA frame ever arrives (unchanged sensors),
    // scaling to the same size is a harmless no-op.
    cfg.scale.width = FRAME_W;    // 320
    cfg.scale.height = FRAME_H;   // 240

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
        // Same torn-frame case as below — DEBUG so it doesn't flood the monitor.
        ESP_LOGD(TAG, "jpeg_dec_parse_header failed: %d (skipping torn frame)", err);
        jpeg_dec_close(dec);
        return ESP_FAIL;
    }

    // Accept the expected source sizes: VGA (the new default, scaled to 320x240
    // above) or QVGA (already 320x240). Reject anything unexpected.
    bool ok_dims = (header.width == 640 && header.height == 480) ||
                   (header.width == FRAME_W && header.height == FRAME_H);
    if (!ok_dims) {
        ESP_LOGW(TAG, "unexpected JPEG dims %dx%d", header.width, header.height);
        jpeg_dec_close(dec);
        return ESP_FAIL;
    }

    err = jpeg_dec_process(dec, &io);
    if (err != JPEG_ERR_OK) {
        // A torn frame that passed the SOI/EOI gate but is internally corrupt
        // (valid start/end markers, damaged middle). Harmless — we just skip it
        // and the next grab is whole — but at ERROR level it floods the monitor.
        // DEBUG so it's available when wanted but silent in normal operation.
        ESP_LOGD(TAG, "jpeg_dec_process failed: %d (skipping torn frame)", err);
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

    // INTEGRITY GATE — check ONCE, up front. At VGA the camera's
    // CAMERA_GRAB_LATEST can hand back a buffer still mid-DMA-write: a truncated
    // JPEG with no end-of-image marker (cam_hal logs "NO-EOI"/"NO-SOI"). Such a
    // frame can never decode — feeding it to jpeg_dec_process just spends CPU and
    // emits "jpeg_dec_process failed: -3" every scan. A valid JPEG starts with
    // SOI (FF D8) and ends with EOI (FF D9); if either is missing, skip this
    // frame entirely (no retain, no decode) and try the next one. This both
    // silences the -3 flood AND guarantees the retained frame the cloud verifies
    // is a complete image.
    bool complete_jpeg =
        fb->len >= 4 &&
        fb->buf[0] == 0xFF && fb->buf[1] == 0xD8 &&
        fb->buf[fb->len - 2] == 0xFF && fb->buf[fb->len - 1] == 0xD9;
    if (!complete_jpeg) {
        esp_camera_fb_return(fb);
        s_last_detect_fail = DETECT_FAIL_BAD_JPEG;
        return false;   // truncated grab; next frame will be whole
    }

    // Retain a copy of this (complete) frame's JPEG while fb is valid. If
    // detection below succeeds, this copy IS the detected frame — exactly what
    // Mode 3 cloud verify needs to judge the same image the local model scored.
    if (s_last_jpeg != nullptr && fb->len <= LAST_JPEG_CAP) {
        memcpy(s_last_jpeg, fb->buf, fb->len);
        s_last_jpeg_len = fb->len;
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
    s_next_stored_id = 1;
    // Keypad enroll is single-user + wipes the DB, so wipe every user's cloud-sync
    // images too (scan the id space; cheap fopen misses are skipped).
    for (int u = 0; u < MAX_ENROLL_USERS; u++) enroll_imgs_clear_user(u);

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
        // Persist this exact gate-passed VGA frame for Mode 3 cloud sync. It's a
        // complete JPEG (detect_once only retains frames with a valid EOI) and
        // it's the same look we just embedded locally — so the cloud enrolls the
        // identical face from the identical camera. Best-effort; never fatal.
        enroll_img_save(id, captured, s_last_jpeg, s_last_jpeg_len);
        captured++;
        // Cleared DB -> position == stored id == running count. Shadow it.
        featmap_append(id, s_next_stored_id++);
        ESP_LOGI(TAG, "  captured sample %d/%d (pos/id %d -> user %d)",
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

extern "C" esp_err_t face_ctrl_enroll_append(int user_id, int samples_wanted, int timeout_ms)
{
    if (s_recognizer == nullptr) {
        ESP_LOGE(TAG, "not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (samples_wanted < 1) samples_wanted = 1;

    // Multi-user path: add user_id's templates WITHOUT wiping OTHER users. But a
    // re-enroll of the SAME user must REPLACE, not stack (else duplicate templates
    // pile up and skew matching, and the cloud — which deletes-then-repushes per
    // user — would drift from us). So first drop this user's own old features and
    // images, then capture fresh ones. Other users are untouched.
    int purged = 0;
    face_ctrl_delete_user(user_id, &purged);   // 0 if new user — fine
    if (purged > 0) ESP_LOGI(TAG, "append-enroll: replaced %d old template(s) for user=%d",
                             purged, user_id);
    enroll_imgs_clear_user(user_id);

    int captured = 0;
    int64_t start_us = esp_timer_get_time();
    ESP_LOGI(TAG, "append-enroll user=%d: want %d samples (hold still)...",
             user_id, samples_wanted);
    int bad_jpeg = 0, no_face = 0, no_frame = 0, gate_rejects = 0, iters = 0;

    while (captured < samples_wanted) {
        if ((esp_timer_get_time() - start_us) / 1000 > timeout_ms) {
            ESP_LOGW(TAG, "append-enroll timed out %d/%d (iters=%d bad_jpeg=%d "
                     "no_face=%d no_frame=%d gate=%d)", captured, samples_wanted,
                     iters, bad_jpeg, no_face, no_frame, gate_rejects);
            break;
        }
        iters++;
        vTaskDelay(pdMS_TO_TICKS(ENROLL_FRAME_INTERVAL_MS));
        if (!face_ctrl_detect_once()) {
            switch (s_last_detect_fail) {
                case DETECT_FAIL_BAD_JPEG: bad_jpeg++; break;
                case DETECT_FAIL_NO_FRAME: no_frame++; break;
                default:                   no_face++;  break;
            }
            continue;
        }
        if (!last_detection_passes(FACE_ENROLL_MIN_SCORE, FACE_ENROLL_MIN_BOX_FRAC)) {
            gate_rejects++;
            continue;
        }
        if (s_feat_count >= MAX_FEATURES) {
            ESP_LOGW(TAG, "append-enroll: feature store full (%d)", MAX_FEATURES);
            break;
        }
        dl::image::img_t img = build_img();
        s_recognizer->enroll(img, s_last_results);
        enroll_img_save(user_id, captured, s_last_jpeg, s_last_jpeg_len);
        captured++;
        featmap_append(user_id, s_next_stored_id++);
        ESP_LOGI(TAG, "  append sample %d/%d (pos %d id %d -> user %d)",
                 captured, samples_wanted, s_feat_count,
                 s_feat_stored_ids[s_feat_count], user_id);
    }

    if (captured == 0) {
        ESP_LOGW(TAG, "append-enroll captured nothing — no good face");
        return ESP_ERR_NOT_FOUND;
    }
    persist_encrypted_db();
    featmap_save();
    ESP_LOGI(TAG, "append-enroll done: %d templates for user=%d (total feats %d)",
             captured, user_id, s_feat_count);
    return ESP_OK;
}

extern "C" esp_err_t face_ctrl_import_face(int user_id, int sample,
                                           const uint8_t *jpeg, size_t len)
{
    // Enroll a face that came FROM the cloud (e.g. a user enrolled via the phone
    // app that this device's camera never saw), so it also matches on the fast
    // LOCAL pass — not only via a cloud round-trip. esp-dl and ArcFace embeddings
    // aren't interchangeable, so we must re-embed the actual JPEG here.
    if (s_recognizer == nullptr || s_detector == nullptr || s_rgb_buf == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (jpeg == nullptr || len < 4) return ESP_ERR_INVALID_ARG;
    if (s_feat_count >= MAX_FEATURES) return ESP_ERR_NO_MEM;

    if (decode_jpeg_to_rgb(jpeg, len) != ESP_OK) {
        ESP_LOGW(TAG, "import user=%d sample=%d: JPEG decode failed", user_id, sample);
        return ESP_FAIL;
    }
    dl::image::img_t img = build_img();
    auto results = s_detector->run(img);
    if (results.empty()) {
        ESP_LOGW(TAG, "import user=%d sample=%d: no face in cloud image", user_id, sample);
        return ESP_ERR_NOT_FOUND;
    }
    s_last_results = results;
    s_have_last_results = true;
    s_recognizer->enroll(img, s_last_results);
    featmap_append(user_id, s_next_stored_id++);
    // Keep a local encrypted copy so a reboot's featmap rebuild sees this user,
    // and so we don't re-pull it next boot.
    enroll_img_save(user_id, sample, jpeg, len);
    persist_encrypted_db();
    featmap_save();
    ESP_LOGI(TAG, "imported cloud face: user=%d sample=%d (total feats %d)",
             user_id, sample, s_feat_count);
    return ESP_OK;
}

extern "C" esp_err_t face_ctrl_delete_user(int user_id, int *out_deleted)
{
    if (s_recognizer == nullptr) {
        ESP_LOGE(TAG, "not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    int deleted = 0;
    // Walk positions from the TOP down so removing one doesn't shift positions
    // we haven't examined yet. delete_feat needs the esp-dl STORED id, which we
    // kept in s_feat_stored_ids alongside the user mapping.
    for (int pos = s_feat_count; pos >= 1; pos--) {
        if (s_feat_users[pos] == (int16_t) user_id) {
            uint16_t stored = s_feat_stored_ids[pos];
            if (s_recognizer->delete_feat(stored) == ESP_OK) {
                featmap_remove_position(pos);
                deleted++;
            } else {
                ESP_LOGW(TAG, "delete_feat(stored=%d) failed", stored);
            }
        }
    }
    if (out_deleted) *out_deleted = deleted;
    if (deleted > 0) {
        persist_encrypted_db();
        featmap_save();
    }
    // Drop this user's cloud-sync images too, so the boot featmap rebuild (which
    // reads per-user image counts) doesn't resurrect a deleted user.
    enroll_imgs_clear_user(user_id);
    ESP_LOGI(TAG, "delete_user %d: removed %d feature(s), %d remain",
             user_id, deleted, s_feat_count);
    return ESP_OK;   // idempotent: 0 removed is not an error
}

extern "C" bool face_ctrl_featmap_selftest(void)
{
    // Snapshot real state so the test doesn't disturb a loaded map.
    static int16_t  save_u[MAX_FEATURES + 1];
    static uint16_t save_s[MAX_FEATURES + 1];
    memcpy(save_u, s_feat_users, sizeof(save_u));
    memcpy(save_s, s_feat_stored_ids, sizeof(save_s));
    int save_count = s_feat_count;
    uint16_t save_next = s_next_stored_id;

    bool pass = true;
    featmap_reset();
    s_next_stored_id = 1;

    // enroll user 7 (2 feats: stored 1,2), user 9 (1 feat: stored 3)
    featmap_append(7, s_next_stored_id++);
    featmap_append(7, s_next_stored_id++);
    featmap_append(9, s_next_stored_id++);
    pass &= (s_feat_count == 3);
    pass &= (s_feat_users[1] == 7 && s_feat_users[2] == 7 && s_feat_users[3] == 9);
    pass &= (s_feat_stored_ids[1] == 1 && s_feat_stored_ids[3] == 3);

    // delete user 7 (positions 2 then 1, top-down) -> only user 9 survives at pos 1
    for (int pos = s_feat_count; pos >= 1; pos--)
        if (s_feat_users[pos] == 7) featmap_remove_position(pos);
    pass &= (s_feat_count == 1);
    pass &= (s_feat_users[1] == 9);
    pass &= (s_feat_stored_ids[1] == 3);   // survivor keeps its ORIGINAL stored id

    // restore real state
    memcpy(s_feat_users, save_u, sizeof(save_u));
    memcpy(s_feat_stored_ids, save_s, sizeof(save_s));
    s_feat_count = save_count;
    s_next_stored_id = save_next;

    ESP_LOGI(TAG, "featmap self-test: %s", pass ? "PASS" : "FAIL");
    return pass;
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

    // We only care about the first detected face. query_feat returns the 1-based
    // POSITION in the valid list, which we translate to the real USER id via the
    // positional map, so the same person always reports the same id regardless of
    // which template matched (and it stays correct after deletes).
    auto &top = results.front();
    int position = top.id;   // 1-based position, NOT a stored feature id
    int user_id = position;  // fallback if unmapped
    if (position >= 1 && position <= s_feat_count && s_feat_users[position] >= 0) {
        user_id = s_feat_users[position];
    }
    *out_id = user_id;
    *out_similarity = top.similarity;
    return ESP_OK;
}

extern "C" esp_err_t face_ctrl_get_last_jpeg(const uint8_t **out_buf, size_t *out_len)
{
    if (out_buf == nullptr || out_len == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_last_jpeg == nullptr || s_last_jpeg_len == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    *out_buf = s_last_jpeg;
    *out_len = s_last_jpeg_len;
    return ESP_OK;
}

extern "C" int face_ctrl_enroll_image_count(int user_id)
{
    // Scan flash rather than trust a RAM counter: after a reboot the images
    // persist and Mode 3 sync runs post-boot. Per-user contiguous run from 0.
    return enroll_imgs_count_user(user_id);
}

extern "C" int face_ctrl_enrolled_user_ids(int *out_ids, int max)
{
    // List every user with at least one enrollment image on flash. The sync layer
    // uses this to push each user's own faces (never a flat 'everyone as user 0').
    int n = 0;
    for (int u = 0; u < MAX_ENROLL_USERS && n < max; u++) {
        if (enroll_imgs_count_user(u) > 0) out_ids[n++] = u;
    }
    return n;
}

extern "C" esp_err_t face_ctrl_enroll_image_decrypt(int user_id, int sample,
                                                    const char *out_path)
{
    if (out_path == nullptr || sample < 0 || sample >= MAX_ENROLL_IMAGES) {
        return ESP_ERR_INVALID_ARG;
    }
    char enc[64];
    enroll_img_enc_path(user_id, sample, enc, sizeof(enc));
    FILE *f = fopen(enc, "rb");
    if (f == nullptr) return ESP_ERR_NOT_FOUND;
    fclose(f);
    return crypto_ctrl_decrypt_file(enc, out_path);
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