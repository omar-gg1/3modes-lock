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
#include "liveness_ctrl.h"

#define ENROLLMENT_TIMEOUT_MS 10000
#define FACE_MATCH_THRESHOLD 0.6f

// Master switch for the liveness challenge. 1 = require a head-turn after a face
// match before unlocking (anti-photo-spoof). 0 = face match alone unlocks
// (snappy daily use; flip to 1 for the anti-spoof demo). Kept ON now that the
// check is patient + cumulative, with this as an escape hatch.
#define LIVENESS_ENABLED   1

// After a face matches, the user has this long to complete the head-turn before
// the attempt is abandoned. Widened to 12s: detections are sparse (~1/s), so the
// pose change needs time to accumulate across frames. The liveness check passes
// as soon as the turn is detected, so a quick turn unlocks well before this.
#define LIVENESS_WINDOW_MS 12000
#define KEYPOINT_BUF_LEN   10

// Multi-sample enrollment: capture this many quality-gated templates per user,
// giving up after the capture window. More samples = more robust recognition.
// Window is generous because each detect+gate iteration takes ~1s; 12s lets all
// 5 samples land even when some frames are rejected by the quality gate.
#define ENROLL_SAMPLES     5
#define ENROLL_CAPTURE_MS  12000

// After any successful unlock, fully PAUSE the face pipeline for this long. The
// lock holds open ~3s; we pause a bit longer so the person can walk through
// without the device re-recognizing them and spamming detections/log. This is
// the "stop recognition after a match" behavior — recognition, detection AND
// liveness are all skipped during cooldown.
#define UNLOCK_COOLDOWN_MS 5000

// The face pipeline is an explicit state machine so recognition, the liveness
// challenge, and the post-unlock rest never run at the same time (which is what
// caused the detector to thrash and flood the terminal).
typedef enum {
    FACE_SCANNING = 0,   // look for + recognize a face (no liveness running)
    FACE_LIVENESS,       // a face matched; run ONLY the liveness challenge
    FACE_COOLDOWN,       // just unlocked/denied; pause the whole pipeline
} face_state_t;

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

    // Face pipeline state machine. Only one phase runs at a time.
    face_state_t face_state = FACE_SCANNING;
    int64_t liveness_started_at_us = 0;
    int64_t cooldown_started_at_us = 0;
    // Diagnostics: why frames are lost during a liveness challenge.
    int lv_bad_jpeg = 0, lv_no_face = 0, lv_no_frame = 0;

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

        // ---- Enrollment takes priority over the normal pipeline ----
        // (Armed by *9999#. Runs a blocking multi-sample capture, then returns
        // to scanning.) Done before the state machine so it can interrupt any
        // state cleanly.
        if (enrollment_armed) {
            if (face_ctrl_detect_once()) {
                if (face_ctrl_enroll_multi(0, ENROLL_SAMPLES, ENROLL_CAPTURE_MS) == ESP_OK) {
                    ESP_LOGI(TAG, ">>> ENROLLED new face (multi-sample)");
                }
                enrollment_armed = false;
                face_state = FACE_SCANNING;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;  // skip the normal pipeline while enrolling
        }

        // ---- Face pipeline state machine ----
        // Exactly one phase runs per iteration, so recognition, the liveness
        // challenge, and the post-unlock rest never trample each other.
        switch (face_state) {

        case FACE_COOLDOWN: {
            // Pause the WHOLE face pipeline (no detect/recognize/liveness) so a
            // just-unlocked person can walk through without re-triggering.
            int64_t elapsed_ms = (esp_timer_get_time() - cooldown_started_at_us) / 1000;
            if (elapsed_ms > UNLOCK_COOLDOWN_MS) {
                ESP_LOGI(TAG, "Resuming face scan");
                face_state = FACE_SCANNING;
            }
            break;  // crucially: do NOT run the camera/detector this iteration
        }

        case FACE_LIVENESS: {
            // A face already matched; run ONLY the liveness challenge now — no
            // recognition. Feed each frame's keypoints until a verdict or the
            // window expires. Count WHY frames are lost so a timeout tells us if
            // the detector is starved by bad frames or genuinely sees no face.
            if (face_ctrl_detect_once()) {
                int kp[KEYPOINT_BUF_LEN];
                int kp_count = 0;
                if (face_ctrl_get_keypoints(kp, KEYPOINT_BUF_LEN, &kp_count) == ESP_OK) {
                    liveness_result_t lv = liveness_ctrl_feed(kp, kp_count);
                    if (lv == LIVENESS_PASS) {
                        lock_ctrl_trigger_unlock("face match + liveness");
                        cooldown_started_at_us = esp_timer_get_time();
                        face_state = FACE_COOLDOWN;
                    } else if (lv == LIVENESS_FAIL_STATIC) {
                        ESP_LOGW(TAG, ">>> DENIED: static image (photo spoof) suspected");
                        cooldown_started_at_us = esp_timer_get_time();
                        face_state = FACE_COOLDOWN;
                    }
                    // LIVENESS_PENDING: keep feeding frames.
                }
            } else {
                switch (face_ctrl_last_detect_fail()) {
                    case 1: lv_no_frame++; break;
                    case 2: lv_bad_jpeg++; break;
                    default: lv_no_face++;  break;
                }
            }
            // Single-window timeout: deny ONCE and rest (cooldown), rather than
            // instantly re-arming and spamming repeated DENIED messages.
            int64_t elapsed_ms = (esp_timer_get_time() - liveness_started_at_us) / 1000;
            if (face_state == FACE_LIVENESS && elapsed_ms > LIVENESS_WINDOW_MS) {
                ESP_LOGW(TAG, ">>> DENIED: liveness timeout "
                         "(bad_jpeg=%d no_face=%d no_frame=%d) — resting",
                         lv_bad_jpeg, lv_no_face, lv_no_frame);
                cooldown_started_at_us = esp_timer_get_time();
                face_state = FACE_COOLDOWN;
            }
            break;
        }

        case FACE_SCANNING:
        default: {
            // Look for a face and recognize it. On a confident match, hand off
            // to the liveness challenge (recognition stops there).
            if (face_ctrl_detect_once() && face_ctrl_last_is_good_quality()) {
                int matched_id;
                float similarity;
                if (face_ctrl_recognize(&matched_id, &similarity) == ESP_OK) {
                    ESP_LOGI(TAG, "match: id=%d similarity=%.3f", matched_id, similarity);
                    if (similarity > FACE_MATCH_THRESHOLD) {
                        if (LIVENESS_ENABLED) {
                            // Hand off to the liveness challenge (recognition stops).
                            ESP_LOGI(TAG, ">>> Face matched (user %d). Liveness: TURN YOUR HEAD",
                                     matched_id);
                            liveness_ctrl_begin();
                            liveness_started_at_us = esp_timer_get_time();
                            lv_bad_jpeg = lv_no_face = lv_no_frame = 0;
                            face_state = FACE_LIVENESS;
                        } else {
                            // Liveness disabled: a confident match unlocks directly.
                            ESP_LOGI(TAG, ">>> Face matched (user %d) — unlocking", matched_id);
                            lock_ctrl_trigger_unlock("face match");
                            cooldown_started_at_us = esp_timer_get_time();
                            face_state = FACE_COOLDOWN;
                        }
                    }
                }
            }
            break;
        }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}