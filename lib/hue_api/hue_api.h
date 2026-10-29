/*
 * hue_api – Hue Bridge requests and responses as data (pure C, no network).
 *
 * Requirements: LIGHT-01, LIGHT-02, NFR-09, NFR-10 (see the requirements doc).
 * Tests:        test/test_hue_api/
 *
 * This module only builds request texts and interprets response texts. The
 * HTTPS transport (esp_http_client, certificate, keep-alive) lives in the
 * hue_task and adds the header "hue-application-key: <key>" to every CLIP v2
 * request.
 *
 * Requests
 * --------
 * Scene recall (LIGHT-02)
 *   PUT  /clip/v2/resource/scene/<scene_id>
 *   body {"recall":{"action":"active","duration":<ms>}}
 *   scene_id must be a UUID: 36 characters, 8-4-4-4-12 hex digits with
 *   hyphens (either case). duration_ms must be 0 … 60000.
 *
 * Pairing (NFR-10), sent while the bridge's link button is pressed
 *   POST /api
 *   body {"devicetype":"bathroom_ambience#<instance>","generateclientkey":true}
 *   instance: 1 … 19 characters from [A-Za-z0-9_-].
 *
 * Bodies are compact JSON exactly as shown (no spaces, keys in that order),
 * NUL-terminated.
 *
 * Responses
 * ---------
 * CLIP v2 answers {"errors":[...],"data":[...]}. Mapping used here:
 *   transport failure (http_status <= 0)             HUE_ERR_TRANSPORT
 *   2xx, "errors" empty, "data" present               HUE_OK
 *   2xx with a non-empty "errors" array               HUE_ERR_REJECTED
 *   2xx with a body that is not that shape            HUE_ERR_BAD_RESPONSE
 *   401, 403                                          HUE_ERR_AUTH
 *   404                                               HUE_ERR_NOT_FOUND
 *   429                                               HUE_ERR_RATE_LIMITED
 *   5xx                                               HUE_ERR_BRIDGE_BUSY
 *   any other status                                  HUE_ERR_REJECTED
 *
 * Pairing answers a one-element JSON array, either
 *   [{"success":{"username":"<key>","clientkey":"<hex>"}}]   → HUE_OK, key copied
 *   [{"error":{"type":101,"address":"","description":"link button not pressed"}}]
 *                                                            → HUE_ERR_LINK_BUTTON
 * Any other error object → HUE_ERR_REJECTED; anything unparsable, or a key
 * that does not fit key_out (including its NUL), → HUE_ERR_BAD_RESPONSE.
 * Transport failures and non-2xx statuses map as for hue_parse_response().
 *
 * Response bodies are NOT NUL-terminated: always use body_len. Whitespace
 * between JSON tokens may appear anywhere. A body that is not complete,
 * well-formed JSON (e.g. truncated) counts as unparsable.
 *
 * Implementation hint: cJSON (MIT, two files) has cJSON_ParseWithLength(),
 * which takes a length. Vendor it into lib/cjson so native and target builds
 * use the same copy, and don't also pull in ESP-IDF's "json" component.
 *
 * Retries (NFR-09)
 * ----------------
 * hue_result_is_retryable(): true for TRANSPORT, RATE_LIMITED, BRIDGE_BUSY and
 * LINK_BUTTON; false otherwise (retrying cannot fix them).
 * hue_retry_delay_ms(attempt): attempt 1 → 500 ms, doubling each attempt,
 * capped at 30000 ms. attempt 0 → 0.
 */
#ifndef HUE_API_H
#define HUE_API_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HUE_METHOD_MAX 8
#define HUE_PATH_MAX   64
#define HUE_BODY_MAX   96

typedef struct {
    char method[HUE_METHOD_MAX];
    char path[HUE_PATH_MAX];
    char body[HUE_BODY_MAX];
} hue_request_t;

typedef enum {
    HUE_OK = 0,
    HUE_ERR_TRANSPORT,
    HUE_ERR_AUTH,
    HUE_ERR_NOT_FOUND,
    HUE_ERR_RATE_LIMITED,
    HUE_ERR_BRIDGE_BUSY,
    HUE_ERR_REJECTED,
    HUE_ERR_BAD_RESPONSE,
    HUE_ERR_LINK_BUTTON,
} hue_result_t;

/* Returns false (and leaves *out unspecified) on invalid input. */
bool hue_build_scene_recall(const char *scene_id, uint32_t duration_ms, hue_request_t *out);
bool hue_build_pairing(const char *instance, hue_request_t *out);

hue_result_t hue_parse_response(int http_status, const char *body, size_t body_len);
hue_result_t hue_parse_pairing(int http_status, const char *body, size_t body_len,
                               char *key_out, size_t key_out_size);

bool hue_result_is_retryable(hue_result_t r);
uint32_t hue_retry_delay_ms(uint32_t attempt);

#ifdef __cplusplus
}
#endif

#endif /* HUE_API_H */
