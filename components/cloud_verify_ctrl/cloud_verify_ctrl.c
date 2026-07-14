#include "cloud_verify_ctrl.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "face_ctrl.h"     // retained murky frame + enrollment images for sync
#include "wifi_config.h"   // VERIFIER_URL + VERIFIER_API_KEY (gitignored)

static const char *TAG = "cloud_verify";

// Two timeouts, because the two calls have very different latency budgets:
//
// VERIFY (murky-band, gates the lock): must NOT hang the door. 8s is plenty for
// a warm model and keeps a stalled cloud from freezing the user; if it exceeds
// this we treat the cloud as unreachable and fall back locally.
#define CLOUD_VERIFY_TIMEOUT_MS 8000
//
// ENROLL (one-time Mode 3 boot sync): the FIRST inference after the verifier
// container (re)starts pays a cold-start cost — ArcFace loads its weights and
// JITs the graph, which on a small instance can take 15-20s. This is a
// background provisioning step the user isn't waiting on a door for, so give it
// a generous timeout instead of failing with EAGAIN mid-warm-up.
#define CLOUD_ENROLL_TIMEOUT_MS 30000

// Multipart/form-data pieces. The verifier's /verify expects a form field named
// "image" containing the JPEG.
#define MP_BOUNDARY "----esp32smartlockboundary"

// Accumulate the HTTP response body here so we can parse the JSON verdict.
// 1KB: the /verify response carries a "quality" object (det_score, box dims,
// num_faces) plus reason/detail strings — a 512B cap risked truncating valid
// JSON, which json_bool() then failed to parse -> misread as "unreachable" ->
// silent local fallback. 1KB comfortably holds the full response.
#define RESP_BUF_SIZE 1024
typedef struct {
    char   buf[RESP_BUF_SIZE];
    int    len;
} resp_accum_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data) {
        resp_accum_t *acc = (resp_accum_t *) evt->user_data;
        int space = RESP_BUF_SIZE - 1 - acc->len;
        if (space > 0) {
            int n = evt->data_len < space ? evt->data_len : space;
            memcpy(acc->buf + acc->len, evt->data, n);
            acc->len += n;
            acc->buf[acc->len] = 0;
        }
    }
    return ESP_OK;
}

// Tiny JSON field extractors — the response is a small fixed-shape object like
// {"match":true,"user_id":0,"confidence":0.7882,...}. A full JSON parser is
// overkill; we scan for the keys we need. To avoid a key matching a SUBSTRING
// of a longer key ("id" inside "user_id"), we only accept a match preceded by
// a key boundary ('{' or ',', ignoring whitespace) — i.e. a real top-level key.
static const char *find_key_value(const char *json, const char *key)
{
    char pat[32];
    int n = snprintf(pat, sizeof(pat), "\"%s\":", key);
    if (n <= 0 || n >= (int) sizeof(pat)) return NULL;
    const char *p = json;
    while ((p = strstr(p, pat)) != NULL) {
        // Walk back over whitespace before the opening quote.
        const char *b = p;
        while (b > json && (b[-1] == ' ' || b[-1] == '\n' || b[-1] == '\t')) b--;
        if (b == json || b[-1] == '{' || b[-1] == ',') {
            const char *v = p + strlen(pat);
            while (*v == ' ') v++;
            return v;   // points at the value
        }
        p += strlen(pat);   // false hit inside a longer key — keep looking
    }
    return NULL;
}

static bool json_bool(const char *json, const char *key, bool *out)
{
    const char *v = find_key_value(json, key);
    if (!v) return false;
    if (strncmp(v, "true", 4) == 0)  { *out = true;  return true; }
    if (strncmp(v, "false", 5) == 0) { *out = false; return true; }
    return false;
}

static bool json_number(const char *json, const char *key, float *out)
{
    const char *v = find_key_value(json, key);
    if (!v) return false;
    if (strncmp(v, "null", 4) == 0) return false;
    *out = strtof(v, NULL);
    return true;
}

