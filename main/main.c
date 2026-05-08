#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define LOCK_PIN GPIO_NUM_2

// Active-LOW relay logic baked in here.
// When we move from on-board LED (active-HIGH) to relay (active-LOW),
// we just flip the LEVEL_LOCKED / LEVEL_UNLOCKED defines below.
#define LEVEL_LOCKED   0   // For on-board LED test: 0 = LED off = "locked"
#define LEVEL_UNLOCKED 1   // For on-board LED test: 1 = LED on = "unlocked"
// When we wire the relay, change to:
//   #define LEVEL_LOCKED   1   // active-LOW relay: HIGH = relay open = lock stays locked
//   #define LEVEL_UNLOCKED 0   // active-LOW relay: LOW = relay closed = solenoid fires

static const char *TAG = "lock";

void lock_init(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LOCK_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(LOCK_PIN, LEVEL_LOCKED);  // start in locked state
    ESP_LOGI(TAG, "Lock initialized in LOCKED state");
}

void lock_unlock(void) {
    gpio_set_level(LOCK_PIN, LEVEL_UNLOCKED);
    ESP_LOGI(TAG, ">>> UNLOCKED");
}

void lock_lock(void) {
    gpio_set_level(LOCK_PIN, LEVEL_LOCKED);
    ESP_LOGI(TAG, ">>> LOCKED");
}

void app_main(void) {
    ESP_LOGI(TAG, "Smart lock booting...");
    lock_init();
    
    // Test cycle: unlock for 3 seconds, lock for 3 seconds, repeat
    while (1) {
        lock_unlock();
        vTaskDelay(pdMS_TO_TICKS(3000));
        lock_lock();
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}