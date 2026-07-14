#include "door_pin.h"

#include <string.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "nvs.h"

// Factory default — matches the old compile-time UNLOCK_PIN. Only used on the
// very first boot before the owner sets their own via the app.
#define DOOR_PIN_DEFAULT "1234"
#define DOOR_PIN_MIN     4
#define DOOR_PIN_MAX     8
#define DOOR_NVS_NS      "door"
#define DOOR_NVS_KEY     "pin"

// Guarded like temp_pin: the MQTT task writes (door_pin_set) while the main loop
// reads (door_pin_matches). Critical section is a short string copy.
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_pin[DOOR_PIN_MAX + 1] = DOOR_PIN_DEFAULT;

// 4-8 ASCII digits, nothing else.
static bool valid_pin(const char *pin)
{
    if (pin == NULL) return false;
    size_t n = strlen(pin);
    if (n < DOOR_PIN_MIN || n > DOOR_PIN_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        if (!isdigit((unsigned char) pin[i])) return false;
    }
    return true;
}

void door_pin_load(void)
{
    nvs_handle_t h;
    if (nvs_open(DOOR_NVS_NS, NVS_READONLY, &h) != ESP_OK) return; // keep default
    char buf[DOOR_PIN_MAX + 1] = {0};
    size_t len = sizeof(buf);
    if (nvs_get_str(h, DOOR_NVS_KEY, buf, &len) == ESP_OK && valid_pin(buf)) {
        portENTER_CRITICAL(&s_mux);
        strncpy(s_pin, buf, DOOR_PIN_MAX);
        s_pin[DOOR_PIN_MAX] = '\0';
        portEXIT_CRITICAL(&s_mux);
    }
    nvs_close(h);
}

bool door_pin_matches(const char *entered)
{
    if (entered == NULL || entered[0] == '\0') return false;
    portENTER_CRITICAL(&s_mux);
    bool ok = (strcmp(entered, s_pin) == 0);
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

bool door_pin_set(const char *pin)
{
    if (!valid_pin(pin)) return false; // never blank it out or accept junk

    // Persist first; only adopt in RAM if the write took, so a failed write
    // can't leave RAM ahead of flash.
    nvs_handle_t h;
    if (nvs_open(DOOR_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_str(h, DOOR_NVS_KEY, pin);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return false;

    portENTER_CRITICAL(&s_mux);
    strncpy(s_pin, pin, DOOR_PIN_MAX);
    s_pin[DOOR_PIN_MAX] = '\0';
    portEXIT_CRITICAL(&s_mux);
    return true;
}
