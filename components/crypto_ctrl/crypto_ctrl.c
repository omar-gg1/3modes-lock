#include "crypto_ctrl.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "mbedtls/gcm.h"

static const char *TAG = "crypto_ctrl";

// AES-256 → 32-byte key.
#define AES_KEY_BITS  256
#define AES_KEY_BYTES (AES_KEY_BITS / 8)

// NVS location for the key. Using a dedicated namespace keeps it separate from
// app NVS. When NVS encryption is enabled (sdkconfig), this namespace is stored
// encrypted on flash; until then the key is plaintext in NVS — that gap is
// closed by the eFuse/flash-encryption hardening stage (see connections.md §7).
#define KEY_NVS_NAMESPACE "slcrypto"
#define KEY_NVS_BLOB      "aeskey"

// Container magic, "SLE1" = Smart Lock Encrypted v1.
static const uint8_t SLE_MAGIC[CRYPTO_CTRL_MAGIC_LEN] = {'S', 'L', 'E', '1'};

static uint8_t s_key[AES_KEY_BYTES];
static bool s_inited = false;

// Load the key from NVS, or create+persist one on first boot.
static esp_err_t load_or_create_key(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(KEY_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t len = sizeof(s_key);
    err = nvs_get_blob(h, KEY_NVS_BLOB, s_key, &len);

    if (err == ESP_OK && len == AES_KEY_BYTES) {
        ESP_LOGI(TAG, "AES-256 key loaded from NVS");
        nvs_close(h);
        return ESP_OK;
    }

    if (err != ESP_ERR_NVS_NOT_FOUND && err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_blob failed: %s", esp_err_to_name(err));
        nvs_close(h);
        return err;
    }

    // First boot (or corrupt/short key): generate a fresh key from the HW RNG.
    // esp_fill_random() draws from the ESP32-S3 hardware RNG, which is seeded by
    // RF/ADC noise — suitable for cryptographic key material.
    ESP_LOGW(TAG, "no valid key in NVS — generating a new AES-256 key");
    esp_fill_random(s_key, sizeof(s_key));

    err = nvs_set_blob(h, KEY_NVS_BLOB, s_key, sizeof(s_key));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to persist key: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "new AES-256 key generated and stored");
    return ESP_OK;
}

esp_err_t crypto_ctrl_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    esp_err_t err = load_or_create_key();
    if (err != ESP_OK) {
        return err;
    }
    s_inited = true;
    return ESP_OK;
}

esp_err_t crypto_ctrl_encrypt(const uint8_t *plaintext, size_t plaintext_len,
                              uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out == NULL || out_len == NULL || (plaintext == NULL && plaintext_len != 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_cap < plaintext_len + CRYPTO_CTRL_OVERHEAD) {
        return ESP_ERR_INVALID_SIZE;
    }

    // Lay out the container header: magic | nonce | tag | ciphertext.
    uint8_t *p_magic = out;
    uint8_t *p_nonce = p_magic + CRYPTO_CTRL_MAGIC_LEN;
    uint8_t *p_tag   = p_nonce + CRYPTO_CTRL_NONCE_LEN;
    uint8_t *p_ct    = p_tag   + CRYPTO_CTRL_TAG_LEN;

    memcpy(p_magic, SLE_MAGIC, CRYPTO_CTRL_MAGIC_LEN);
    // Fresh random nonce per encryption — never reuse a nonce with the same key.
    esp_fill_random(p_nonce, CRYPTO_CTRL_NONCE_LEN);

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);

    esp_err_t result = ESP_OK;
    int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, s_key, AES_KEY_BITS);
    if (rc != 0) {
        ESP_LOGE(TAG, "gcm_setkey failed: -0x%04x", -rc);
        result = ESP_FAIL;
        goto done;
    }

    // We authenticate the magic as additional data (AAD) so it can't be swapped
    // for a different version header without failing the tag check.
    rc = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, plaintext_len,
                                   p_nonce, CRYPTO_CTRL_NONCE_LEN,
                                   p_magic, CRYPTO_CTRL_MAGIC_LEN,
                                   plaintext, p_ct,
                                   CRYPTO_CTRL_TAG_LEN, p_tag);
    if (rc != 0) {
        ESP_LOGE(TAG, "gcm_crypt_and_tag failed: -0x%04x", -rc);
        result = ESP_FAIL;
        goto done;
    }

    *out_len = plaintext_len + CRYPTO_CTRL_OVERHEAD;

done:
    mbedtls_gcm_free(&gcm);
    return result;
}

