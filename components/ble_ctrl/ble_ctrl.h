#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ble_ctrl — BLE proximity unlock transport.
 *
 * Advertises a GATT service (Nixis Lock) with a Command (write) and an Ack
 * (notify) characteristic. When a phone that is physically near connects and
 * writes a backend-signed `unlock` command, the write callback runs the SAME
 * HMAC/expiry/nonce verification the MQTT command channel uses (cmd_verify),
 * triggers the lock, reports the event, and notifies the ack back — all with no
 * cloud round-trip. See docs/superpowers/specs/2026-07-15-ble-proximity-unlock.
 *
 * Security: the phone never holds the HMAC secret; it replays a command the
 * backend signed while online. A sniffed pass can be replayed at most once (the
 * shared single-use nonce ring). No BLE bonding — the signature is the auth.
 */

// Start NimBLE, register the GATT service, and begin advertising as
// "Nixis-<device_id>". Safe to call once after mqtt_ctrl_init(). No-op if BLE
// is unavailable at build time.
void ble_ctrl_init(void);

#ifdef __cplusplus
}
#endif
