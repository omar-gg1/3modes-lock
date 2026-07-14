#include "wifi_creds.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "wifi_config.h"   // WIFI_SSID / WIFI_PASSWORD factory defaults (gitignored)

#define WIFI_NVS_NS   "wifi"
#define WIFI_NVS_SSID "ssid"
#define WIFI_NVS_PASS "pass"

// The MQTT/scan task writes while the boot/reconnect path reads. Short string
// copies under a spinlock, same as door_pin/confirm_pin.
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_ssid[WIFI_CREDS_SSID_MAX + 1] = WIFI_SSID;
static char s_pass[WIFI_CREDS_PASS_MAX + 1] = WIFI_PASSWORD;

// Non-empty SSID within the 802.11 length cap. Password may be empty (open
// network) but not over-length.
static bool valid_creds(const char *ssid, const char *pass)
{
    if (ssid == NULL || pass == NULL) return false;
    size_t sn = strlen(ssid);
    if (sn == 0 || sn > WIFI_CREDS_SSID_MAX) return false;
    if (strlen(pass) > WIFI_CREDS_PASS_MAX) return false;
    return true;
}

void wifi_creds_load(void)
{
    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NS, NVS_READONLY, &h) != ESP_OK) return; // keep defaults

    char ssid[WIFI_CREDS_SSID_MAX + 1] = {0};
    char pass[WIFI_CREDS_PASS_MAX + 1] = {0};
    size_t sl = sizeof(ssid), pl = sizeof(pass);
    esp_err_t es = nvs_get_str(h, WIFI_NVS_SSID, ssid, &sl);
    esp_err_t ep = nvs_get_str(h, WIFI_NVS_PASS, pass, &pl);
    nvs_close(h);

    // Only adopt if the SSID read cleanly and is valid; a missing pass key means
    // "open network" (empty). Never let a partial read blank a working SSID.
    if (es == ESP_OK && valid_creds(ssid, ep == ESP_OK ? pass : "")) {
        portENTER_CRITICAL(&s_mux);
        strncpy(s_ssid, ssid, WIFI_CREDS_SSID_MAX);
        s_ssid[WIFI_CREDS_SSID_MAX] = '\0';
        strncpy(s_pass, ep == ESP_OK ? pass : "", WIFI_CREDS_PASS_MAX);
        s_pass[WIFI_CREDS_PASS_MAX] = '\0';
        portEXIT_CRITICAL(&s_mux);
    }
}

void wifi_creds_get(char *ssid_out, char *pass_out)
{
    portENTER_CRITICAL(&s_mux);
    if (ssid_out) {
        strncpy(ssid_out, s_ssid, WIFI_CREDS_SSID_MAX);
        ssid_out[WIFI_CREDS_SSID_MAX] = '\0';
    }
    if (pass_out) {
        strncpy(pass_out, s_pass, WIFI_CREDS_PASS_MAX);
        pass_out[WIFI_CREDS_PASS_MAX] = '\0';
    }
    portEXIT_CRITICAL(&s_mux);
}

bool wifi_creds_set(const char *ssid, const char *pass)
{
    if (!valid_creds(ssid, pass)) return false;

    // Persist first; only adopt in RAM if both writes took, so a failed write
    // can't leave RAM ahead of flash.
    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_str(h, WIFI_NVS_SSID, ssid);
    if (err == ESP_OK) err = nvs_set_str(h, WIFI_NVS_PASS, pass);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return false;

    portENTER_CRITICAL(&s_mux);
    strncpy(s_ssid, ssid, WIFI_CREDS_SSID_MAX);
    s_ssid[WIFI_CREDS_SSID_MAX] = '\0';
    strncpy(s_pass, pass, WIFI_CREDS_PASS_MAX);
    s_pass[WIFI_CREDS_PASS_MAX] = '\0';
    portEXIT_CRITICAL(&s_mux);
    return true;
}
