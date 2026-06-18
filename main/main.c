#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "face_ctrl.h"
#include "lock_ctrl.h"
#include "button_ctrl.h"
#include "camera_ctrl.h"
#include "keypad_ctrl.h"
#include "wifi_ctrl.h"
#include "debug_server.h"
#include "crypto_ctrl.h"

#define ENROLLMENT_TIMEOUT_MS 10000
#define FACE_MATCH_THRESHOLD 0.6f

// PIN codes. Both end with '#' to commit.
// '*' at start of entry = arm enrollment mode.
// '*' mid-entry = clear and restart.
#define UNLOCK_PIN     "1234"
#define ENROLL_PIN     "9999"
#define MAX_PIN_LEN    8

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
    // crypto_ctrl must come up before face_ctrl: face_ctrl_init() decrypts the
    // face database on mount, which needs the AES key loaded/created first.
    ESP_ERROR_CHECK(crypto_ctrl_init());
    lock_ctrl_init();
    button_ctrl_init();
    keypad_ctrl_init();
    camera_ctrl_init();
    face_ctrl_init();

    ESP_LOGI(TAG, "System ready.");
    ESP_LOGI(TAG, "  - Show enrolled face: unlock");
    ESP_LOGI(TAG, "  - Tap BOOT: manual unlock");
    ESP_LOGI(TAG, "  - PIN %s# : unlock", UNLOCK_PIN);
    ESP_LOGI(TAG, "  - PIN *%s# : arm enrollment", ENROLL_PIN);

    bool enrollment_armed = false;
    int64_t enrollment_armed_at_us = 0;

    // PIN entry buffer. Built up as keys are pressed.
    // '*' at start arms enrollment; '*' mid-entry clears.
    // '#' commits the buffer for evaluation.
    char pin_buf[MAX_PIN_LEN + 1] = {0};
    int pin_len = 0;
    bool enroll_mode_prefix = false;

    while (1) {
        // ---- Keypad input ----
        char key = keypad_ctrl_scan();
        if (key != 0) {
            ESP_LOGI(TAG, "key: %c", key);

            if (key == '*') {
                if (pin_len == 0 && !enroll_mode_prefix) {
                    // Empty buffer, fresh entry: arm enrollment PIN.
                    enroll_mode_prefix = true;
                    ESP_LOGI(TAG, "PIN entry: enrollment mode (enter code and #)");
                } else {
                    // Mid-entry: clear and start fresh.
                    pin_len = 0;
                    pin_buf[0] = 0;
                    enroll_mode_prefix = false;
                    ESP_LOGI(TAG, "PIN entry cleared");
                }
            } else if (key == '#') {
                // Commit the PIN. Compare against unlock or enroll code.
                pin_buf[pin_len] = 0;
                if (enroll_mode_prefix) {
                    if (strcmp(pin_buf, ENROLL_PIN) == 0) {
                        enrollment_armed = true;
                        enrollment_armed_at_us = esp_timer_get_time();
                        ESP_LOGI(TAG, ">>> ENROLLMENT ARMED via PIN — show face within %ds",
                                 ENROLLMENT_TIMEOUT_MS / 1000);
                    } else {
                        ESP_LOGW(TAG, "Wrong enrollment PIN");
                    }
                } else {
                    if (strcmp(pin_buf, UNLOCK_PIN) == 0) {
                        lock_ctrl_trigger_unlock("PIN code");
                    } else {
                        ESP_LOGW(TAG, "Wrong unlock PIN");
                    }
                }
                pin_len = 0;
                pin_buf[0] = 0;
                enroll_mode_prefix = false;
            } else if (key >= '0' && key <= '9') {
                // Append digit.
                if (pin_len < MAX_PIN_LEN) {
                    pin_buf[pin_len++] = key;
                }
            }
            // Letters (A-D) are ignored for now — reserved for future features.
        }

        // ---- BOOT button: manual unlock fallback ----
        if (button_ctrl_was_pressed()) {
            lock_ctrl_trigger_unlock("BOOT button press");
        }

        // ---- Enrollment timeout ----
        if (enrollment_armed) {
            int64_t elapsed_ms = (esp_timer_get_time() - enrollment_armed_at_us) / 1000;
            if (elapsed_ms > ENROLLMENT_TIMEOUT_MS) {
                enrollment_armed = false;
                ESP_LOGI(TAG, "Enrollment timed out — no face detected");
            }
        }

        // ---- Face pipeline ----
        if (face_ctrl_detect_once()) {
            if (enrollment_armed) {
                if (face_ctrl_enroll(0) == ESP_OK) {
                    ESP_LOGI(TAG, ">>> ENROLLED new face");
                }
                enrollment_armed = false;
            } else {
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