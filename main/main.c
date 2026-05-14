#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "face_ctrl.h"
#include "lock_ctrl.h"
#include "button_ctrl.h"
#include "camera_ctrl.h"
#include "wifi_ctrl.h"
#include "debug_server.h"

#define WIFI_SSID "SPACEDOME"
#define WIFI_PASS "spacedome@2021"

// How long enrollment stays armed after a long-press, in milliseconds.
// User has this long to put their face in front of the camera.
#define ENROLLMENT_TIMEOUT_MS 10000

// Cosine similarity threshold for accepting a face match.
// 0.5 is the published floor; 0.6 leaves daylight before false accepts.
#define FACE_MATCH_THRESHOLD 0.6f

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

    // Wi-Fi disabled — caused heap corruption when WIFI_IRAM_OPT is off.
    // Re-enable later if network features are needed; not required for the demo.
    // if (wifi_ctrl_connect(WIFI_SSID, WIFI_PASS, 10000) == ESP_OK) {
    //     // debug_server_start();
    // } else {
    //     ESP_LOGW(TAG, "No Wi-Fi — continuing offline");
    // }

    ESP_LOGI(TAG, "System ready.");
    ESP_LOGI(TAG, "  - Show enrolled face: unlock");
    ESP_LOGI(TAG, "  - Tap BOOT: manual unlock");
    ESP_LOGI(TAG, "  - Hold BOOT 2s: arm enrollment");

    // Enrollment mode state.
    bool enrollment_armed = false;
    int64_t enrollment_armed_at_us = 0;

    while (1) {
        // Long press: arm enrollment mode. Next detected face will be enrolled.
        if (button_ctrl_long_press_fired()) {
            enrollment_armed = true;
            enrollment_armed_at_us = esp_timer_get_time();
            ESP_LOGI(TAG, ">>> ENROLLMENT ARMED — show your face within %d seconds",
                     ENROLLMENT_TIMEOUT_MS / 1000);
        }

        // Short press: manual unlock (fallback when face fails: low light, mask, etc).
        if (button_ctrl_was_pressed()) {
            lock_ctrl_trigger_unlock("BOOT button press");
        }

        // Enrollment timeout: disarm if no face shown in time.
        if (enrollment_armed) {
            int64_t elapsed_ms = (esp_timer_get_time() - enrollment_armed_at_us) / 1000;
            if (elapsed_ms > ENROLLMENT_TIMEOUT_MS) {
                enrollment_armed = false;
                ESP_LOGI(TAG, "Enrollment timed out — no face detected");
            }
        }

        if (face_ctrl_detect_once()) {
            if (enrollment_armed) {
                // User armed enrollment and is now in frame — enroll them.
                // Note: the recognizer assigns its own internal ID regardless
                // of what we pass here, so the literal 0 is just a placeholder.
                if (face_ctrl_enroll(0) == ESP_OK) {
                    ESP_LOGI(TAG, ">>> ENROLLED new face");
                }
                enrollment_armed = false;
            } else {
                // Normal mode: try to recognize the face.
                int matched_id;
                float similarity;
                esp_err_t r = face_ctrl_recognize(&matched_id, &similarity);
                if (r == ESP_OK) {
                    ESP_LOGI(TAG, "match: id=%d similarity=%.3f",
                             matched_id, similarity);
                    if (similarity > FACE_MATCH_THRESHOLD) {
                        lock_ctrl_trigger_unlock("face match");
                    }
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}