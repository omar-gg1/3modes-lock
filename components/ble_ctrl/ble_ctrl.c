#include "ble_ctrl.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "cJSON.h"
#include "cmd_verify.h"
#include "lock_ctrl.h"
#include "mqtt_ctrl.h"
#include "wifi_config.h"   // MQTT_DEVICE_ID, CMD_HMAC_SECRET

static const char *TAG = "ble_ctrl";

// ---- Fixed 128-bit UUIDs (must match the app's ble_facade constants) --------
// Nixis Lock service and its two characteristics. Byte order is little-endian
// as NimBLE's BLE_UUID128_INIT expects (least-significant byte first), so the
// human-readable UUID is the reverse of the bytes below:
//   service : 4e495849-5300-0000-0000-00000000104c   ("NIXIS" ..'lock')
//   command : 4e495849-5300-0000-0000-0000c0de0000   (write)
//   ack     : 4e495849-5300-0000-0000-0000ac4b0000   (notify)
static const ble_uuid128_t SVC_UUID = BLE_UUID128_INIT(
    0x4c, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x53, 0x49, 0x58, 0x49, 0x4e);
static const ble_uuid128_t CMD_UUID = BLE_UUID128_INIT(
    0x00, 0x00, 0xde, 0xc0, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x53, 0x49, 0x58, 0x49, 0x4e);
static const ble_uuid128_t ACK_UUID = BLE_UUID128_INIT(
    0x00, 0x00, 0x4b, 0xac, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x53, 0x49, 0x58, 0x49, 0x4e);

static uint8_t  s_own_addr_type;
static uint16_t s_ack_handle;     // value-attr handle of the Ack characteristic
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static char     s_dev_name[24];   // "Nixis-<device_id>"

static void ble_advertise(void);

// Notify "<result>/<detail>" back on the Ack characteristic so the phone sees
// the outcome offline. Best-effort: if no one is connected/subscribed it's a
// no-op. Called from the write callback (NimBLE host task).
static void notify_ack(const char *result, const char *detail)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    char msg[32];
    int n = snprintf(msg, sizeof(msg), "%s/%s", result, detail);
    if (n <= 0) return;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(msg, (uint16_t) n);
    if (om == NULL) return;
    ble_gatts_notify_custom(s_conn_handle, s_ack_handle, om);
    ESP_LOGI(TAG, "ble ack -> %s", msg);
}

// Verify a written command blob and, if it passes, unlock. This is the BLE twin
// of mqtt_ctrl's handle_command, deliberately scoped to `unlock` only — BLE is
// an unlock transport, not a general command channel. It reuses the exact same
// cmd_verify core (signing string, constant-time sig compare, single-use nonce
// ring) so the security is byte-for-byte identical to the cloud path.
static void handle_ble_command(const char *json, int len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (root == NULL) { notify_ack("error", "bad_json"); return; }

    const cJSON *jtype  = cJSON_GetObjectItem(root, "type");
    const cJSON *jnonce = cJSON_GetObjectItem(root, "nonce");
    const cJSON *jiat   = cJSON_GetObjectItem(root, "iat");
    const cJSON *jexp   = cJSON_GetObjectItem(root, "exp");
    const cJSON *jsig   = cJSON_GetObjectItem(root, "sig");

    if (!cJSON_IsString(jtype) || !cJSON_IsString(jnonce) ||
        !cJSON_IsNumber(jiat) || !cJSON_IsNumber(jexp) || !cJSON_IsString(jsig)) {
        notify_ack("error", "bad_json");
        cJSON_Delete(root);
        return;
    }
    if (strcmp(jtype->valuestring, "unlock") != 0) {
        // No reason to accept set_mode/enroll/etc. over an anonymous BLE link.
        notify_ack("denied", "unknown_type");
        cJSON_Delete(root);
        return;
    }

    const char *nonce = jnonce->valuestring;
    long long iat = (long long) jiat->valuedouble;
    long long exp = (long long) jexp->valuedouble;

    char sstr[256];
    cmd_build_signing_string(sstr, sizeof(sstr), MQTT_DEVICE_ID, "unlock",
                             nonce, iat, exp, "{}");

    // 1) signature
    if (!cmd_sig_matches(CMD_HMAC_SECRET, sstr, jsig->valuestring)) {
        notify_ack("denied", "bad_sig");
        cJSON_Delete(root);
        return;
    }
    // 2) expiry — needs a real (SNTP-synced) clock. The BLE pass carries a 120s
    // TTL, plus a 5s skew allowance to match the MQTT path.
    time_t now = time(NULL);
    if ((long long) now < 1700000000LL || (long long) now > exp + 5) {
        notify_ack("denied", "expired");
        cJSON_Delete(root);
        return;
    }
    // 3) replay — shared ring with the MQTT path, so a pass used over BLE can't
    // be re-used over either transport.
    if (cmd_nonce_seen_or_record(nonce)) {
        notify_ack("denied", "replay");
        cJSON_Delete(root);
        return;
    }
    // 4) act
    lock_ctrl_trigger_unlock("ble");
    mqtt_ctrl_publish_event(MQTT_METHOD_BLE, -1, NAN, true);
    notify_ack("ok", "unlocked");
    cJSON_Delete(root);
}

