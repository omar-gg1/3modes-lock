#include "keypad_ctrl.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "keypad_ctrl";

// Row pins driven LOW one at a time to scan.
// Mapping per multimeter-verified keypad pinout:
//   Row 0 = keypad pin 8 (leftmost when facing keypad)
//   Row 1 = keypad pin 7
//   Row 2 = keypad pin 6
//   Row 3 = keypad pin 5
static const gpio_num_t ROW_PINS[4] = {
    GPIO_NUM_38,  // Row 0 (top — has 1,2,3,A)
    GPIO_NUM_39,  // Row 1 (has 4,5,6,B)
    GPIO_NUM_40,  // Row 2 (has 7,8,9,C)
    GPIO_NUM_41,  // Row 3 (bottom — has *,0,#,D)
};

// Column pins read as inputs with internal pull-up.
// Mapping per multimeter-verified keypad pinout:
//   Col 0 = keypad pin 4
//   Col 1 = keypad pin 3
//   Col 2 = keypad pin 2
//   Col 3 = keypad pin 1 (rightmost when facing keypad)
// Note: GPIO 48 was originally Col 2 but had onboard RGB LED interference
// (stuck LOW at idle). Moved to GPIO 1 which is a clean general-purpose pin.
static const gpio_num_t COL_PINS[4] = {
    GPIO_NUM_42,  // Col 0 (has 1,4,7,*)
    GPIO_NUM_47,  // Col 1 (has 2,5,8,0)
    GPIO_NUM_1,   // Col 2 (has 3,6,9,#)
    GPIO_NUM_2,   // Col 3 (has A,B,C,D)
};

// Standard 4x4 membrane keypad layout.
static const char KEYMAP[4][4] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'},
};

// Track the last key seen so we only fire on edges (key just went from
// not-pressed to pressed). Without this, holding a key would spam.
static char last_key = 0;

// Software debounce: ignore key changes happening within this window.
#define DEBOUNCE_MS 30
static int64_t last_change_us = 0;

esp_err_t keypad_ctrl_init(void) {
    // Configure rows as outputs, idle HIGH.
    gpio_config_t row_conf = {
        .pin_bit_mask = (1ULL << GPIO_NUM_38) | (1ULL << GPIO_NUM_39)
                      | (1ULL << GPIO_NUM_40) | (1ULL << GPIO_NUM_41),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&row_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "row gpio_config failed: 0x%x", err);
        return err;
    }
    for (int i = 0; i < 4; i++) {
        gpio_set_level(ROW_PINS[i], 1);
    }

    // Configure columns as inputs with internal pull-up.
    gpio_config_t col_conf = {
        .pin_bit_mask = (1ULL << GPIO_NUM_42) | (1ULL << GPIO_NUM_47)
                      | (1ULL << GPIO_NUM_1)  | (1ULL << GPIO_NUM_2),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&col_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "col gpio_config failed: 0x%x", err);
        return err;
    }

    ESP_LOGI(TAG, "Keypad initialized (rows 38-41, cols 42/47/1/2)");
    return ESP_OK;
}

// Scan the matrix once and return the currently-pressed key, or 0.
// If multiple keys are pressed simultaneously, returns the first one found.
static char scan_matrix(void) {
    for (int r = 0; r < 4; r++) {
        // Drive this row LOW, others HIGH.
        for (int i = 0; i < 4; i++) {
            gpio_set_level(ROW_PINS[i], i == r ? 0 : 1);
        }
        // Tiny settling delay so the column reading is stable.
        esp_rom_delay_us(5);

        for (int c = 0; c < 4; c++) {
            if (gpio_get_level(COL_PINS[c]) == 0) {
                // Restore all rows HIGH before returning.
                for (int i = 0; i < 4; i++) {
                    gpio_set_level(ROW_PINS[i], 1);
                }
                return KEYMAP[r][c];
            }
        }
    }
    // Nothing pressed.
    for (int i = 0; i < 4; i++) {
        gpio_set_level(ROW_PINS[i], 1);
    }
    return 0;
}

char keypad_ctrl_scan(void) {
    char current = scan_matrix();
    int64_t now_us = esp_timer_get_time();

    // Edge detection: only return a key when the pressed key changes from
    // "nothing" to "something". Holding a key returns 0 after the first hit.
    if (current != last_key) {
        // Debounce: require the state to be stable for DEBOUNCE_MS.
        if ((now_us - last_change_us) / 1000 < DEBOUNCE_MS) {
            return 0;
        }
        last_change_us = now_us;
        char fired = (current != 0 && last_key == 0) ? current : 0;
        last_key = current;
        return fired;
    }

    return 0;
}

void keypad_ctrl_debug_dump(void) {
    // Drive ALL rows LOW. Then any pressed key should pull its column LOW.
    for (int i = 0; i < 4; i++) {
        gpio_set_level(ROW_PINS[i], 0);
    }
    esp_rom_delay_us(10);

    int c0 = gpio_get_level(GPIO_NUM_42);
    int c1 = gpio_get_level(GPIO_NUM_47);
    int c2 = gpio_get_level(GPIO_NUM_1);
    int c3 = gpio_get_level(GPIO_NUM_2);

    // Restore rows HIGH.
    for (int i = 0; i < 4; i++) {
        gpio_set_level(ROW_PINS[i], 1);
    }

    ESP_LOGI(TAG, "COLS: c0(42)=%d c1(47)=%d c2(1)=%d c3(2)=%d", c0, c1, c2, c3);
}