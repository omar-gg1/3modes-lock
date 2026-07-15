#include "cmd_verify.h"

#include <stdio.h>
#include <string.h>

#include "mbedtls/md.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *SELFTEST_TAG = "cmd_verify";

#define NONCE_RING 32
#define NONCE_LEN  17  // 16 hex + NUL

static char s_nonce_ring[NONCE_RING][NONCE_LEN];
static int  s_nonce_head = 0;
static int  s_nonce_count = 0;
// The nonce ring is now touched from two tasks — the MQTT command handler and
// the BLE write callback (both deliver signed commands). Guard it so a BLE and
// an MQTT command can't interleave a lookup/insert and let a replay slip past.
static portMUX_TYPE s_nonce_mux = portMUX_INITIALIZER_UNLOCKED;

void cmd_build_signing_string(char *out, size_t out_sz,
                              const char *device_id, const char *type,
                              const char *nonce, long long iat, long long exp,
                              const char *compact_args) {
    snprintf(out, out_sz, "%s|%s|%s|%lld|%lld|%s",
             device_id, type, nonce, iat, exp, compact_args);
}

static int hex2bytes(const char *hex, unsigned char *out, size_t out_len) {
    if (strlen(hex) != out_len * 2) return -1;
    for (size_t i = 0; i < out_len; i++) {
        unsigned int b;
        if (sscanf(hex + i * 2, "%2x", &b) != 1) return -1;
        out[i] = (unsigned char) b;
    }
    return 0;
}

int cmd_hmac_hex(const char *secret_hex, const char *msg, char out_hex[65]) {
    unsigned char key[32];
    if (hex2bytes(secret_hex, key, sizeof(key)) != 0) return -1;
    unsigned char mac[32];
    const mbedtls_md_info_t *info =
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (info == NULL) return -1;
    if (mbedtls_md_hmac(info, key, sizeof(key),
                        (const unsigned char *) msg, strlen(msg), mac) != 0)
        return -1;
    for (int i = 0; i < 32; i++) snprintf(out_hex + i * 2, 3, "%02x", mac[i]);
    out_hex[64] = '\0';
    return 0;
}

int cmd_sig_matches(const char *secret_hex, const char *signing_string,
                    const char *provided_sig_hex) {
    char computed[65];
    if (cmd_hmac_hex(secret_hex, signing_string, computed) != 0) return 0;
    if (provided_sig_hex == NULL || strlen(provided_sig_hex) != 64) return 0;
    // constant-time compare over the 64 hex chars
    unsigned char diff = 0;
    for (int i = 0; i < 64; i++)
        diff |= (unsigned char) (computed[i] ^ provided_sig_hex[i]);
    return diff == 0 ? 1 : 0;
}

bool cmd_nonce_seen_or_record(const char *nonce) {
    portENTER_CRITICAL(&s_nonce_mux);
    for (int i = 0; i < s_nonce_count; i++) {
        int idx = (s_nonce_head - 1 - i + NONCE_RING) % NONCE_RING;
        if (strncmp(s_nonce_ring[idx], nonce, NONCE_LEN - 1) == 0) {
            portEXIT_CRITICAL(&s_nonce_mux);
            return true;
        }
    }
    strncpy(s_nonce_ring[s_nonce_head], nonce, NONCE_LEN - 1);
    s_nonce_ring[s_nonce_head][NONCE_LEN - 1] = '\0';
    s_nonce_head = (s_nonce_head + 1) % NONCE_RING;
    if (s_nonce_count < NONCE_RING) s_nonce_count++;
    portEXIT_CRITICAL(&s_nonce_mux);
    return false;
}

// --- Known-answer self-test ---
//
// Vectors below were generated from the backend's app/security.py against the
// all-zero secret ("00"*32). If the on-device HMAC ever diverges from the
// backend, this fails loudly at boot — which would mean every real command gets
// rejected as bad_sig, so catching it here is the whole point.
//
//   sign_command("00"*32,"lock-01","unlock","abcd",100,108,{})
//     -> ecacf26ee54063bd981cbf54ebf9417dcb8629e46b5440b4eee392399a4a572e
//   sign_command("00"*32,"lock-01","ping","abcd",100,108,{})
//     -> 06cf5960479a4b4aa0c937c7bb32166794f4e9de0c1e499e7779549cbad24469

#define KAT_ZERO_SECRET \
    "0000000000000000000000000000000000000000000000000000000000000000"
#define KAT_UNLOCK_SIG \
    "ecacf26ee54063bd981cbf54ebf9417dcb8629e46b5440b4eee392399a4a572e"
#define KAT_PING_SIG \
    "06cf5960479a4b4aa0c937c7bb32166794f4e9de0c1e499e7779549cbad24469"

bool cmd_verify_selftest(void) {
    bool ok = true;

    // 1) signing string is canonical
    char sstr[128];
    cmd_build_signing_string(sstr, sizeof(sstr),
                             "lock-01", "unlock", "abcd", 100, 108, "{}");
    if (strcmp(sstr, "lock-01|unlock|abcd|100|108|{}") != 0) {
        ESP_LOGE(SELFTEST_TAG, "selftest FAIL: signing string '%s'", sstr);
        ok = false;
    }

    // 2) HMAC matches the backend byte-for-byte (unlock + ping vectors)
    char hex[65];
    if (cmd_hmac_hex(KAT_ZERO_SECRET, "lock-01|unlock|abcd|100|108|{}", hex) != 0
        || strcmp(hex, KAT_UNLOCK_SIG) != 0) {
        ESP_LOGE(SELFTEST_TAG, "selftest FAIL: unlock HMAC '%s'", hex);
        ok = false;
    }
    if (cmd_hmac_hex(KAT_ZERO_SECRET, "lock-01|ping|abcd|100|108|{}", hex) != 0
        || strcmp(hex, KAT_PING_SIG) != 0) {
        ESP_LOGE(SELFTEST_TAG, "selftest FAIL: ping HMAC '%s'", hex);
        ok = false;
    }

    // 3) constant-time match accepts the right sig, rejects a tampered one
    if (cmd_sig_matches(KAT_ZERO_SECRET,
                        "lock-01|unlock|abcd|100|108|{}", KAT_UNLOCK_SIG) != 1) {
        ESP_LOGE(SELFTEST_TAG, "selftest FAIL: good sig rejected");
        ok = false;
    }
    char bad[65];
    strcpy(bad, KAT_UNLOCK_SIG);
    bad[0] = (bad[0] == 'a') ? 'b' : 'a';   // flip first char
    if (cmd_sig_matches(KAT_ZERO_SECRET,
                        "lock-01|unlock|abcd|100|108|{}", bad) != 0) {
        ESP_LOGE(SELFTEST_TAG, "selftest FAIL: bad sig accepted");
        ok = false;
    }

    // 4) nonce ring: first sighting false, repeat true (throwaway nonces)
    if (cmd_nonce_seen_or_record("__st_n1") != false
        || cmd_nonce_seen_or_record("__st_n1") != true
        || cmd_nonce_seen_or_record("__st_n2") != false) {
        ESP_LOGE(SELFTEST_TAG, "selftest FAIL: nonce ring");
        ok = false;
    }

    ESP_LOGI(SELFTEST_TAG, "command-verify self-test: %s",
             ok ? "PASS (HMAC matches backend)" : "FAIL");
    return ok;
}