// GATT access callback for the Command characteristic (write only).
static int cmd_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len == 0 || len > 200) {
        notify_ack("error", "bad_json");
        return 0;  // accept the write; we've reported the problem over notify
    }
    char buf[201];
    uint16_t copied = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf) - 1, &copied) != 0) {
        notify_ack("error", "bad_json");
        return 0;
    }
    buf[copied] = '\0';
    handle_ble_command(buf, copied);
    return 0;
}

// Ack characteristic needs no read/write access; it is notify-only. NimBLE still
// wants a callback for the value attribute — return not-supported for any op.
static int ack_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return BLE_ATT_ERR_READ_NOT_PERMITTED;
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &CMD_UUID.u,
                .access_cb = cmd_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = &ACK_UUID.u,
                .access_cb = ack_access_cb,
                .val_handle = &s_ack_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 }  // no more characteristics
        },
    },
    { 0 }  // no more services
};

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "phone connected");
        } else {
            ble_advertise();  // connection failed — re-advertise
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "phone disconnected (0x%02x)", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        ble_advertise();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        ble_advertise();
        break;
    default:
        break;
    }
    return 0;
}

static void ble_advertise(void)
{
    struct ble_gap_adv_params advp;
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *) s_dev_name;
    fields.name_len = strlen(s_dev_name);
    fields.name_is_complete = 1;
    if (ble_gap_adv_set_fields(&fields) != 0) {
        ESP_LOGW(TAG, "adv_set_fields failed");
        return;
    }
    memset(&advp, 0, sizeof(advp));
    advp.conn_mode = BLE_GAP_CONN_MODE_UND;   // connectable
    advp.disc_mode = BLE_GAP_DISC_MODE_GEN;   // general discoverable
    int rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                               &advp, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "adv_start failed rc=%d", rc);
    }
}

static void on_sync(void)
{
    if (ble_hs_util_ensure_addr(0) != 0 ||
        ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
        ESP_LOGE(TAG, "no BLE address available");
        return;
    }
    ESP_LOGI(TAG, "advertising as %s", s_dev_name);
    ble_advertise();
}

static void host_task(void *param)
{
    nimble_port_run();   // returns only on nimble_port_stop()
    nimble_port_freertos_deinit();
}

void ble_ctrl_init(void)
{
    snprintf(s_dev_name, sizeof(s_dev_name), "Nixis-%s", MQTT_DEVICE_ID);

    // NVS is already initialised by main() before this runs; the NimBLE port
    // needs it for the controller's calibration/keys.
    if (nimble_port_init() != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed — BLE unlock disabled");
        return;
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();
    if (ble_gatts_count_cfg(s_gatt_svcs) != 0 ||
        ble_gatts_add_svcs(s_gatt_svcs) != 0) {
        ESP_LOGE(TAG, "GATT registration failed — BLE unlock disabled");
        return;
    }
    ble_svc_gap_device_name_set(s_dev_name);

    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "ble_ctrl started");
}
