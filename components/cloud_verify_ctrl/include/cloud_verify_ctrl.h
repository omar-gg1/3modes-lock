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

/**
 * @brief Mode 3 one-time provisioning: push every locally-enrolled face to the
 *        cloud so /verify has a matching reference from the SAME camera.
 *
 *        Enrollment is always LOCAL-only (privacy: local modes never touch the
 *        cloud). When the device runs Mode 3 and has WiFi, it calls this once to
 *        upload the gate-passed enrollment JPEGs (saved by face_ctrl) to the
 *        verifier's /enroll. The cloud re-embeds them with its own ArcFace, so
 *        both sides hold one face from one camera — the whole point of Mode 3.
 *
 *        Idempotent enough for repeated boots: re-enrolling on the server just
 *        replaces the reference. Best-effort per image; a failure on one image
 *        does not abort the rest.
 *
 * @param user_id  The user id to enroll these frames under (matches the local
 *                 enrollment id, e.g. 0 for the primary user).
 * @param name     Human-readable name required by the /enroll endpoint.
 * @return number of enrollment images successfully accepted by the cloud
 *         (0 if none / no enrollment images / cloud unreachable).
 */
int cloud_verify_sync_enrollments(int user_id, const char *name);

/**
 * @brief Read the cloud gallery's revision token (GET /faces/revision). The ESP
 *        stores the last value it synced and only reconciles when it differs —
 *        replacing the old blind "re-push everything on every boot" sync.
 * @param out_count  [out] total enrolled encodings. May be NULL.
 * @param out_max_id [out] highest encoding id (monotonic). May be NULL.
 * @return true if the revision was read, false if the verifier was unreachable.
 */
bool cloud_verify_faces_revision(int *out_count, int *out_max_id);

/**
 * @brief Pull faces for users this device does NOT already hold locally and
 *        import them into the local recognizer (GET /faces, then per-image
 *        GET /faces/{id}/image). This makes an app-enrolled user match on the
 *        fast local pass, not only via a cloud round-trip.
 * @param known_users Array of user ids already enrolled locally (skip these).
 * @param known_count Length of known_users.
 * @return number of cloud faces imported.
 */
int cloud_verify_pull_new_faces(const int *known_users, int known_count);

/**
 * @brief Remove one user's references from the cloud gallery
 *        (DELETE /encodings?user_id=N). Keeps the cloud consistent with a local
 *        delete_user so the deleted person stops matching on both sides.
 * @return true if the cloud accepted the delete.
 */
bool cloud_verify_delete_user(int user_id);

#ifdef __cplusplus
}
#endif
