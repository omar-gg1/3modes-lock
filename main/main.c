#include <string.h>
#include <math.h>
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
#include "mqtt_ctrl.h"
#include "wifi_config.h"

#define ENROLLMENT_TIMEOUT_MS 10000
#define FACE_MATCH_THRESHOLD 0.6f

// ---- Mode 2 (Hybrid) reporting ----
// 1 = connect Wi-Fi and report each access event to the backend over MQTT.
// 0 = pure Mode 1 (offline, no network). Reporting is a SIDE EFFECT only: the
// lock decides + acts locally regardless, and never blocks on the network. If
// Wi-Fi/broker is unavailable at boot or at runtime, events are silently dropped
// and the lock keeps working exactly as in Mode 1. See [[mode2-backend]].
#define MODE2_REPORTING_ENABLED 1
// How long to wait for Wi-Fi at boot before giving up and running offline. Short
// so a missing network barely delays startup.
#define WIFI_CONNECT_TIMEOUT_MS 8000

// Master switch for the liveness challenge. 1 = require a brief frontal-motion
// challenge after a face match before unlocking (anti-photo-spoof). 0 = face
// match alone unlocks (snappy daily use; flip to 1 for the anti-spoof demo).
// Kept ON now that the challenge keeps the face frontal so the detector no
// longer loses it (the head-turn version did), with this as an escape hatch.
#define LIVENESS_ENABLED   1

// After a face matches, the user has this long to complete the frontal-motion
// challenge before the attempt is abandoned. The face stays frontal so detection
// is dense now (~10-20 frames/s), and the check passes as soon as enough natural
// micro-motion accumulates — a small lean/movement unlocks well before this.
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

// ---- Second-factor confirm PIN (anti-spoof fallback) ----
// Liveness alone cannot reliably reject a hand-held photo on this 5-keypoint
// detector (measured: a shaken photo's keypoint NOISE deforms the geometry MORE
// than a real face's parallax, so no threshold separates them — see
// [[liveness-decision]]). So after a face+liveness PASS we DON'T unlock directly:
// we demand a short secret PIN as a second factor. A photo can pass the camera
// but cannot type the PIN, so a photo alone never opens the lock.
//
// 1 = require the confirm PIN after liveness; 0 = liveness PASS unlocks directly
// (old behaviour / snappy demo). Toggleable just like LIVENESS_ENABLED.
#define CONFIRM_PIN_ENABLED 1

// The confirm PIN itself — SEPARATE from UNLOCK_PIN (1234) and ENROLL_PIN (9999).
// Committed with '#', same as the others.
#define CONFIRM_PIN     "0000"

// How long the user has to type the confirm PIN after passing liveness. Short by
// design (a second factor should be quick); adjustable.
#define CONFIRM_PIN_WINDOW_MS 5000

// After this many wrong/timed-out confirm attempts in a row, lock the pipeline
// down for a longer cooldown instead of immediately re-scanning — frustrates a
// spoofer hammering the keypad. Reset to 0 on any successful unlock.
#define CONFIRM_PIN_MAX_STRIKES 3
#define CONFIRM_LOCKOUT_MS      15000

