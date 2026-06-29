#include "mqtt_ctrl.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sntp.h"
#include "mqtt_client.h"

// Broker + identity live in a gitignored header (SSID/pass are used by the
// caller for Wi-Fi; we use the broker fields + device id here). See
// wifi_config.h.example for the expected fields.
#include "wifi_config.h"

static const char *TAG = "mqtt_ctrl";

static esp_mqtt_client_handle_t s_client = NULL;
static volatile bool s_connected = false;

// Build the events topic once: smartlock/<device_id>/events.
static char s_topic[64];

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    switch ((esp_mqtt_event_id_t) event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "broker connected");
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "broker disconnected (will retry in background)");
        break;
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

esp_err_t mqtt_ctrl_init(void)
{
    if (s_client != NULL) {
        ESP_LOGW(TAG, "already initialized");
        return ESP_OK;
    }

    time_sync_start();

    snprintf(s_topic, sizeof(s_topic), "smartlock/%s/events", MQTT_DEVICE_ID);

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = MQTT_BROKER_URI,   // e.g. "mqtt://192.168.1.10:1883"
    };
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
    // FIRE-AND-FORGET. If we're not connected (or never started), drop the event
    // and return instantly — the lock must never stall on reporting.
    if (s_client == NULL || !s_connected) {
        return;
    }

    // Build the JSON by hand — the payload is tiny and fixed-shape, so a manual
    // snprintf is lighter than pulling in a JSON library. id<0 and NaN score are
    // emitted as JSON null to match the contract ([[mode2-backend]]).
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

    // QoS 0, non-retained: best-effort telemetry. enqueue (non-blocking) rather
    // than the blocking publish so we never wait on the network here.
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
