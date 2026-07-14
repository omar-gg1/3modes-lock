#include "mqtt_ctrl.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sntp.h"
#include "esp_heap_caps.h"
#include "mqtt_client.h"
#include "cJSON.h"

#include "cmd_verify.h"
#include "lock_ctrl.h"
#include "enroll_request.h"
#include "temp_pin.h"
#include "door_pin.h"
#include "face_ctrl.h"
#include "cloud_verify_ctrl.h"

// Broker + identity live in a gitignored header (SSID/pass are used by the
// caller for Wi-Fi; we use the broker fields + device id here). See
// wifi_config.h.example for the expected fields.
#include "wifi_config.h"

static const char *TAG = "mqtt_ctrl";

static esp_mqtt_client_handle_t s_client = NULL;
static volatile bool s_connected = false;

// Topics, built once in mqtt_ctrl_init:
//   events   : smartlock/<device_id>/events   (outbound, existing)
//   commands : smartlock/<device_id>/commands (inbound, Nixis command channel)
//   acks     : smartlock/<device_id>/acks      (outbound command results)
static char s_topic[64];      // events
static char s_cmd_topic[64];  // commands (subscribe)
static char s_ack_topic[64];  // acks (publish)

static long long event_timestamp(void);          // fwd decl (defined below)
static void handle_command(const char *json, int len);  // fwd decl

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    switch ((esp_mqtt_event_id_t) event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "broker connected");
        // Subscribe to the inbound command topic (QoS 1). Re-subscribes on every
        // reconnect, which is what we want.
        esp_mqtt_client_subscribe(s_client, s_cmd_topic, 1);
        ESP_LOGI(TAG, "subscribed to %s", s_cmd_topic);
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "broker disconnected (will retry in background)");
        break;
    case MQTT_EVENT_DATA: {
        esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t) event_data;
        // esp-mqtt's topic pointer is valid for exactly topic_len bytes and is
        // NOT guaranteed NUL-terminated, so match by length-bounded compare.
        // Only whole messages on .../commands are handled. Payloads (~200 B) are
        // under the 256 B buffer, so fragmentation does not occur in practice.
        const char *suffix = "/commands";
        const size_t suf_len = 9;  // strlen("/commands")
        if (event->topic_len > (int) suf_len &&
            strncmp(event->topic, "smartlock/", 10) == 0 &&
            strncmp(event->topic + event->topic_len - suf_len, suffix, suf_len) == 0) {
            handle_command(event->data, event->data_len);
        }
        break;
    }
    case MQTT_EVENT_ERROR:
        // Non-fatal: the client retries on its own. Just note it.
        ESP_LOGW(TAG, "mqtt error event");
        break;
    default:
        break;
    }
}

// Kick off SNTP so the device learns real wall-clock time. The ESP32 has no
// RTC battery, so until this completes time() returns seconds-since-1970-epoch-0
// (i.e. year 1970). We start it async here (Wi-Fi is already up by the time
// mqtt_ctrl_init runs) and the event's ts uses real epoch ONCE the clock is set,
// falling back to uptime seconds otherwise — so an event is never blocked
// waiting for time sync.
static void time_sync_start(void)
{
    if (esp_sntp_enabled()) {
        return;  // already started
    }
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP started (pool.ntp.org) — ts becomes real epoch once synced");
}

// Real epoch seconds if the clock has been set by SNTP; otherwise uptime
// seconds as a clearly-not-1970 fallback. A synced clock reads year >= 2024.
static long long event_timestamp(void)
{
    time_t now = time(NULL);
    if (now > 1700000000) {   // ~Nov 2023; proves SNTP has set real time
        return (long long) now;
    }
    return (long long)(esp_timer_get_time() / 1000000);  // uptime fallback
}

// Publish one command ack on smartlock/<id>/acks (QoS 1). Fire-and-forget like
// events — never blocks. nonce echoes the command; detail is a Plan 0 value.
static void publish_ack(const char *nonce, const char *result, const char *detail)
{
    if (s_client == NULL) {
        return;
    }
    char payload[160];
    int n = snprintf(payload, sizeof(payload),
        "{\"nonce\":\"%s\",\"result\":\"%s\",\"detail\":\"%s\",\"ts\":%lld}",
        nonce, result, detail, event_timestamp());
    if (n <= 0 || n >= (int) sizeof(payload)) {
        ESP_LOGW(TAG, "ack payload truncated, skipping");
        return;
    }
    esp_mqtt_client_enqueue(s_client, s_ack_topic, payload, n, 1, 0, true);
    ESP_LOGI(TAG, "ack: %s -> %s/%s", nonce, result, detail);
}

