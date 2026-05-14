#include "button_ctrl.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

#define BUTTON_PIN GPIO_NUM_0

// 2 seconds. Long enough not to trigger by accident, short enough not to feel awkward.
#define LONG_PRESS_MS 2000

static const char *TAG = "button_ctrl";

// Button state machine:
//   IDLE      → button released, nothing happening
//   PRESSED   → button is currently down, < threshold
//   HELD      → button is currently down, threshold already fired
typedef enum {
    BTN_IDLE,
    BTN_PRESSED,
    BTN_HELD,
} btn_state_t;

static btn_state_t state = BTN_IDLE;
static int64_t press_start_us = 0;

esp_err_t button_ctrl_init(void) {
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << BUTTON_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&btn_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: 0x%x", err);
        return err;
    }
    ESP_LOGI(TAG, "Button initialized on GPIO %d (BOOT)", BUTTON_PIN);
    return ESP_OK;
}

// Read GPIO once per poll cycle; both was_pressed() and long_press_fired()
// share this state but we read separately so call order doesn't matter.
// Active LOW: 1 = released (pull-up), 0 = pressed (button to GND).
static inline bool is_pressed_now(void) {
    return gpio_get_level(BUTTON_PIN) == 0;
}

bool button_ctrl_was_pressed(void) {
    bool down = is_pressed_now();

    switch (state) {
    case BTN_IDLE:
        if (down) {
            state = BTN_PRESSED;
            press_start_us = esp_timer_get_time();
        }
        return false;

    case BTN_PRESSED:
        if (!down) {
            // Released before threshold — that's a short press.
            int64_t held_ms = (esp_timer_get_time() - press_start_us) / 1000;
            state = BTN_IDLE;
            // Cheap debounce: ignore <30ms blips.
            return held_ms >= 30;
        }
        // Still held, threshold not yet fired by long_press_fired().
        return false;

    case BTN_HELD:
        // Long press already fired. Release just returns us to idle —
        // do NOT also report a short press.
        if (!down) {
            state = BTN_IDLE;
        }
        return false;
    }
    return false;
}

bool button_ctrl_long_press_fired(void) {
    bool down = is_pressed_now();

    if (state == BTN_PRESSED && down) {
        int64_t held_ms = (esp_timer_get_time() - press_start_us) / 1000;
        if (held_ms >= LONG_PRESS_MS) {
            state = BTN_HELD;
            return true;
        }
    }
    return false;
}