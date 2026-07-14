#include "mode_ctrl.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"

#define MODE_CTRL_DEFAULT 3     // fresh-flash behaves exactly like the old MODE3_ENABLED=1
#define MODE_NVS_NS       "mode"
#define MODE_NVS_KEY      "active"

static const char *TAG = "mode_ctrl";

// Guarded like confirm_pin: the MQTT task writes while the main loop reads.
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_mode = MODE_CTRL_DEFAULT;

static bool valid_mode(uint8_t m) { return m >= 1 && m <= 3; }

void mode_ctrl_load(void)
{
    nvs_handle_t h;
    if (nvs_open(MODE_NVS_NS, NVS_READONLY, &h) != ESP_OK) return; // keep default
    uint8_t m = MODE_CTRL_DEFAULT;
    if (nvs_get_u8(h, MODE_NVS_KEY, &m) == ESP_OK && valid_mode(m)) {
        portENTER_CRITICAL(&s_mux);
        s_mode = m;
        portEXIT_CRITICAL(&s_mux);
    }
    nvs_close(h);
}

uint8_t mode_ctrl_get(void)
{
    portENTER_CRITICAL(&s_mux);
    uint8_t m = s_mode;
    portEXIT_CRITICAL(&s_mux);
    return m;
}

bool mode_ctrl_set(uint8_t m)
{
    if (!valid_mode(m)) return false;

    // Persist first; only adopt in RAM if the write took, so a failed write
    // can't leave RAM ahead of flash.
    nvs_handle_t h;
    if (nvs_open(MODE_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_u8(h, MODE_NVS_KEY, m);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return false;

    portENTER_CRITICAL(&s_mux);
    s_mode = m;
    portEXIT_CRITICAL(&s_mux);
    return true;
}

bool mode_ctrl_selftest(void)
{
    uint8_t saved = mode_ctrl_get();
    bool pass = mode_ctrl_set(1) && mode_ctrl_get() == 1
             && mode_ctrl_set(2) && mode_ctrl_get() == 2
             && mode_ctrl_set(3) && mode_ctrl_get() == 3
             && !mode_ctrl_set(4) && mode_ctrl_get() == 3;   // reject leaves it at 3
    mode_ctrl_set(saved);                                    // restore
    ESP_LOGI(TAG, "mode_ctrl self-test: %s", pass ? "PASS" : "FAIL");
    return pass;
}
