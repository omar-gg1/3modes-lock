#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "face_ctrl.h"
#include "lock_ctrl.h"
#include "button_ctrl.h"
#include "camera_ctrl.h"
#include "wifi_ctrl.h"
#include "debug_server.h"

#define WIFI_SSID "SPACEDOME"
#define WIFI_PASS "spacedome@2021"

static const char *TAG = "main";

static void nvs_init_safe(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

void app_main(void) {
    ESP_LOGI(TAG, "==== Smart Lock booting ====");

    nvs_init_safe();
    lock_ctrl_init();
    button_ctrl_init();
    camera_ctrl_init();
    face_ctrl_init();

    // if (wifi_ctrl_connect(WIFI_SSID, WIFI_PASS, 10000) == ESP_OK) {
    //     // debug_server_start();   <-- comment this line to disable debug interface
    // } else {
    //     ESP_LOGW(TAG, "No Wi-Fi — continuing offline");
    // }

    ESP_LOGI(TAG, "System ready. Press BOOT to unlock.");

    while (1) {
        if (button_ctrl_was_pressed()) {
            lock_ctrl_trigger_unlock("BOOT button press");
        }

        if (face_ctrl_detect_once()) {
    if (!face_ctrl_has_enrolled()) {
        // First-time setup: enroll the first face we see as id=0.
        if (face_ctrl_enroll(0) == ESP_OK) {
            ESP_LOGI(TAG, ">>> ENROLLED first face as user 0");
        }
    } else {
        int matched_id;
        float similarity;
        esp_err_t r = face_ctrl_recognize(&matched_id, &similarity);
        if (r == ESP_OK) {
            ESP_LOGI(TAG, "match: id=%d similarity=%.3f",
                     matched_id, similarity);
            // Threshold to decide if it's really a match.
            // Cosine similarity > 0.5 is typical for face recognition.
            if (similarity > 0.6f) {
                lock_ctrl_trigger_unlock("face match");
            }
        }
    }
}

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}