// Parse + verify + execute one command JSON, then ack. Runs in the mqtt task.
// Verification order is Plan 0: parse -> known type -> signature -> expiry ->
// replay -> execute. A failed check acks with the reason and never acts.
static void handle_command(const char *json, int len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (root == NULL) {
        // No reliable nonce to ack against — drop silently.
        ESP_LOGW(TAG, "command: unparseable JSON dropped");
        return;
    }

    const cJSON *jtype  = cJSON_GetObjectItem(root, "type");
    const cJSON *jnonce = cJSON_GetObjectItem(root, "nonce");
    const cJSON *jiat   = cJSON_GetObjectItem(root, "iat");
    const cJSON *jexp   = cJSON_GetObjectItem(root, "exp");
    const cJSON *jsig   = cJSON_GetObjectItem(root, "sig");
    const cJSON *jargs  = cJSON_GetObjectItem(root, "args");

    if (!cJSON_IsString(jtype) || !cJSON_IsString(jnonce) ||
        !cJSON_IsNumber(jiat) || !cJSON_IsNumber(jexp) ||
        !cJSON_IsString(jsig)) {
        ESP_LOGW(TAG, "command: malformed fields dropped");
        cJSON_Delete(root);
        return;  // no reliable nonce -> drop
    }

    const char *type  = jtype->valuestring;
    const char *nonce = jnonce->valuestring;
    long long iat = (long long) jiat->valuedouble;
    long long exp = (long long) jexp->valuedouble;

    // Recompute compact args exactly as the backend did. Phase 1 commands use
    // empty args, so "{}" is canonical. If a non-empty object is present, print
    // it unformatted (Phase 2 must ensure the backend sends sorted keys).
    char argsbuf[128] = "{}";
    if (jargs != NULL && cJSON_IsObject(jargs) && jargs->child != NULL) {
        char *p = cJSON_PrintUnformatted((cJSON *) jargs);
        if (p != NULL) {
            snprintf(argsbuf, sizeof(argsbuf), "%s", p);
            cJSON_free(p);
        }
    }

    char sstr[256];
    cmd_build_signing_string(sstr, sizeof(sstr), MQTT_DEVICE_ID, type, nonce,
                             iat, exp, argsbuf);

    // 1) signature
    if (!cmd_sig_matches(CMD_HMAC_SECRET, sstr, jsig->valuestring)) {
        publish_ack(nonce, "denied", "bad_sig");
        cJSON_Delete(root);
        return;
    }
    // 2) expiry — needs a real (SNTP-synced) clock. If unsynced, event_timestamp
    // returns uptime seconds (< 1.7e9), which we treat as expired: never accept
    // a command we cannot time-check. CMD_SKEW_S absorbs SNTP jitter so a fresh
    // command isn't nipped by a sub-second drift against the 8s window.
    // ponytail: fixed 5s allowance, widen only if real drift exceeds it.
    const long long CMD_SKEW_S = 5;
    long long now = event_timestamp();
    if (now < 1700000000LL || now > exp + CMD_SKEW_S) {
        publish_ack(nonce, "denied", "expired");
        cJSON_Delete(root);
        return;
    }
    // 3) replay
    if (cmd_nonce_seen_or_record(nonce)) {
        publish_ack(nonce, "denied", "replay");
        cJSON_Delete(root);
        return;
    }
    // 4) dispatch
    if (strcmp(type, "ping") == 0) {
        publish_ack(nonce, "ok", "pong");
    } else if (strcmp(type, "unlock") == 0) {
        lock_ctrl_trigger_unlock("app command");
        publish_ack(nonce, "ok", "unlocked");
    } else if (strcmp(type, "delete_user") == 0) {
        const cJSON *juid = cJSON_GetObjectItem(jargs, "user_id");
        if (!cJSON_IsNumber(juid)) {
            publish_ack(nonce, "error", "bad_args");
        } else {
            int deleted = 0;
            int uid = (int) juid->valuedouble;
            face_ctrl_delete_user(uid, &deleted);
            // Keep the cloud gallery consistent: remove this user's references
            // there too, else the deleted person still matches via /verify.
            cloud_verify_delete_user(uid);
            publish_ack(nonce, "ok", "deleted");
        }
    } else if (strcmp(type, "append_enroll") == 0) {
        const cJSON *juid = cJSON_GetObjectItem(jargs, "user_id");
        if (!cJSON_IsNumber(juid)) {
            publish_ack(nonce, "error", "bad_args");
        } else {
            const cJSON *jsamp = cJSON_GetObjectItem(jargs, "samples");
            int samples = cJSON_IsNumber(jsamp) ? (int) jsamp->valuedouble : 5;
            // Enrollment is a blocking multi-second capture — it CANNOT run in
            // the MQTT task (would block acks + trip the task watchdog). Hand it
            // to the main loop and ack that we've armed it. Enroll success is
            // reported later as a normal access event, not this ack.
            enroll_request_set((int) juid->valuedouble, samples);
            publish_ack(nonce, "ok", "arming");
        }
    } else if (strcmp(type, "set_temp_pin") == 0) {
        // OTP-style guest PIN. Empty pin => revoke. Stored in RAM; accepted on
        // the keypad once (temp_pin_try) until it expires. See [[temp_pin]].
        const cJSON *jpin = cJSON_GetObjectItem(jargs, "pin");
        const cJSON *jttl = cJSON_GetObjectItem(jargs, "ttl_s");
        const char *pin = cJSON_IsString(jpin) ? jpin->valuestring : "";
        int ttl_s = cJSON_IsNumber(jttl) ? (int) jttl->valuedouble : 0;
        temp_pin_set(pin, ttl_s);
        publish_ack(nonce, "ok", pin[0] == '\0' ? "cleared" : "armed");
    } else if (strcmp(type, "set_door_pin") == 0) {
        // Persistent household PIN — survives reboot, no expiry. See [[door_pin]].
        const cJSON *jpin = cJSON_GetObjectItem(jargs, "pin");
        const char *pin = cJSON_IsString(jpin) ? jpin->valuestring : "";
        if (door_pin_set(pin)) {
            publish_ack(nonce, "ok", "updated");
        } else {
            publish_ack(nonce, "error", "bad_pin");
        }
    } else {
        publish_ack(nonce, "error", "unknown_type");
    }

    cJSON_Delete(root);
}

