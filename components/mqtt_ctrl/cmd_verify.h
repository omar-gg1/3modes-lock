#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * cmd_verify — command verification core for the Nixis command channel.
 *
 * Pure logic (no ESP/network deps beyond mbedTLS HMAC) so it is verifiable in
 * isolation. Implements the Plan 0 signing scheme:
 *   signing string = "<device_id>|<type>|<nonce>|<iat>|<exp>|<compact_args>"
 *   sig            = HMAC-SHA256(secret, signing_string) as lowercase hex
 * plus a single-use nonce ring buffer to defeat replay.
 */

// Build the canonical signing string into `out` (Plan 0 §"canonical signing
// string"). `compact_args` must already be compact JSON (sorted keys, no
// spaces); Phase 1 empty args are "{}".
void cmd_build_signing_string(char *out, size_t out_sz,
                              const char *device_id, const char *type,
                              const char *nonce, long long iat, long long exp,
                              const char *compact_args);

// HMAC-SHA256(secret_hex bytes, msg) -> 64 lowercase hex chars + NUL in out_hex.
// Returns 0 on success, non-zero on error (bad secret hex / mbedTLS failure).
int cmd_hmac_hex(const char *secret_hex, const char *msg, char out_hex[65]);

// Constant-time check that provided_sig_hex equals HMAC(secret, signing_string).
// Returns 1 on match, 0 otherwise.
int cmd_sig_matches(const char *secret_hex, const char *signing_string,
                    const char *provided_sig_hex);

// Single-use nonce ring. Returns true if `nonce` was already seen (=> reject as
// replay); otherwise records it and returns false.
bool cmd_nonce_seen_or_record(const char *nonce);

// Runtime self-test with known-answer vectors cross-generated from the backend
// (app/security.py) against the all-zero secret. Proves the on-device HMAC and
// nonce ring match the backend byte-for-byte. Returns true if all checks pass;
// logs each result. Call once at boot. Does NOT touch the production nonce ring
// state used by real commands beyond its own throwaway nonces.
bool cmd_verify_selftest(void);

#ifdef __cplusplus
}
#endif
