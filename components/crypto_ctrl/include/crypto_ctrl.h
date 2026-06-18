#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * crypto_ctrl — on-device AES-256-GCM encryption for data at rest.
 *
 * This is the software half of the project's encryption strategy. It exists
 * to replace the ATECC608A crypto co-processor (which we could not source).
 * The face-embedding database is encrypted with AES-256-GCM so that a flash
 * dump yields ciphertext, and any tampering with the stored blob is *detected*
 * (GCM is authenticated encryption — a bad tag means decrypt fails loudly
 * rather than returning garbage embeddings).
 *
 * Key storage (this stage): a 256-bit key generated from the ESP32-S3 hardware
 * RNG on first boot and persisted in an ENCRYPTED NVS namespace. A later
 * hardening stage moves the root of trust into eFuse + flash encryption so the
 * key is hardware-protected; this API does not change when that happens.
 *
 * On-disk container format produced by crypto_ctrl_encrypt():
 *
 *     [ 4 bytes  magic  "SLE1" ]   ("Smart Lock Encrypted v1")
 *     [ 12 bytes nonce         ]   random per encryption (GCM IV)
 *     [ 16 bytes tag           ]   GCM authentication tag
 *     [ N  bytes ciphertext    ]   same length as the plaintext
 *
 * Total overhead is a fixed CRYPTO_CTRL_OVERHEAD bytes.
 */

#define CRYPTO_CTRL_MAGIC_LEN  4
#define CRYPTO_CTRL_NONCE_LEN  12
#define CRYPTO_CTRL_TAG_LEN    16
#define CRYPTO_CTRL_OVERHEAD   (CRYPTO_CTRL_MAGIC_LEN + CRYPTO_CTRL_NONCE_LEN + CRYPTO_CTRL_TAG_LEN)

/**
 * @brief Initialize the crypto subsystem.
 *
 * Loads the AES-256 key from encrypted NVS, or generates a fresh random key
 * (via the hardware RNG) and persists it on first boot. Must be called once at
 * startup, after nvs_flash_init().
 *
 * @return ESP_OK on success; an esp_err_t from the NVS layer otherwise.
 */
esp_err_t crypto_ctrl_init(void);

/**
 * @brief Encrypt a plaintext buffer into the SLE1 container format.
 *
 * @param plaintext      Input bytes to encrypt (may be NULL only if len == 0).
 * @param plaintext_len  Number of input bytes.
 * @param out            Output buffer; must hold at least
 *                       plaintext_len + CRYPTO_CTRL_OVERHEAD bytes.
 * @param out_cap        Capacity of @p out in bytes.
 * @param out_len        Receives the number of bytes written to @p out.
 * @return ESP_OK on success,
 *         ESP_ERR_INVALID_STATE if not initialized,
 *         ESP_ERR_INVALID_SIZE if out_cap is too small,
 *         ESP_FAIL on a crypto error.
 */
esp_err_t crypto_ctrl_encrypt(const uint8_t *plaintext, size_t plaintext_len,
                              uint8_t *out, size_t out_cap, size_t *out_len);

/**
 * @brief Decrypt and authenticate an SLE1 container.
 *
 * Verifies the magic and the GCM tag before returning plaintext. A tampered or
 * truncated blob fails with ESP_ERR_INVALID_CRC and writes nothing useful.
 *
 * @param blob       Input container produced by crypto_ctrl_encrypt().
 * @param blob_len   Length of @p blob.
 * @param out        Output buffer; must hold at least
 *                   blob_len - CRYPTO_CTRL_OVERHEAD bytes.
 * @param out_cap    Capacity of @p out in bytes.
 * @param out_len    Receives the number of plaintext bytes written.
 * @return ESP_OK on success,
 *         ESP_ERR_INVALID_STATE if not initialized,
 *         ESP_ERR_INVALID_ARG if the blob is too short or magic is wrong,
 *         ESP_ERR_INVALID_SIZE if out_cap is too small,
 *         ESP_ERR_INVALID_CRC if authentication fails (tampered/wrong key).
 */
esp_err_t crypto_ctrl_decrypt(const uint8_t *blob, size_t blob_len,
                              uint8_t *out, size_t out_cap, size_t *out_len);

/**
 * @brief Encrypt the file at @p plain_path and write the SLE1 container to
 *        @p enc_path. Reads the whole file into RAM, so intended for the small
 *        face database (tens of KB), not arbitrary large files.
 *
 * @return ESP_OK on success; ESP_ERR_NOT_FOUND if @p plain_path is missing;
 *         other esp_err_t on I/O or crypto failure.
 */
esp_err_t crypto_ctrl_encrypt_file(const char *plain_path, const char *enc_path);

/**
 * @brief Decrypt the SLE1 container at @p enc_path and write the recovered
 *        plaintext to @p plain_path. If @p enc_path does not exist, returns
 *        ESP_ERR_NOT_FOUND so the caller can treat it as "no DB yet".
 *
 * @return ESP_OK on success; ESP_ERR_NOT_FOUND if @p enc_path is missing;
 *         ESP_ERR_INVALID_CRC if authentication fails; other esp_err_t on I/O.
 */
esp_err_t crypto_ctrl_decrypt_file(const char *enc_path, const char *plain_path);

#ifdef __cplusplus
}
#endif