// The face pipeline is an explicit state machine so recognition, the liveness
// challenge, the confirm-PIN step, and the post-unlock rest never run at the
// same time (which is what caused the detector to thrash and flood the terminal).
// Exactly one phase runs per loop iteration: one completes before the next
// starts, so there is no task clash and no wasted RAM.
typedef enum {
    FACE_SCANNING = 0,   // look for + recognize a face (no liveness running)
    FACE_LIVENESS,       // a face matched; run ONLY the liveness challenge
    FACE_CONFIRM_PIN,    // liveness passed; wait ONLY for the secret confirm PIN
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

    // ---- Mode 2 (Hybrid) network bring-up ----
    // Done LAST, after the lock is fully functional, so a missing/slow network
    // never delays or blocks the core lock. On any failure we log and continue
    // in Mode 1 — reporting is optional, the lock is not.
    if (MODE2_REPORTING_ENABLED) {
        if (wifi_ctrl_connect(WIFI_SSID, WIFI_PASSWORD, WIFI_CONNECT_TIMEOUT_MS) == ESP_OK) {
            ESP_LOGI(TAG, "Wi-Fi connected — starting MQTT reporting (Mode 2)");
            mqtt_ctrl_init();  // async; reconnects in the background on its own
        } else {
            ESP_LOGW(TAG, "Wi-Fi unavailable — running OFFLINE (Mode 1), no reporting");
        }
    }

    ESP_LOGI(TAG, "System ready.");
    ESP_LOGI(TAG, "  - Show enrolled face: unlock");
    ESP_LOGI(TAG, "  - Tap BOOT: manual unlock");
    ESP_LOGI(TAG, "  - PIN %s# : unlock", UNLOCK_PIN);
    ESP_LOGI(TAG, "  - PIN *%s# : arm enrollment", ENROLL_PIN);
    if (CONFIRM_PIN_ENABLED) {
        ESP_LOGI(TAG, "  - After face+liveness: enter confirm PIN %s# (2nd factor)",
                 CONFIRM_PIN);
    }

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
    // How long the current cooldown lasts. Normally UNLOCK_COOLDOWN_MS, but a
    // confirm-PIN lockout sets it to CONFIRM_LOCKOUT_MS for a longer rest.
    int     cooldown_ms = UNLOCK_COOLDOWN_MS;
    // The face match that started the current liveness/confirm flow, carried
    // forward so the Mode 2 event we report at unlock/deny names the right user
    // and score even though those values were read back in the scanning state.
    int     pending_face_id = -1;
    float   pending_face_score = NAN;
    // Diagnostics: why frames are lost during a liveness challenge.
    int lv_bad_jpeg = 0, lv_no_face = 0, lv_no_frame = 0;

    // Confirm-PIN (second factor) state. Its own buffer so it never tangles with
    // the normal unlock/enroll PIN entry above. Strikes persist across attempts
    // until a successful unlock resets them, so repeated failures escalate to a
    // longer lockout.
    char confirm_buf[MAX_PIN_LEN + 1] = {0};
    int  confirm_len = 0;
    int64_t confirm_started_at_us = 0;
    int  confirm_strikes = 0;

    while (1) {
        // ---- Keypad input ----
        // Read one key per iteration. While we are in the FACE_CONFIRM_PIN state
        // the key belongs to the confirm-PIN entry (handled inside that state
        // below), so the normal unlock/enroll handler is skipped — this keeps the
        // two PIN flows from fighting over the same buffer.
        char key = keypad_ctrl_scan();
        if (key != 0 && face_state != FACE_CONFIRM_PIN) {
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
                        mqtt_ctrl_publish_event(MQTT_METHOD_PIN, -1, NAN, true);
                    } else {
                        ESP_LOGW(TAG, "Wrong unlock PIN");
                        mqtt_ctrl_publish_event(MQTT_METHOD_PIN, -1, NAN, false);
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
            mqtt_ctrl_publish_event(MQTT_METHOD_BUTTON, -1, NAN, true);
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
            if (elapsed_ms > cooldown_ms) {
                ESP_LOGI(TAG, "Resuming face scan");
                cooldown_ms = UNLOCK_COOLDOWN_MS;  // reset to the normal rest
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
                        if (CONFIRM_PIN_ENABLED) {
                            // Liveness alone can't reject a hand-held photo on
                            // this detector, so require a secret second factor:
                            // hand off to the confirm-PIN phase (lock stays shut).
                            // A photo can pass the camera but cannot type the PIN.
                            confirm_len = 0;
                            confirm_buf[0] = 0;
                            confirm_started_at_us = esp_timer_get_time();
                            ESP_LOGI(TAG, ">>> Liveness OK. Enter confirm PIN within %ds (then #)",
                                     CONFIRM_PIN_WINDOW_MS / 1000);
                            face_state = FACE_CONFIRM_PIN;
                        } else {
                            // Confirm PIN disabled: liveness PASS unlocks directly.
                            lock_ctrl_trigger_unlock("face match + liveness");
                            mqtt_ctrl_publish_event(MQTT_METHOD_FACE,
                                                    pending_face_id, pending_face_score, true);
                            cooldown_started_at_us = esp_timer_get_time();
                            face_state = FACE_COOLDOWN;
                        }
                    } else if (lv == LIVENESS_FAIL_STATIC) {
                        ESP_LOGW(TAG, ">>> DENIED: static image (photo spoof) suspected");
                        mqtt_ctrl_publish_event(MQTT_METHOD_FACE,
                                                pending_face_id, pending_face_score, false);
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
                mqtt_ctrl_publish_event(MQTT_METHOD_FACE,
                                        pending_face_id, pending_face_score, false);
                cooldown_started_at_us = esp_timer_get_time();
                face_state = FACE_COOLDOWN;
            }
            break;
        }

        case FACE_CONFIRM_PIN: {
            // Liveness passed; the ONLY thing that runs now is confirm-PIN entry.
            // No camera/detector this phase — one process at a time. The lock
            // stays CLOSED until the correct PIN is committed within the window.
            //
            // 'key' was read at the top of the loop but the normal handler was
            // skipped for this state, so we consume it here.
            if (key != 0) {
                ESP_LOGI(TAG, "key: %c", key);
                if (key == '*') {
                    // Clear and restart confirm entry.
                    confirm_len = 0;
                    confirm_buf[0] = 0;
                    ESP_LOGI(TAG, "Confirm PIN cleared");
                } else if (key == '#') {
                    confirm_buf[confirm_len] = 0;
                    if (strcmp(confirm_buf, CONFIRM_PIN) == 0) {
                        // Second factor satisfied — unlock for real.
                        confirm_strikes = 0;  // reset on success
                        lock_ctrl_trigger_unlock("face + liveness + confirm PIN");
                        mqtt_ctrl_publish_event(MQTT_METHOD_FACE,
                                                pending_face_id, pending_face_score, true);
                        cooldown_started_at_us = esp_timer_get_time();
                        face_state = FACE_COOLDOWN;
                    } else {
                        confirm_strikes++;
                        ESP_LOGW(TAG, ">>> DENIED: wrong confirm PIN (strike %d/%d)",
                                 confirm_strikes, CONFIRM_PIN_MAX_STRIKES);
                        mqtt_ctrl_publish_event(MQTT_METHOD_FACE,
                                                pending_face_id, pending_face_score, false);
                        // Reset buffer either way; decide rest vs lockout below.
                        confirm_len = 0;
                        confirm_buf[0] = 0;
                        if (confirm_strikes >= CONFIRM_PIN_MAX_STRIKES) {
                            ESP_LOGW(TAG, ">>> LOCKOUT: too many bad confirm PINs — resting %ds",
                                     CONFIRM_LOCKOUT_MS / 1000);
                            confirm_strikes = 0;
                            cooldown_ms = CONFIRM_LOCKOUT_MS;  // longer rest
                        } else {
                            // Wrong but strikes remain: rest briefly, then scan.
                            cooldown_ms = UNLOCK_COOLDOWN_MS;
                        }
                        cooldown_started_at_us = esp_timer_get_time();
                        face_state = FACE_COOLDOWN;
                    }
                } else if (key >= '0' && key <= '9') {
                    if (confirm_len < MAX_PIN_LEN) {
                        confirm_buf[confirm_len++] = key;
                    }
                }
                // Letters ignored, same as the main PIN handler.
            }
            // Window timeout: no/incomplete PIN in time = a strike, then rest.
            int64_t elapsed_ms = (esp_timer_get_time() - confirm_started_at_us) / 1000;
            if (face_state == FACE_CONFIRM_PIN && elapsed_ms > CONFIRM_PIN_WINDOW_MS) {
                confirm_strikes++;
                ESP_LOGW(TAG, ">>> DENIED: confirm PIN timeout (strike %d/%d) — resting",
                         confirm_strikes, CONFIRM_PIN_MAX_STRIKES);
                mqtt_ctrl_publish_event(MQTT_METHOD_FACE,
                                        pending_face_id, pending_face_score, false);
                confirm_len = 0;
                confirm_buf[0] = 0;
                if (confirm_strikes >= CONFIRM_PIN_MAX_STRIKES) {
                    ESP_LOGW(TAG, ">>> LOCKOUT: too many failed confirms — resting %ds",
                             CONFIRM_LOCKOUT_MS / 1000);
                    confirm_strikes = 0;
                    cooldown_ms = CONFIRM_LOCKOUT_MS;  // longer rest
                } else {
                    cooldown_ms = UNLOCK_COOLDOWN_MS;
                }
                cooldown_started_at_us = esp_timer_get_time();
                face_state = FACE_COOLDOWN;
            }
            break;  // no camera/detector this iteration
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
                        // Remember who matched so the eventual unlock/deny event
                        // (reported from a later state) names this user + score.
                        pending_face_id = matched_id;
                        pending_face_score = similarity;
                        if (LIVENESS_ENABLED) {
                            // Hand off to the liveness challenge (recognition stops).
                            ESP_LOGI(TAG, ">>> Face matched (user %d). Liveness: LEAN IN / MOVE A LITTLE",
                                     matched_id);
                            liveness_ctrl_begin();
                            liveness_started_at_us = esp_timer_get_time();
                            lv_bad_jpeg = lv_no_face = lv_no_frame = 0;
                            face_state = FACE_LIVENESS;
                        } else {
                            // Liveness disabled: a confident match unlocks directly.
                            ESP_LOGI(TAG, ">>> Face matched (user %d) — unlocking", matched_id);
                            lock_ctrl_trigger_unlock("face match");
                            mqtt_ctrl_publish_event(MQTT_METHOD_FACE, matched_id, similarity, true);
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