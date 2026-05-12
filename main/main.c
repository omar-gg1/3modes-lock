#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_camera.h"

// === Lock control (unchanged from Iteration 5) ===
#define LOCK_PIN GPIO_NUM_21
#define BUTTON_PIN GPIO_NUM_0
#define LEVEL_LOCKED   1
#define LEVEL_UNLOCKED 0
#define UNLOCK_DURATION_MS 3000

// === Camera pin map for Gooouuu Tech ESP32-S3-CAM (OV3660) ===
// These pins are SPECIFIC to this board. Other clones will differ.
#define CAM_PIN_PWDN    -1   // No power-down pin on this board
#define CAM_PIN_RESET   -1   // No reset pin on this board
#define CAM_PIN_XCLK    15
#define CAM_PIN_SIOD    4    // I2C SDA (camera config)
#define CAM_PIN_SIOC    5    // I2C SCL (camera config)
#define CAM_PIN_D7      16
#define CAM_PIN_D6      17
#define CAM_PIN_D5      18
#define CAM_PIN_D4      12
#define CAM_PIN_D3      10
#define CAM_PIN_D2      8
#define CAM_PIN_D1      9
#define CAM_PIN_D0      11
#define CAM_PIN_VSYNC   6
#define CAM_PIN_HREF    7
#define CAM_PIN_PCLK    13

static const char *TAG = "lock";
static volatile bool unlock_in_progress = false;

// === Lock functions (unchanged) ===
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
        .pull_up_en = GPIO_PULLUP_ENABLE,
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

// === Camera initialization ===
esp_err_t camera_init(void) {
    camera_config_t config = {
        .pin_pwdn = CAM_PIN_PWDN,
        .pin_reset = CAM_PIN_RESET,
        .pin_xclk = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d7 = CAM_PIN_D7,
        .pin_d6 = CAM_PIN_D6,
        .pin_d5 = CAM_PIN_D5,
        .pin_d4 = CAM_PIN_D4,
        .pin_d3 = CAM_PIN_D3,
        .pin_d2 = CAM_PIN_D2,
        .pin_d1 = CAM_PIN_D1,
        .pin_d0 = CAM_PIN_D0,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href = CAM_PIN_HREF,
        .pin_pclk = CAM_PIN_PCLK,
        
        .xclk_freq_hz = 20000000,        // 20 MHz
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        
        .pixel_format = PIXFORMAT_JPEG,  // JPEG to save memory
        .frame_size = FRAMESIZE_QVGA,    // 320x240, small for testing
        .jpeg_quality = 12,              // 0-63, lower = higher quality
        .fb_count = 2,                   // 2 buffers for smoother capture
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
    };
    
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init FAILED with error 0x%x", err);
        return err;
    }
    
    ESP_LOGI(TAG, "Camera initialized successfully");
    return ESP_OK;
}

void app_main(void) {
    ESP_LOGI(TAG, "Smart lock booting...");
    lock_init();
    button_init();
    
    if (camera_init() != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed. Continuing without camera.");
    }
    
    ESP_LOGI(TAG, "System ready. Press BOOT to unlock. Capturing frames every 2 sec.");

    int last_state = 1;
    int frame_count = 0;
    TickType_t last_capture = xTaskGetTickCount();
    
    while (1) {
        // Button check (same as before)
        int current_state = gpio_get_level(BUTTON_PIN);
        if (last_state == 1 && current_state == 0) {
            trigger_unlock("BOOT button press");
        }
        last_state = current_state;
        
        // Capture frame every 2 seconds
        if ((xTaskGetTickCount() - last_capture) > pdMS_TO_TICKS(2000)) {
            camera_fb_t *fb = esp_camera_fb_get();
            if (fb) {
                frame_count++;
                ESP_LOGI(TAG, "Frame %d captured: %d bytes, %dx%d", 
                         frame_count, fb->len, fb->width, fb->height);
                esp_camera_fb_return(fb);
            } else {
                ESP_LOGW(TAG, "Frame capture failed");
            }
            last_capture = xTaskGetTickCount();
        }
        
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}