cloud_verify_result_t cloud_verify_current_frame(int *out_user_id,
                                                 float *out_confidence)
{
    if (out_user_id) *out_user_id = -1;
    if (out_confidence) *out_confidence = 0.0f;

    // 1. Use the RETAINED frame — the exact JPEG the local recognizer just
    //    scored in the murky band. Grabbing a fresh frame here was the old bug:
    //    seconds later (worse with any camera reconfig) the user has moved, so
    //    the cloud judged a DIFFERENT scene and kept answering no_face. The
    //    retained frame is guaranteed to contain the face the local model saw.
    //    No resolution switching: both VGA attempts failed (set_framesize left
    //    QVGA-sized buffers -> truncated JPEGs; full reinit thrashed the camera
    //    for ~2s per call) and a QVGA face crop (~110px) is already at ArcFace's
    //    native 112x112 input size, so QVGA is sufficient for recognition.
    const uint8_t *jpeg = NULL;
    size_t jpeg_len = 0;
    if (face_ctrl_get_last_jpeg(&jpeg, &jpeg_len) != ESP_OK) {
        ESP_LOGW(TAG, "no retained frame for cloud verify");
        return CLOUD_VERIFY_UNREACHABLE;
    }

    // 2. Build the multipart body: [header][jpeg bytes][trailer].
    const char *hdr_fmt =
        "--" MP_BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"image\"; filename=\"frame.jpg\"\r\n"
        "Content-Type: image/jpeg\r\n\r\n";
    const char *trailer = "\r\n--" MP_BOUNDARY "--\r\n";
    size_t hdr_len = strlen(hdr_fmt);
    size_t trl_len = strlen(trailer);
    size_t body_len = hdr_len + jpeg_len + trl_len;

    // PSRAM, not malloc(): a ~15KB body would otherwise land in the scarce
    // internal heap (allocations under SPIRAM_MALLOC_ALWAYSINTERNAL=16KB are
    // forced internal), and internal RAM is the tightest resource on this build.
    char *body = heap_caps_malloc(body_len, MALLOC_CAP_SPIRAM);
    if (!body) {
        ESP_LOGE(TAG, "OOM building multipart body (%u bytes)", (unsigned) body_len);
        return CLOUD_VERIFY_UNREACHABLE;
    }
    memcpy(body, hdr_fmt, hdr_len);
    memcpy(body + hdr_len, jpeg, jpeg_len);
    memcpy(body + hdr_len + jpeg_len, trailer, trl_len);

    // 3. POST to the verifier.
    resp_accum_t acc = { .len = 0 };
    esp_http_client_config_t cfg = {
        .url = VERIFIER_URL "/verify",
        .method = HTTP_METHOD_POST,
        .timeout_ms = CLOUD_VERIFY_TIMEOUT_MS,
        .event_handler = http_event_handler,
        .user_data = &acc,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        heap_caps_free(body);
        return CLOUD_VERIFY_UNREACHABLE;
    }
    esp_http_client_set_header(client, "Content-Type",
                               "multipart/form-data; boundary=" MP_BOUNDARY);
    esp_http_client_set_header(client, "X-API-Key", VERIFIER_API_KEY);
    esp_http_client_set_post_field(client, body, body_len);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    heap_caps_free(body);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cloud verify HTTP failed: %s — falling back locally",
                 esp_err_to_name(err));
        return CLOUD_VERIFY_UNREACHABLE;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "cloud verify HTTP status %d: %s", status, acc.buf);
        return CLOUD_VERIFY_UNREACHABLE;
    }

    // 4. Parse the verdict.
    ESP_LOGI(TAG, "cloud verify response: %s", acc.buf);
    bool match = false;
    if (!json_bool(acc.buf, "match", &match)) {
        ESP_LOGW(TAG, "cloud verify: no 'match' field in response");
        return CLOUD_VERIFY_UNREACHABLE;
    }
    float conf = 0.0f;
    json_number(acc.buf, "confidence", &conf);
    if (out_confidence) *out_confidence = conf;

    if (match) {
        float uid = -1.0f;
        if (json_number(acc.buf, "user_id", &uid) && out_user_id) {
            *out_user_id = (int) uid;
        }
        ESP_LOGI(TAG, "cloud MATCH user=%d confidence=%.3f", (int) uid, conf);
        return CLOUD_VERIFY_MATCH;
    }
    ESP_LOGI(TAG, "cloud NO_MATCH confidence=%.3f", conf);
    return CLOUD_VERIFY_NO_MATCH;
}

