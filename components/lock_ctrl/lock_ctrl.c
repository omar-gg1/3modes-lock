#include "lock_ctrl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define LOCK_PIN GPIO_NUM_21
#define LEVEL_LOCKED   1
#define LEVEL_UNLOCKED 0
#define UNLOCK_DURATION_MS 3000

static const char *TAG = "lock_ctrl";
static volatile bool unlock_in_progress = false;

static void lock_set_unlocked(void) {
    gpio_set_level(LOCK_PIN, LEVEL_UNLOCKED);
    ESP_LOGI(TAG, ">>> UNLOCKED");
}

static void lock_set_locked(void) {
    gpio_set_level(LOCK_PIN, LEVEL_LOCKED);
    ESP_LOGI(TAG, ">>> LOCKED");
}

esp_err_t lock_ctrl_init(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LOCK_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: 0x%x", err);
        return err;
    }
    gpio_set_level(LOCK_PIN, LEVEL_LOCKED);
    ESP_LOGI(TAG, "Lock initialized in LOCKED state (GPIO %d)", LOCK_PIN);
    return ESP_OK;
}

void lock_ctrl_trigger_unlock(const char *reason) {
    if (unlock_in_progress) {
        ESP_LOGW(TAG, "Unlock by '%s' ignored — already in progress", reason);
        return;
    }
    unlock_in_progress = true;
    ESP_LOGI(TAG, "Unlock triggered by: %s", reason);
    lock_set_unlocked();
    vTaskDelay(pdMS_TO_TICKS(UNLOCK_DURATION_MS));
    lock_set_locked();
    unlock_in_progress = false;
}