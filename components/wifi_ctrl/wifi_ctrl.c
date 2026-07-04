#include "wifi_ctrl.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"

#define WIFI_MAX_RETRY 5
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static const char *TAG = "wifi_ctrl";
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;

// FIX #4: guard against a second call re-running esp_wifi_init()/netif setup,
// which would abort via ESP_ERROR_CHECK on a duplicate netif/driver. If you
// ever add a "retry Wi-Fi from settings" feature, extend wifi_ctrl_connect()
// to skip straight to esp_wifi_connect() when this is already true.
static bool s_wifi_stack_initialized = false;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        // FIX #2: log WHY we disconnected instead of flying blind. reason codes
        // are defined in esp_wifi_types.h (e.g. 2=AUTH_EXPIRE, 15=4WAY_HANDSHAKE_TIMEOUT,
        // 200=BEACON_TIMEOUT, 201=NO_AP_FOUND, 202=AUTH_FAIL). Look up whatever
        // number shows up here before assuming it's the same failure every time.
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "disconnected, reason=%d", disc->reason);

        if (s_retry_num < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Wi-Fi retry %d/%d", s_retry_num, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "============================================");
        ESP_LOGI(TAG, "Wi-Fi connected. IP: " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Open in browser: http://" IPSTR "/", IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "============================================");
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_ctrl_connect(const char *ssid, const char *password, uint32_t timeout_ms) {
    if (s_wifi_stack_initialized) {
        ESP_LOGW(TAG, "wifi_ctrl_connect() called twice — stack already up, refusing to re-init");
        return ESP_ERR_INVALID_STATE;
    }

    s_wifi_event_group = xEventGroupCreate();
    // FIX #3: xEventGroupCreate() can return NULL under heap pressure. Right now
    // this runs early at boot on a clean heap so it's unlikely, but don't let
    // that be the reason it silently dereferences NULL the one time it isn't.
    if (s_wifi_event_group == NULL) {
        ESP_LOGE(TAG, "failed to create wifi event group (heap pressure?)");
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // FIX #1 — THE ACTUAL BUG: default storage mode is WIFI_STORAGE_FLASH, which
    // makes the driver persist connection state to its own NVS namespace on
    // every state transition (including auth timeouts/retries). That NVS write
    // briefly disables the flash cache on this core; if the driver's own
    // internal logger (wifi_log) tries to fetch a format string from flash at
    // that exact moment, you get "Cache disabled but cached memory region
    // accessed" — which is the crash in your last two boot logs. RAM-only
    // storage removes the flash write entirely, closing the race window.
    // Must be called after esp_wifi_init() and before esp_wifi_start().
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = { 0 };
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_wifi_stack_initialized = true;
    ESP_LOGI(TAG, "Wi-Fi init done, connecting to '%s'...", ssid);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(timeout_ms));

    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }

    // FIX #3: don't leave the radio armed and "silently" running after we've
    // told the rest of the system we're offline. Without this, the WiFi driver
    // keeps doing internal reconnect/power-save activity in the background —
    // meaning the exact crash surface above stays live for the entire runtime
    // instead of being confined to this 8-second boot window. If it fires
    // later, it fires mid face-scan or mid-unlock instead of here where you
    // can see it on serial.
    ESP_LOGW(TAG, "Wi-Fi connection failed or timed out — stopping radio, running fully offline");
    esp_wifi_stop();
    return ESP_FAIL;
}