#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * mqtt_ctrl — Mode 2 (Hybrid) access-event reporter.
 *
 * DESIGN PRINCIPLE: reporting is a SIDE EFFECT, never a dependency. The lock
 * decides and acts entirely locally (Mode 1). After it has acted, it calls
 * mqtt_ctrl_publish_event() to tell the backend what happened. If Wi-Fi or the
 * broker is unavailable, the publish is a no-op that returns immediately — the
 * lock never blocks on the network and degrades silently to Mode 1.
 *
 * The underlying esp-mqtt client runs its own background task and reconnects on
 * its own; callers here never wait on connection state.
 *
 * Event JSON contract (must match backend/app + [[mode2-backend]]):
 *   topic: smartlock/<device_id>/events
 *   {"event":"access","method":"face|pin|button","id":<int|null>,
 *    "score":<float|null>,"result":"granted|denied","ts":<epoch_s>}
 */

// Access method, maps to the "method" field in the JSON.
typedef enum {
    MQTT_METHOD_FACE = 0,
    MQTT_METHOD_PIN,
    MQTT_METHOD_BUTTON,
    MQTT_METHOD_TEMP_PIN,   // OTP guest PIN consumed — distinct so the app can
                            // clear its armed slot and the log reads "Temp PIN"
} mqtt_method_t;

/**
 * @brief Start the MQTT client against the broker in wifi_config.h.
 *        Non-blocking: returns once the client task is started; actual
 *        connection happens asynchronously in the background. Safe to call even
 *        if Wi-Fi is not connected — it will connect when the network is up.
 * @return ESP_OK if the client started, an error if it could not be created.
 */
esp_err_t mqtt_ctrl_init(void);

/**
 * @brief Publish one access event. FIRE-AND-FORGET: if the client is not
 *        connected, the event is dropped and the function returns immediately
 *        without blocking. Never call this on a path that must not stall — but
 *        it is safe to, because it does not wait.
 *
 * @param method  face / pin / button
 * @param id       matched user id, or -1 for "null" (e.g. pin/button/denied)
 * @param score    similarity score, or NaN for "null"
 * @param granted  true => "granted", false => "denied"
 */
void mqtt_ctrl_publish_event(mqtt_method_t method, int id, float score, bool granted);

/**
 * @brief Whether the MQTT client currently reports itself connected. Optional —
 *        for diagnostics/logging only; callers should NOT gate behaviour on it.
 */
bool mqtt_ctrl_is_connected(void);

/**
 * @brief Publish the current WiFi association state so the app can show which
 *        network the lock is on (green/connected vs offline). Fire-and-forget,
 *        same no-op-when-disconnected semantics as publish_event.
 *   {"event":"wifi","ssid":"<name>","connected":<bool>,"ts":<epoch_s>}
 * @param ssid       associated SSID, or "" when not connected
 * @param connected  true => associated with an AP / has IP
 */
void mqtt_ctrl_publish_wifi(const char *ssid, bool connected);

#ifdef __cplusplus
}
#endif