// ---- Mode 3 enrollment sync (local faces -> cloud /enroll) -----------------

// POST one JPEG (already in a heap buffer) to /enroll as multipart form-data
// with the three fields the endpoint requires: user_id, name, image. Returns
// true iff the server answered 200 (it enrolled the face). Text form fields are
// emitted as their own multipart parts before the file part.
static bool post_enroll_image(int user_id, const char *name,
                              const uint8_t *jpeg, size_t jpeg_len)
{
    // Build the field parts. Keep them small and fixed-shape; the JPEG dominates.
    char fields[512];
    int fld = snprintf(fields, sizeof(fields),
        "--" MP_BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"user_id\"\r\n\r\n%d\r\n"
        "--" MP_BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"name\"\r\n\r\n%s\r\n"
        "--" MP_BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"source\"\r\n\r\nesp32\r\n"
        "--" MP_BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"image\"; filename=\"enroll.jpg\"\r\n"
        "Content-Type: image/jpeg\r\n\r\n",
        user_id, name);
    if (fld <= 0 || fld >= (int) sizeof(fields)) return false;

    const char *trailer = "\r\n--" MP_BOUNDARY "--\r\n";
    size_t trl_len = strlen(trailer);
    size_t body_len = (size_t) fld + jpeg_len + trl_len;

    char *body = heap_caps_malloc(body_len, MALLOC_CAP_SPIRAM);
    if (!body) {
        ESP_LOGE(TAG, "OOM building enroll body (%u bytes)", (unsigned) body_len);
        return false;
    }
    memcpy(body, fields, fld);
    memcpy(body + fld, jpeg, jpeg_len);
    memcpy(body + fld + jpeg_len, trailer, trl_len);

    resp_accum_t acc = { .len = 0 };
    esp_http_client_config_t cfg = {
        .url = VERIFIER_URL "/enroll",
        .method = HTTP_METHOD_POST,
        .timeout_ms = CLOUD_ENROLL_TIMEOUT_MS,   // tolerate ArcFace cold start
        .event_handler = http_event_handler,
        .user_data = &acc,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { heap_caps_free(body); return false; }
    esp_http_client_set_header(client, "Content-Type",
                               "multipart/form-data; boundary=" MP_BOUNDARY);
    esp_http_client_set_header(client, "X-API-Key", VERIFIER_API_KEY);
    esp_http_client_set_post_field(client, body, body_len);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    heap_caps_free(body);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "enroll POST failed: %s", esp_err_to_name(err));
        return false;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "enroll HTTP %d: %s", status, acc.buf);
        return false;
    }
    ESP_LOGI(TAG, "enroll accepted: %s", acc.buf);
    return true;
}

// A bigger accumulator for the /faces LIST (many rows) than the 1KB verify buf.
// 8KB holds ~250 rows of {"id":N,"user_id":N,"name":"..","source":".."}.
#define FACES_BUF_SIZE 8192
typedef struct { char *buf; int cap; int len; } big_accum_t;

static esp_err_t big_http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data) {
        big_accum_t *acc = (big_accum_t *) evt->user_data;
        int space = acc->cap - 1 - acc->len;
        if (space > 0) {
            int n = evt->data_len < space ? evt->data_len : space;
            memcpy(acc->buf + acc->len, evt->data, n);
            acc->len += n;
            acc->buf[acc->len] = 0;
        }
    }
    return ESP_OK;
}

