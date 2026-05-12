#include "button_ctrl.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define BUTTON_PIN GPIO_NUM_0

static const char *TAG = "button_ctrl";
static int last_state = 1;

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

bool button_ctrl_was_pressed(void) {
    int current = gpio_get_level(BUTTON_PIN);
    bool pressed = (last_state == 1 && current == 0);  // HIGH -> LOW transition
    last_state = current;
    return pressed;
}