#include "cloud_verify_ctrl.h"

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "camera_ctrl.h"
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
#define RESP_BUF_SIZE 512
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
// overkill; we scan for the keys we need.
static bool json_bool(const char *json, const char *key, bool *out)
{
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ') p++;
    if (strncmp(p, "true", 4) == 0)  { *out = true;  return true; }
    if (strncmp(p, "false", 5) == 0) { *out = false; return true; }
    return false;
}

static bool json_number(const char *json, const char *key, float *out)
{
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ') p++;
    if (strncmp(p, "null", 4) == 0) return false;
    *out = strtof(p, NULL);
    return true;
}

cloud_verify_result_t cloud_verify_current_frame(int *out_user_id,
                                                 float *out_confidence)
{
    if (out_user_id) *out_user_id = -1;
    if (out_confidence) *out_confidence = 0.0f;

    // 1. Grab a HIGH-RES (VGA) frame — QVGA gives the cloud a face too small to
    //    detect/recognise reliably. This momentarily switches the sensor to VGA.
    camera_fb_t *fb = camera_ctrl_grab_highres();
    if (!fb) {
        ESP_LOGW(TAG, "no camera frame for cloud verify");
        return CLOUD_VERIFY_UNREACHABLE;
    }
    if (fb->format != PIXFORMAT_JPEG) {
        ESP_LOGW(TAG, "frame not JPEG (%d)", fb->format);
        camera_ctrl_return_frame(fb);
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
    size_t body_len = hdr_len + fb->len + trl_len;

    char *body = malloc(body_len);
    if (!body) {
        ESP_LOGE(TAG, "OOM building multipart body (%u bytes)", (unsigned) body_len);
        camera_ctrl_return_frame(fb);
        return CLOUD_VERIFY_UNREACHABLE;
    }
    memcpy(body, hdr_fmt, hdr_len);
    memcpy(body + hdr_len, fb->buf, fb->len);
    memcpy(body + hdr_len + fb->len, trailer, trl_len);
    camera_ctrl_return_frame(fb);   // frame copied into body; release it

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
        free(body);
        return CLOUD_VERIFY_UNREACHABLE;
    }
    esp_http_client_set_header(client, "Content-Type",
                               "multipart/form-data; boundary=" MP_BOUNDARY);
    esp_http_client_set_header(client, "X-API-Key", VERIFIER_API_KEY);
    esp_http_client_set_post_field(client, body, body_len);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(body);

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
