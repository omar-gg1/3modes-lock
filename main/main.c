#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define LOCK_PIN GPIO_NUM_21
#define BUTTON_PIN GPIO_NUM_0  // BOOT button on most ESP32-S3 dev boards

// Active-LOW relay logic
#define LEVEL_LOCKED   1
#define LEVEL_UNLOCKED 0

// Unlock duration when triggered
#define UNLOCK_DURATION_MS 3000

static const char *TAG = "lock";
static volatile bool unlock_in_progress = false;

void lock_init(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LOCK_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(LOCK_PIN, LEVEL_LOCKED);
    ESP_LOGI(TAG, "Lock initialized in LOCKED state");
}

void button_init(void) {
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << BUTTON_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,   // BOOT button pulls to GND when pressed
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_conf);
    ESP_LOGI(TAG, "Button initialized on GPIO 0 (BOOT)");
}

void lock_unlock(void) {
    gpio_set_level(LOCK_PIN, LEVEL_UNLOCKED);
    ESP_LOGI(TAG, ">>> UNLOCKED");
}

void lock_lock(void) {
    gpio_set_level(LOCK_PIN, LEVEL_LOCKED);
    ESP_LOGI(TAG, ">>> LOCKED");
}

// Triggered unlock: opens lock for UNLOCK_DURATION_MS, then auto-relocks
void trigger_unlock(const char *reason) {
    if (unlock_in_progress) {
        ESP_LOGW(TAG, "Unlock requested by '%s' but already in progress, ignoring", reason);
        return;
    }
    unlock_in_progress = true;
    ESP_LOGI(TAG, "Unlock triggered by: %s", reason);
    lock_unlock();
    vTaskDelay(pdMS_TO_TICKS(UNLOCK_DURATION_MS));
    lock_lock();
    unlock_in_progress = false;
}

void app_main(void) {
    ESP_LOGI(TAG, "Smart lock booting...");
    lock_init();
    button_init();
    ESP_LOGI(TAG, "Ready. Press BOOT button to unlock.");

    int last_state = 1;  // pulled high by default
    while (1) {
        int current_state = gpio_get_level(BUTTON_PIN);
        // Detect press: HIGH -> LOW transition (BOOT pulled to GND when pressed)
        if (last_state == 1 && current_state == 0) {
            trigger_unlock("BOOT button press");
        }
        last_state = current_state;
        vTaskDelay(pdMS_TO_TICKS(50));  // simple polling debounce
    }
}