#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_camera.h"

#ifdef __cplusplus
extern "C" {
#endif
/**
 * @brief Check whether anyone is currently enrolled in the recognizer.
 *        Used to gate auto-enrollment on first boot.
 *
 * @return true if at least one face is enrolled.
 */
bool face_ctrl_has_enrolled(void);
/**
 * @brief Initialize face detection + recognition pipeline.
 *        Allocates JPEG decoder, HumanFaceDetect, and HumanFaceRecognizer.
 *        Must be called after camera_ctrl_init().
 */
esp_err_t face_ctrl_init(void);

/**
 * @brief Grab one frame from camera, decode, run detection.
 *        Logs bbox + score to serial if face found.
 *        Designed to be called from main loop, throttled by caller.
 *
 * @return true if at least one face was detected this frame.
 */
bool face_ctrl_detect_once(void);

/**
 * @brief Enroll the most recently detected face under the given numeric ID.
 *        Must be called immediately after face_ctrl_detect_once() returned true.
 *        Persists features to flash; survives reboots.
 *
 * @param id Numeric ID to associate with this face (e.g., 0 for primary user).
 * @return ESP_OK on success.
 */
esp_err_t face_ctrl_enroll(int id);

/**
 * @brief Robust multi-sample enrollment. Captures several quality-gated
 *        templates for one person, each from a fresh frame, and stores them all.
 *        Storing multiple feature vectors per user is what makes later
 *        recognition score high and consistent (a match need only be close to
 *        one stored look). Blocks while capturing (~samples * 200ms).
 *
 * @param id             Numeric ID for this person.
 * @param samples_wanted How many templates to capture (e.g. 5).
 * @param timeout_ms     Give up after this long if good frames don't appear.
 * @return ESP_OK if at least one template was stored,
 *         ESP_ERR_NOT_FOUND if no good face was seen in time.
 */
esp_err_t face_ctrl_enroll_multi(int id, int samples_wanted, int timeout_ms);

/**
 * @brief Whether the most recent detection is good enough to recognize/enroll
 *        from (big enough + confident enough). Gate recognition on this to keep
 *        similarity scores high. Call after face_ctrl_detect_once().
 */
bool face_ctrl_last_is_good_quality(void);

/**
 * @brief Why the last face_ctrl_detect_once() returned false.
 *        0=face found, 1=no frame from camera, 2=bad/corrupt JPEG, 3=no face in
 *        a good frame. Lets callers log the real reason during liveness/scanning.
 */
int face_ctrl_last_detect_fail(void);

/**
 * @brief Borrow the JPEG of the most recent frame processed by
 *        face_ctrl_detect_once(). When detect_once() returned true, this is the
 *        EXACT frame the detector/recognizer just scored — what the Mode 3
 *        cloud verifier must judge (a fresh grab seconds later would show a
 *        different scene). The pointer stays valid until the next detect_once().
 *
 * @param out_buf  Receives a pointer to the internal JPEG buffer (do not free).
 * @param out_len  Receives the JPEG length in bytes.
 * @return ESP_OK, or ESP_ERR_NOT_FOUND if no frame has been retained yet.
 */
esp_err_t face_ctrl_get_last_jpeg(const uint8_t **out_buf, size_t *out_len);

/**
 * @brief How many enrollment JPEGs are stored on flash for Mode 3 cloud sync.
 *        These are the encrypted, gate-passed frames saved during the last
 *        face_ctrl_enroll_multi(). The Mode 3 sync layer uses this to know how
 *        many faces to push to the cloud /enroll endpoint.
 *
 * @param user_id Which user's images to count.
 * @return count of /spiffs/enroll_uUUU_SS.jpg.enc for this user (0..8).
 */
int face_ctrl_enroll_image_count(int user_id);

/**
 * @brief List every user that has at least one enrollment image on flash.
 *        The Mode 3 sync layer uses this to push each user's own faces under
 *        their own id (never a flat 'everyone as user 0').
 * @param out_ids Caller buffer receiving the user ids.
 * @param max     Capacity of out_ids.
 * @return number of ids written.
 */
int face_ctrl_enrolled_user_ids(int *out_ids, int max);

/**
 * @brief Decrypt enrollment image @p idx to a caller-provided plaintext path,
 *        so the Mode 3 sync layer can read the JPEG bytes and POST them to the
 *        cloud /enroll. The caller is responsible for removing @p out_path when
 *        done (the plaintext JPEG should not linger on flash).
 *
 * @param user_id   Which user's image.
 * @param sample    0-based sample index, < face_ctrl_enroll_image_count(user_id).
 * @param out_path  Where to write the decrypted JPEG (e.g. "/spiffs/sync.jpg").
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if that image doesn't exist,
 *         or a crypto/IO error.
 */
esp_err_t face_ctrl_enroll_image_decrypt(int user_id, int sample,
                                         const char *out_path);

/**
 * @brief Check whether the most recently detected face matches an enrolled one.
 *        Must be called immediately after face_ctrl_detect_once() returned true.
 *
 * @param out_id          Output: the matched enrollment ID.
 * @param out_similarity  Output: cosine similarity score (0..1, higher = closer match).
 * @return ESP_OK if a match was found,
 *         ESP_ERR_NOT_FOUND if no face or no enrolled match,
 *         other errors on failure.
 */
esp_err_t face_ctrl_recognize(int *out_id, float *out_similarity);

/**
 * @brief Get the 5-point facial keypoints of the most recently detected face.
 *        Must be called after face_ctrl_detect_once() returned true.
 *
 *        Order (10 ints): [Lx,Ly, Rx,Ry, Nx,Ny, MLx,MLy, MRx,MRy]
 *        = left eye, right eye, nose, mouth-left, mouth-right.
 *        Used by the liveness check to measure head pose + micro-motion.
 *
 * @param out_keypoints Caller-provided buffer of at least @p max ints.
 * @param max           Capacity of @p out_keypoints.
 * @param out_count     Receives the number of ints written (typically 10).
 * @return ESP_OK on success,
 *         ESP_ERR_INVALID_STATE if there's no recent detection,
 *         ESP_ERR_NOT_FOUND if the detection carried no keypoints.
 */
esp_err_t face_ctrl_get_keypoints(int *out_keypoints, int max, int *out_count);

/**
 * @brief Append-enroll a face under @p user_id WITHOUT wiping existing users.
 *        Same quality-gated multi-sample capture as face_ctrl_enroll_multi, but
 *        it adds to the DB instead of clearing it first. Use for multi-user.
 * @return ESP_OK if >=1 template stored, ESP_ERR_NOT_FOUND if no good face.
 */
esp_err_t face_ctrl_enroll_append(int user_id, int samples_wanted, int timeout_ms);

/**
 * @brief Enroll a face that came FROM the cloud (pulled JPEG) into the LOCAL
 *        recognizer under @p user_id, so an app-enrolled user also matches on the
 *        device's fast local pass. Decodes + detects + enrolls the image and
 *        keeps a local encrypted copy.
 * @param user_id Owner of this face.
 * @param sample  Per-user sample index (0-based) for the local image filename.
 * @param jpeg    JPEG bytes pulled from the verifier.
 * @param len     JPEG length.
 * @return ESP_OK if enrolled, ESP_ERR_NOT_FOUND if no face in the image, else err.
 */
esp_err_t face_ctrl_import_face(int user_id, int sample,
                                const uint8_t *jpeg, size_t len);

/**
 * @brief Remove every face feature belonging to @p user_id from the recognizer.
 * @param out_deleted Optional: receives how many features were removed (0 if the
 *                    user had none — still ESP_OK; delete is idempotent).
 * @return ESP_OK on success (including the 0-removed no-op).
 */
esp_err_t face_ctrl_delete_user(int user_id, int *out_deleted);

/**
 * @brief Boot self-test of the positional feature->user map bookkeeping.
 *        Runs pure in-memory (no camera/DB): simulates enroll/delete sequences
 *        and asserts the map stays consistent. Logs PASS/FAIL over serial.
 * @return true if all assertions held.
 */
bool face_ctrl_featmap_selftest(void);

/**
 * @brief Cleanup. Not strictly needed but exposed for completeness.
 */
void face_ctrl_deinit(void);

#ifdef __cplusplus
}
#endif