bool cloud_verify_faces_revision(int *out_count, int *out_max_id)
{
    resp_accum_t acc = {0};
    char url[128];
    snprintf(url, sizeof(url), VERIFIER_URL "/faces/revision");
    esp_http_client_config_t cfg = {
        .url = url, .method = HTTP_METHOD_GET,
        .timeout_ms = CLOUD_VERIFY_TIMEOUT_MS,
        .event_handler = http_event_handler, .user_data = &acc,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return false;
    esp_http_client_set_header(cli, "X-API-Key", VERIFIER_API_KEY);
    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);
    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "faces revision: unreachable (err=%d status=%d)", err, status);
        return false;
    }
    float c = 0, m = 0;
    json_number(acc.buf, "count", &c);
    json_number(acc.buf, "max_id", &m);
    if (out_count)  *out_count  = (int) c;
    if (out_max_id) *out_max_id = (int) m;
    return true;
}

// Fetch one face image (GET /faces/{id}/image) into a PSRAM buffer. Caller frees.
static uint8_t *fetch_face_image(int enc_id, size_t *out_len)
{
    uint8_t *buf = heap_caps_malloc(64 * 1024, MALLOC_CAP_SPIRAM);
    if (!buf) return NULL;
    big_accum_t acc = { .buf = (char *) buf, .cap = 64 * 1024, .len = 0 };
    char url[128];
    snprintf(url, sizeof(url), VERIFIER_URL "/faces/%d/image", enc_id);
    esp_http_client_config_t cfg = {
        .url = url, .method = HTTP_METHOD_GET,
        .timeout_ms = CLOUD_ENROLL_TIMEOUT_MS,
        .event_handler = big_http_event_handler, .user_data = &acc,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) { heap_caps_free(buf); return NULL; }
    esp_http_client_set_header(cli, "X-API-Key", VERIFIER_API_KEY);
    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);
    if (err != ESP_OK || status != 200 || acc.len < 4) {
        heap_caps_free(buf);
        return NULL;
    }
    *out_len = acc.len;
    return buf;
}

bool cloud_verify_delete_user(int user_id)
{
    char url[128];
    snprintf(url, sizeof(url), VERIFIER_URL "/encodings?user_id=%d", user_id);
    esp_http_client_config_t cfg = {
        .url = url, .method = HTTP_METHOD_DELETE,
        .timeout_ms = CLOUD_ENROLL_TIMEOUT_MS,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return false;
    esp_http_client_set_header(cli, "X-API-Key", VERIFIER_API_KEY);
    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);
    bool ok = (err == ESP_OK && status == 200);
    ESP_LOGI(TAG, "cloud delete user %d: %s (status=%d)", user_id,
             ok ? "ok" : "FAILED", status);
    return ok;
}

int cloud_verify_pull_new_faces(const int *known_users, int known_count)
{
    // GET /faces, and for every face whose user_id we DON'T already hold locally,
    // pull its JPEG and import it into the local recognizer. This is how an
    // app-enrolled user (that this camera never saw) comes to match on the fast
    // local pass too. Per-user sample counter so local image filenames don't clash.
    char *body = heap_caps_malloc(FACES_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!body) return 0;
    big_accum_t acc = { .buf = body, .cap = FACES_BUF_SIZE, .len = 0 };
    char url[128];
    snprintf(url, sizeof(url), VERIFIER_URL "/faces");
    esp_http_client_config_t cfg = {
        .url = url, .method = HTTP_METHOD_GET,
        .timeout_ms = CLOUD_VERIFY_TIMEOUT_MS,
        .event_handler = big_http_event_handler, .user_data = &acc,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) { heap_caps_free(body); return 0; }
    esp_http_client_set_header(cli, "X-API-Key", VERIFIER_API_KEY);
    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);
    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "pull faces: list unreachable (err=%d status=%d)", err, status);
        heap_caps_free(body);
        return 0;
    }

    // Walk the array element by element: each object has "id" and "user_id".
    // Reuse the top-level-key-aware json_number by scanning object-at-a-time.
    int imported = 0;
    int sample_of[64];               // per-user local sample counter (id 0..63)
    memset(sample_of, 0, sizeof(sample_of));
    const char *p = body;
    while ((p = strstr(p, "\"id\"")) != NULL) {
        float fid = -1, fuid = -1;
        json_number(p, "id", &fid);
        json_number(p, "user_id", &fuid);
        p += 4;
        int enc_id = (int) fid, uid = (int) fuid;
        if (enc_id < 0 || uid < 0) continue;

        bool known = false;
        for (int k = 0; k < known_count; k++)
            if (known_users[k] == uid) { known = true; break; }
        if (known) continue;   // we already hold this user locally

        size_t jlen = 0;
        uint8_t *jpg = fetch_face_image(enc_id, &jlen);
        if (!jpg) { ESP_LOGW(TAG, "pull: image %d fetch failed", enc_id); continue; }
        int s = (uid < 64) ? sample_of[uid]++ : 0;
        if (face_ctrl_import_face(uid, s, jpg, jlen) == ESP_OK) imported++;
        heap_caps_free(jpg);
        vTaskDelay(pdMS_TO_TICKS(20));   // HTTP + esp-dl enroll per iter — yield
    }
    heap_caps_free(body);
    ESP_LOGI(TAG, "pull faces: imported %d new cloud face(s)", imported);
    return imported;
}

