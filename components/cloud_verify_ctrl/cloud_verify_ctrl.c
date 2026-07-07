#include "cloud_verify_ctrl.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "face_ctrl.h"     // face_ctrl_get_last_jpeg — the murky-scored frame
#include "wifi_config.h"   // VERIFIER_URL + VERIFIER_API_KEY (gitignored)

static const char *TAG = "cloud_verify";

// Bounded HTTP timeout: the murky-case round-trip (upload frame + ArcFace
// inference + response) must not hang the lock. If it exceeds this we treat the
// cloud as unreachable and fall back locally.
#define CLOUD_HTTP_TIMEOUT_MS 8000

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
        .timeout_ms = CLOUD_HTTP_TIMEOUT_MS,
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
