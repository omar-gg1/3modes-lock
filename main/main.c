#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_camera.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs_flash.h"

// === Wi-Fi credentials ===
#define WIFI_SSID "SPACEDOME"
#define WIFI_PASS "spacedome@2021"
#define WIFI_MAX_RETRY 5

// === Lock control ===
#define LOCK_PIN GPIO_NUM_21
#define BUTTON_PIN GPIO_NUM_0
#define LEVEL_LOCKED   1
#define LEVEL_UNLOCKED 0
#define UNLOCK_DURATION_MS 3000

// === Camera pin map (Gooouuu Tech ESP32-S3-CAM, OV3660) ===
#define CAM_PIN_PWDN    -1
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK    15
#define CAM_PIN_SIOD    4
#define CAM_PIN_SIOC    5
#define CAM_PIN_D7      16
#define CAM_PIN_D6      17
#define CAM_PIN_D5      18
#define CAM_PIN_D4      12
#define CAM_PIN_D3      10
#define CAM_PIN_D2      8
#define CAM_PIN_D1      9
#define CAM_PIN_D0      11
#define CAM_PIN_VSYNC   6
#define CAM_PIN_HREF    7
#define CAM_PIN_PCLK    13

static const char *TAG = "lock";
static volatile bool unlock_in_progress = false;

// Wi-Fi event handling
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static int s_retry_num = 0;

// === Lock functions ===
void lock_init(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LOCK_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(LOCK_PIN, LEVEL_LOCKED);
    ESP_LOGI(TAG, "Lock initialized in LOCKED state");
}

void button_init(void) {
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << BUTTON_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_conf);
    ESP_LOGI(TAG, "Button initialized on GPIO 0 (BOOT)");
}

void lock_unlock(void) {
    gpio_set_level(LOCK_PIN, LEVEL_UNLOCKED);
    ESP_LOGI(TAG, ">>> UNLOCKED");
}

void lock_lock(void) {
    gpio_set_level(LOCK_PIN, LEVEL_LOCKED);
    ESP_LOGI(TAG, ">>> LOCKED");
}

void trigger_unlock(const char *reason) {
    if (unlock_in_progress) {
        ESP_LOGW(TAG, "Unlock requested by '%s' but already in progress, ignoring", reason);
        return;
    }
    unlock_in_progress = true;
    ESP_LOGI(TAG, "Unlock triggered by: %s", reason);
    lock_unlock();
    vTaskDelay(pdMS_TO_TICKS(UNLOCK_DURATION_MS));
    lock_lock();
    unlock_in_progress = false;
}

// === Camera initialization ===
esp_err_t camera_init(void) {
    camera_config_t config = {
        .pin_pwdn = CAM_PIN_PWDN,
        .pin_reset = CAM_PIN_RESET,
        .pin_xclk = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d7 = CAM_PIN_D7,
        .pin_d6 = CAM_PIN_D6,
        .pin_d5 = CAM_PIN_D5,
        .pin_d4 = CAM_PIN_D4,
        .pin_d3 = CAM_PIN_D3,
        .pin_d2 = CAM_PIN_D2,
        .pin_d1 = CAM_PIN_D1,
        .pin_d0 = CAM_PIN_D0,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href = CAM_PIN_HREF,
        .pin_pclk = CAM_PIN_PCLK,
        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = FRAMESIZE_VGA,    // bumped to 640x480 for better viewing
        .jpeg_quality = 12,
        .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
    };
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init FAILED with error 0x%x", err);
        return err;
    }
    ESP_LOGI(TAG, "Camera initialized successfully");
    return ESP_OK;
}

// === Wi-Fi setup ===
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Wi-Fi retry %d/%d", s_retry_num, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "============================================");
        ESP_LOGI(TAG, "Wi-Fi connected. IP: " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Open in browser: http://" IPSTR "/", IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "============================================");
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void) {
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Wi-Fi init done, connecting to '%s'...", WIFI_SSID);
}

// === HTTP handlers ===
static esp_err_t capture_handler(httpd_req_t *req) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "Camera capture failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    return res;
}

static esp_err_t stream_handler(httpd_req_t *req) {
    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;
    char part_buf[128];
    static const char *BOUNDARY = "123456789000000000000987654321";
    static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=123456789000000000000987654321";
    
    res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    if (res != ESP_OK) return res;

    while (true) {
        fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGE(TAG, "Camera capture failed in stream");
            res = ESP_FAIL;
            break;
        }
        size_t hlen = snprintf(part_buf, sizeof(part_buf),
            "\r\n--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
            BOUNDARY, (unsigned int)fb->len);
        if (httpd_resp_send_chunk(req, part_buf, hlen) != ESP_OK) { res = ESP_FAIL; }
        if (res == ESP_OK && httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len) != ESP_OK) { res = ESP_FAIL; }
        esp_camera_fb_return(fb);
        if (res != ESP_OK) break;
    }
    return res;
}

static esp_err_t index_handler(httpd_req_t *req) {
    const char *html =
        "<!DOCTYPE html><html><head><title>Smart Lock Debug</title>"
        "<style>body{font-family:sans-serif;background:#111;color:#eee;text-align:center;}"
        "img{max-width:90%;border:2px solid #444;}h1{color:#0f0;}</style></head>"
        "<body><h1>Smart Lock Camera Debug</h1>"
        "<p>Live stream:</p><img src='/stream'/>"
        "<p><a href='/capture' style='color:#0af;'>Single snapshot</a></p>"
        "</body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, strlen(html));
}

void start_webserver(void) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32768;
    config.stack_size = 8192;
    
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t index_uri = { .uri = "/", .method = HTTP_GET, .handler = index_handler };
        httpd_uri_t capture_uri = { .uri = "/capture", .method = HTTP_GET, .handler = capture_handler };
        httpd_uri_t stream_uri = { .uri = "/stream", .method = HTTP_GET, .handler = stream_handler };
        httpd_register_uri_handler(server, &index_uri);
        httpd_register_uri_handler(server, &capture_uri);
        httpd_register_uri_handler(server, &stream_uri);
        ESP_LOGI(TAG, "HTTP server started on port 80");
    } else {
        ESP_LOGE(TAG, "Failed to start HTTP server");
    }
}

// === Main ===
void app_main(void) {
    ESP_LOGI(TAG, "Smart lock booting...");
    
    // NVS init (required for Wi-Fi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    lock_init();
    button_init();
    
    if (camera_init() != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed. Continuing without camera.");
    }
    
    wifi_init_sta();
    
    // Wait for Wi-Fi (max 10 sec)
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
    
    if (bits & WIFI_CONNECTED_BIT) {
        start_webserver();
    } else {
        ESP_LOGW(TAG, "Wi-Fi failed, continuing without web server");
    }
    
    ESP_LOGI(TAG, "System ready. Press BOOT to unlock.");

    int last_state = 1;
    while (1) {
        int current_state = gpio_get_level(BUTTON_PIN);
        if (last_state == 1 && current_state == 0) {
            trigger_unlock("BOOT button press");
        }
        last_state = current_state;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}