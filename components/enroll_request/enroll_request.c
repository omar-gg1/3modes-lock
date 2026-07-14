#include "enroll_request.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Shared between the MQTT task (set) and the main loop (take). The critical
// section is trivially short (two ints + a flag), so a spinlock is the right
// tool — no queue/semaphore overhead for a single-slot latch.
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_pending = false;
static volatile int  s_user_id = 0;
static volatile int  s_samples = 0;

void enroll_request_set(int user_id, int samples)
{
    portENTER_CRITICAL(&s_mux);
    s_user_id = user_id;
    s_samples = samples;
    s_pending = true;
    portEXIT_CRITICAL(&s_mux);
}

bool enroll_request_take(int *user_id, int *samples)
{
    bool had = false;
    portENTER_CRITICAL(&s_mux);
    if (s_pending) {
        if (user_id) *user_id = s_user_id;
        if (samples) *samples = s_samples;
        s_pending = false;
        had = true;
    }
    portEXIT_CRITICAL(&s_mux);
    return had;
}

// Separate single-slot latch for the WiFi QR scan trigger. Shares the same
// spinlock — both are set from the MQTT task, taken from the main loop.
static volatile bool s_wifi_scan_pending = false;

void wifi_scan_request_set(void)
{
    portENTER_CRITICAL(&s_mux);
    s_wifi_scan_pending = true;
    portEXIT_CRITICAL(&s_mux);
}

bool wifi_scan_request_take(void)
{
    portENTER_CRITICAL(&s_mux);
    bool had = s_wifi_scan_pending;
    s_wifi_scan_pending = false;
    portEXIT_CRITICAL(&s_mux);
    return had;
}