esp_err_t mqtt_ctrl_init(void)
{
    if (s_client != NULL) {
        ESP_LOGW(TAG, "already initialized");
        return ESP_OK;
    }

    time_sync_start();

    // Prove the command-verify core matches the backend byte-for-byte (KAT).
    // Logs PASS/FAIL over serial — visible evidence the HMAC is interoperable.
    cmd_verify_selftest();

    snprintf(s_topic, sizeof(s_topic), "smartlock/%s/events", MQTT_DEVICE_ID);
    snprintf(s_cmd_topic, sizeof(s_cmd_topic), "smartlock/%s/commands", MQTT_DEVICE_ID);
    snprintf(s_ack_topic, sizeof(s_ack_topic), "smartlock/%s/acks", MQTT_DEVICE_ID);

    // Keep MQTT's memory footprint SMALL. 
    esp_mqtt_client_config_t cfg;
    memset(&cfg, 0, sizeof(esp_mqtt_client_config_t));

    cfg.broker.address.uri = MQTT_BROKER_URI;   // e.g. "mqtt://192.168.1.10:1883"
    
    // Reduce internal task stack size safely to leave breathing room for internal DRAM
    cfg.task.stack_size = 3072;                 
    
    // Shrink internal network message buffers for 120-byte payload footprints
    cfg.buffer.size = 256;                      
    cfg.buffer.out_size = 256;                  
    cfg.outbox.limit = 512;                     

#ifdef MQTT_USERNAME
    cfg.credentials.username = MQTT_USERNAME;
    cfg.credentials.authentication.password = MQTT_PASSWORD;
#endif

    s_client = esp_mqtt_client_init(&cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "failed to create mqtt client");
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);

    esp_err_t err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to start mqtt client: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "mqtt client started, publishing to %s", s_topic);
    return ESP_OK;
}

static const char *method_str(mqtt_method_t m)
{
    switch (m) {
    case MQTT_METHOD_FACE:   return "face";
    case MQTT_METHOD_PIN:    return "pin";
    case MQTT_METHOD_BUTTON: return "button";
    default:                 return "unknown";
    }
}

void mqtt_ctrl_publish_event(mqtt_method_t method, int id, float score, bool granted)
{
    if (s_client == NULL || !s_connected) {
        return;
    }

    char id_buf[16];
    char score_buf[24];
    if (id < 0) {
        strcpy(id_buf, "null");
    } else {
        snprintf(id_buf, sizeof(id_buf), "%d", id);
    }
    if (isnan(score)) {
        strcpy(score_buf, "null");
    } else {
        snprintf(score_buf, sizeof(score_buf), "%.3f", score);
    }

    char payload[160];
    int n = snprintf(payload, sizeof(payload),
        "{\"event\":\"access\",\"method\":\"%s\",\"id\":%s,\"score\":%s,"
        "\"result\":\"%s\",\"ts\":%lld}",
        method_str(method), id_buf, score_buf,
        granted ? "granted" : "denied",
        event_timestamp());
    if (n <= 0 || n >= (int) sizeof(payload)) {
        ESP_LOGW(TAG, "event payload truncated, skipping");
        return;
    }

    int msg_id = esp_mqtt_client_enqueue(s_client, s_topic, payload, n, 0, 0, true);
    if (msg_id < 0) {
        ESP_LOGW(TAG, "failed to enqueue event (dropped)");
    } else {
        ESP_LOGI(TAG, "event queued: %s", payload);
    }
}

bool mqtt_ctrl_is_connected(void)
{
    return s_connected;
}
