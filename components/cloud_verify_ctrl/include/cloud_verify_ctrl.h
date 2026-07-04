#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * cloud_verify_ctrl — Mode 3 second-opinion client.
 *
 * When local recognition is UNSURE (murky similarity band), the lock sends the
 * current camera frame to the cloud ArcFace verifier (POST /verify) and gets a
 * confident verdict back. The heavy cloud model is far more discriminative than
 * the on-device model, so it resolves the ambiguous cases.
 *
 * Reporting/verification is best-effort: if the network or the verifier is
 * unreachable, verify() returns CLOUD_VERIFY_UNREACHABLE and the caller falls
 * back to a local decision (the lock never hangs on the cloud).
 */

typedef enum {
    CLOUD_VERIFY_MATCH = 0,      // cloud confidently matched an enrolled user
    CLOUD_VERIFY_NO_MATCH,       // cloud says this is not an enrolled user
    CLOUD_VERIFY_UNREACHABLE,    // network/verifier error — caller falls back
} cloud_verify_result_t;

/**
 * @brief Capture the current camera frame and ask the cloud verifier to judge
 *        it. Blocks for the round-trip (bounded by an internal HTTP timeout).
 *
 * @param out_user_id   [out] matched user id on MATCH (else -1). May be NULL.
 * @param out_confidence[out] cloud cosine similarity (else 0). May be NULL.
 * @return MATCH / NO_MATCH / UNREACHABLE.
 */
cloud_verify_result_t cloud_verify_current_frame(int *out_user_id,
                                                 float *out_confidence);

#ifdef __cplusplus
}
#endif
