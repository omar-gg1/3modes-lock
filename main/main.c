#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define LOCK_PIN GPIO_NUM_2
static const char *TAG = "lock_test";

void app_main(void) {
    ESP_LOGI(TAG, "Lock control test starting");
    
    // Configure GPIO 2 as output
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LOCK_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    
    while (1) {
        ESP_LOGI(TAG, "Pin HIGH (LED on / relay would energize if active-HIGH)");
        gpio_set_level(LOCK_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(2000));
        
        ESP_LOGI(TAG, "Pin LOW (LED off / relay would energize if active-LOW)");
        gpio_set_level(LOCK_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}