int cloud_verify_sync_enrollments(int user_id, const char *name)
{
    if (name == NULL) name = "user";

    int total = face_ctrl_enroll_image_count(user_id);
    if (total <= 0) {
        ESP_LOGW(TAG, "cloud sync: no enrollment images for user %d — nothing to push",
                 user_id);
        return 0;
    }

    // Clear this user's existing cloud references FIRST, so a re-sync REPLACES
    // rather than stacks. save_encoding always inserts; without this, every boot
    // would add another 5 duplicate references and inflate the genuine-score
    // distribution in the FAR/FRR eval. Best-effort — if it fails we still push
    // (worst case a few duplicates, not a broken match).
    {
        char url[128];
        snprintf(url, sizeof(url), VERIFIER_URL "/encodings?user_id=%d", user_id);
        esp_http_client_config_t dcfg = {
            .url = url,
            .method = HTTP_METHOD_DELETE,
            .timeout_ms = CLOUD_ENROLL_TIMEOUT_MS,   // may hit a cold server first
        };
        esp_http_client_handle_t dcli = esp_http_client_init(&dcfg);
        if (dcli) {
            esp_http_client_set_header(dcli, "X-API-Key", VERIFIER_API_KEY);
            if (esp_http_client_perform(dcli) == ESP_OK) {
                ESP_LOGI(TAG, "cloud sync: cleared old cloud references for user %d",
                         user_id);
            }
            esp_http_client_cleanup(dcli);
        }
    }

    ESP_LOGI(TAG, "cloud sync: pushing %d enrolled face(s) to the cloud...", total);

    const char *tmp = "/spiffs/cloud_sync.jpg";
    int synced = 0;
    for (int i = 0; i < total; i++) {
        if (face_ctrl_enroll_image_decrypt(user_id, i, tmp) != ESP_OK) {
            ESP_LOGW(TAG, "cloud sync: could not decrypt user %d image %d", user_id, i);
            continue;
        }
        // Read the plaintext JPEG into a PSRAM buffer.
        FILE *f = fopen(tmp, "rb");
        if (!f) { remove(tmp); continue; }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz <= 0) { fclose(f); remove(tmp); continue; }
        uint8_t *buf = heap_caps_malloc((size_t) sz, MALLOC_CAP_SPIRAM);
        if (!buf) { fclose(f); remove(tmp); continue; }
        size_t rd = fread(buf, 1, (size_t) sz, f);
        fclose(f);
        remove(tmp);   // don't leave plaintext JPEG on flash
        if (rd == (size_t) sz && post_enroll_image(user_id, name, buf, rd)) {
            synced++;
        }
        heap_caps_free(buf);
    }
    ESP_LOGI(TAG, "cloud sync: %d/%d enrolled faces accepted by the cloud",
             synced, total);
    return synced;
}
