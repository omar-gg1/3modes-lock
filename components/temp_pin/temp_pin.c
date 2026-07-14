#include "temp_pin.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_timer.h"

// Shared between the MQTT task (set) and the main loop (try). Critical section
// is a short string copy + a couple of scalars, so a spinlock is right — same
// reasoning as enroll_request.
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

#define TEMP_PIN_MAX 8
static char    s_pin[TEMP_PIN_MAX + 1] = {0};   // "" => no active PIN
static int64_t s_exp_us = 0;
static bool    s_used   = false;

void temp_pin_set(const char *pin, int ttl_s)
{
    portENTER_CRITICAL(&s_mux);
    if (pin == NULL || pin[0] == '\0' || ttl_s <= 0) {
        s_pin[0] = '\0';
        s_exp_us = 0;
        s_used   = true;
    } else {
        strncpy(s_pin, pin, TEMP_PIN_MAX);
        s_pin[TEMP_PIN_MAX] = '\0';
        s_exp_us = esp_timer_get_time() + (int64_t) ttl_s * 1000000LL;
        s_used   = false;
    }
    portEXIT_CRITICAL(&s_mux);
}

bool temp_pin_try(const char *entered)
{
    if (entered == NULL || entered[0] == '\0') return false;
    bool ok = false;
    portENTER_CRITICAL(&s_mux);
    if (s_pin[0] != '\0' && !s_used) {
        if (esp_timer_get_time() >= s_exp_us) {
            // Expired: clear the stale slot, count as a miss.
            s_pin[0] = '\0';
            s_used   = true;
        } else if (strcmp(entered, s_pin) == 0) {
            s_used = true;           // one-shot: kill it on first accept
            ok = true;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    return ok;
}
