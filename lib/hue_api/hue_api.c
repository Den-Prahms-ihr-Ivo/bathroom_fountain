/*
 * hue_api – STUB.
 *
 * Compiles so the tests build; every test fails until this is implemented.
 * The contract is in hue_api.h, the tests in test/test_hue_api/.
 */
#include "hue_api.h"

bool hue_build_scene_recall(const char *scene_id, uint32_t duration_ms, hue_request_t *out)
{
    (void)scene_id;
    (void)duration_ms;
    (void)out;
    return false;
}

bool hue_build_pairing(const char *instance, hue_request_t *out)
{
    (void)instance;
    (void)out;
    return false;
}

hue_result_t hue_parse_response(int http_status, const char *body, size_t body_len)
{
    (void)http_status;
    (void)body;
    (void)body_len;
    return HUE_ERR_BAD_RESPONSE;
}

hue_result_t hue_parse_pairing(int http_status, const char *body, size_t body_len,
                               char *key_out, size_t key_out_size)
{
    (void)http_status;
    (void)body;
    (void)body_len;
    (void)key_out;
    (void)key_out_size;
    return HUE_ERR_BAD_RESPONSE;
}

bool hue_result_is_retryable(hue_result_t r)
{
    (void)r;
    return false;
}

uint32_t hue_retry_delay_ms(uint32_t attempt)
{
    (void)attempt;
    return 0;
}
