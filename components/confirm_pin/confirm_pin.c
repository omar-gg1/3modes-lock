#include "confirm_pin.h"

#include <string.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "nvs.h"

// Factory defaults — match the old compile-time CONFIRM_PIN / CONFIRM_PIN_ENABLED.
// Only used on the very first boot before the owner sets their own via the app.
#define CONFIRM_PIN_DEFAULT "0000"
#define CONFIRM_ON_DEFAULT  1
#define CONFIRM_PIN_MIN     4
#define CONFIRM_PIN_MAX     8
#define CONFIRM_NVS_NS      "confirm"
#define CONFIRM_NVS_KEY     "pin"
#define CONFIRM_NVS_ON      "on"

// Guarded like door_pin: the MQTT task writes while the main loop reads. The
// critical section is a short string copy / bool read.
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_pin[CONFIRM_PIN_MAX + 1] = CONFIRM_PIN_DEFAULT;
static bool s_on = CONFIRM_ON_DEFAULT;

// 4-8 ASCII digits, nothing else.
static bool valid_pin(const char *pin)
{
    if (pin == NULL) return false;
    size_t n = strlen(pin);
    if (n < CONFIRM_PIN_MIN || n > CONFIRM_PIN_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        if (!isdigit((unsigned char) pin[i])) return false;
    }
    return true;
}

void confirm_pin_load(void)
{
    nvs_handle_t h;
    if (nvs_open(CONFIRM_NVS_NS, NVS_READONLY, &h) != ESP_OK) return; // keep defaults
    char buf[CONFIRM_PIN_MAX + 1] = {0};
    size_t len = sizeof(buf);
    if (nvs_get_str(h, CONFIRM_NVS_KEY, buf, &len) == ESP_OK && valid_pin(buf)) {
        portENTER_CRITICAL(&s_mux);
        strncpy(s_pin, buf, CONFIRM_PIN_MAX);
        s_pin[CONFIRM_PIN_MAX] = '\0';
        portEXIT_CRITICAL(&s_mux);
    }
    uint8_t on = CONFIRM_ON_DEFAULT;
    if (nvs_get_u8(h, CONFIRM_NVS_ON, &on) == ESP_OK) {
        portENTER_CRITICAL(&s_mux);
        s_on = (on != 0);
        portEXIT_CRITICAL(&s_mux);
    }
    nvs_close(h);
}

bool confirm_pin_matches(const char *entered)
{
    if (entered == NULL || entered[0] == '\0') return false;
    portENTER_CRITICAL(&s_mux);
    bool ok = (strcmp(entered, s_pin) == 0);
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

bool confirm_pin_set(const char *pin)
{
    if (!valid_pin(pin)) return false; // never blank it out or accept junk

    // Persist first; only adopt in RAM if the write took, so a failed write
    // can't leave RAM ahead of flash.
    nvs_handle_t h;
    if (nvs_open(CONFIRM_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_str(h, CONFIRM_NVS_KEY, pin);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return false;

    portENTER_CRITICAL(&s_mux);
    strncpy(s_pin, pin, CONFIRM_PIN_MAX);
    s_pin[CONFIRM_PIN_MAX] = '\0';
    portEXIT_CRITICAL(&s_mux);
    return true;
}

bool confirm_pin_enabled(void)
{
    portENTER_CRITICAL(&s_mux);
    bool on = s_on;
    portEXIT_CRITICAL(&s_mux);
    return on;
}

void confirm_pin_set_enabled(bool on)
{
    // Persist first, then adopt — same ordering guard as the code setter.
    nvs_handle_t h;
    if (nvs_open(CONFIRM_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    esp_err_t err = nvs_set_u8(h, CONFIRM_NVS_ON, on ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return;

    portENTER_CRITICAL(&s_mux);
    s_on = on;
    portEXIT_CRITICAL(&s_mux);
}
