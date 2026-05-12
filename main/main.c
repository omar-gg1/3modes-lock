#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define RELAY_PIN GPIO_NUM_21
// GPIO 21 is a plain general-purpose pin, no strapping conflicts,
//    no internal peripheral routing on this board

static const char *TAG = "relay_test";

void app_main(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << RELAY_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    ESP_LOGI(TAG, "Starting relay test on GPIO%d", RELAY_PIN);

    while (1) {
        ESP_LOGI(TAG, "LOW -> relay should CLICK ON");
        gpio_set_level(RELAY_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(3000));

        ESP_LOGI(TAG, "HIGH -> relay should CLICK OFF");
        gpio_set_level(RELAY_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}