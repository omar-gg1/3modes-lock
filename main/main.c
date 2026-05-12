#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"

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

    if (wifi_ctrl_connect(WIFI_SSID, WIFI_PASS, 10000) == ESP_OK) {
        debug_server_start();   // <-- comment this line to disable debug interface
    } else {
        ESP_LOGW(TAG, "No Wi-Fi — continuing offline");
    }

    ESP_LOGI(TAG, "System ready. Press BOOT to unlock.");

    while (1) {
        if (button_ctrl_was_pressed()) {
            lock_ctrl_trigger_unlock("BOOT button press");
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}