esp_err_t crypto_ctrl_decrypt(const uint8_t *blob, size_t blob_len,
                              uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (blob == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (blob_len < CRYPTO_CTRL_OVERHEAD) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t *p_magic = blob;
    const uint8_t *p_nonce = p_magic + CRYPTO_CTRL_MAGIC_LEN;
    const uint8_t *p_tag   = p_nonce + CRYPTO_CTRL_NONCE_LEN;
    const uint8_t *p_ct    = p_tag   + CRYPTO_CTRL_TAG_LEN;
    size_t ct_len = blob_len - CRYPTO_CTRL_OVERHEAD;

    if (memcmp(p_magic, SLE_MAGIC, CRYPTO_CTRL_MAGIC_LEN) != 0) {
        ESP_LOGE(TAG, "bad container magic");
        return ESP_ERR_INVALID_ARG;
    }
    if (out == NULL && ct_len != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_cap < ct_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);

    esp_err_t result = ESP_OK;
    int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, s_key, AES_KEY_BITS);
    if (rc != 0) {
        ESP_LOGE(TAG, "gcm_setkey failed: -0x%04x", -rc);
        result = ESP_FAIL;
        goto done;
    }

    // auth_decrypt verifies the tag *before* trusting the plaintext. A mismatch
    // (tampering, wrong key, corruption) returns MBEDTLS_ERR_GCM_AUTH_FAILED.
    rc = mbedtls_gcm_auth_decrypt(&gcm, ct_len,
                                  p_nonce, CRYPTO_CTRL_NONCE_LEN,
                                  p_magic, CRYPTO_CTRL_MAGIC_LEN,
                                  p_tag, CRYPTO_CTRL_TAG_LEN,
                                  p_ct, out);
    if (rc == MBEDTLS_ERR_GCM_AUTH_FAILED) {
        ESP_LOGE(TAG, "authentication FAILED — blob tampered or wrong key");
        result = ESP_ERR_INVALID_CRC;
        goto done;
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "gcm_auth_decrypt failed: -0x%04x", -rc);
        result = ESP_FAIL;
        goto done;
    }

    *out_len = ct_len;

done:
    mbedtls_gcm_free(&gcm);
    return result;
}

// Read an entire file into a freshly malloc'd buffer. Caller frees *buf.
static esp_err_t read_whole_file(const char *path, uint8_t **buf, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return ESP_FAIL;
    }
    rewind(f);

    uint8_t *data = (uint8_t *) malloc((size_t) sz);
    if (data == NULL && sz > 0) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t got = (sz > 0) ? fread(data, 1, (size_t) sz, f) : 0;
    fclose(f);

    if (got != (size_t) sz) {
        free(data);
        return ESP_FAIL;
    }
    *buf = data;
    *len = (size_t) sz;
    return ESP_OK;
}

// Write a buffer to a file, all-or-nothing as far as we can manage.
static esp_err_t write_whole_file(const char *path, const uint8_t *buf, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "cannot open %s for write", path);
        return ESP_FAIL;
    }
    size_t put = (len > 0) ? fwrite(buf, 1, len, f) : 0;
    fclose(f);
    if (put != len) {
        ESP_LOGE(TAG, "short write to %s (%u/%u)", path, (unsigned) put, (unsigned) len);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t crypto_ctrl_encrypt_file(const char *plain_path, const char *enc_path)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t *plain = NULL;
    size_t plain_len = 0;
    esp_err_t err = read_whole_file(plain_path, &plain, &plain_len);
    if (err != ESP_OK) {
        return err;  // ESP_ERR_NOT_FOUND propagates so caller knows "no DB".
    }

    size_t blob_cap = plain_len + CRYPTO_CTRL_OVERHEAD;
    uint8_t *blob = (uint8_t *) malloc(blob_cap);
    if (blob == NULL) {
        free(plain);
        return ESP_ERR_NO_MEM;
    }

    size_t blob_len = 0;
    err = crypto_ctrl_encrypt(plain, plain_len, blob, blob_cap, &blob_len);
    free(plain);

    if (err == ESP_OK) {
        err = write_whole_file(enc_path, blob, blob_len);
    }
    free(blob);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "encrypted %s -> %s (%u plaintext bytes)",
                 plain_path, enc_path, (unsigned) plain_len);
    }
    return err;
}

esp_err_t crypto_ctrl_decrypt_file(const char *enc_path, const char *plain_path)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t *blob = NULL;
    size_t blob_len = 0;
    esp_err_t err = read_whole_file(enc_path, &blob, &blob_len);
    if (err != ESP_OK) {
        return err;  // ESP_ERR_NOT_FOUND → caller treats as "no encrypted DB yet".
    }

    size_t out_cap = (blob_len > CRYPTO_CTRL_OVERHEAD) ? blob_len - CRYPTO_CTRL_OVERHEAD : 0;
    uint8_t *plain = (uint8_t *) malloc(out_cap > 0 ? out_cap : 1);
    if (plain == NULL) {
        free(blob);
        return ESP_ERR_NO_MEM;
    }

    size_t plain_len = 0;
    err = crypto_ctrl_decrypt(blob, blob_len, plain, out_cap, &plain_len);
    free(blob);

    if (err == ESP_OK) {
        err = write_whole_file(plain_path, plain, plain_len);
    }
    free(plain);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "decrypted %s -> %s (%u plaintext bytes)",
                 enc_path, plain_path, (unsigned) plain_len);
    }
    return err;
}
