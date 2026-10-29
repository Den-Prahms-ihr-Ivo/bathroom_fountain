/*
 * Tests for lib/hue_api – requirements LIGHT-01, LIGHT-02, NFR-09, NFR-10.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include "hue_api.h"

#define SCENE_ID "3f2a9c1e-5b7d-4e8a-9c21-7d4e5f6a8b90"

void setUp(void) {}
void tearDown(void) {}

static hue_result_t parse(int status, const char *body)
{
    return hue_parse_response(status, body, body ? strlen(body) : 0);
}

/* ------------------------------------------------------------------------ */
/* Scene recall request (LIGHT-02)                                          */
/* ------------------------------------------------------------------------ */

static void test_scene_recall_request(void)
{
    hue_request_t r;
    TEST_ASSERT_TRUE(hue_build_scene_recall(SCENE_ID, 4000, &r));
    TEST_ASSERT_EQUAL_STRING("PUT", r.method);
    TEST_ASSERT_EQUAL_STRING("/clip/v2/resource/scene/" SCENE_ID, r.path);
    TEST_ASSERT_EQUAL_STRING("{\"recall\":{\"action\":\"active\",\"duration\":4000}}", r.body);
}

static void test_scene_recall_duration_limits(void)
{
    hue_request_t r;
    TEST_ASSERT_TRUE(hue_build_scene_recall(SCENE_ID, 0, &r));
    TEST_ASSERT_EQUAL_STRING("{\"recall\":{\"action\":\"active\",\"duration\":0}}", r.body);
    TEST_ASSERT_TRUE(hue_build_scene_recall(SCENE_ID, 60000, &r));
    TEST_ASSERT_EQUAL_STRING("{\"recall\":{\"action\":\"active\",\"duration\":60000}}", r.body);
    TEST_ASSERT_FALSE(hue_build_scene_recall(SCENE_ID, 60001, &r));
}

static void test_scene_id_upper_case_accepted(void)
{
    hue_request_t r;
    TEST_ASSERT_TRUE(hue_build_scene_recall("3F2A9C1E-5B7D-4E8A-9C21-7D4E5F6A8B90", 4000, &r));
    TEST_ASSERT_EQUAL_STRING("/clip/v2/resource/scene/3F2A9C1E-5B7D-4E8A-9C21-7D4E5F6A8B90", r.path);
}

static void test_invalid_scene_ids_rejected(void)
{
    hue_request_t r;
    const char *bad[] = {
        "",
        "3f2a9c1e-5b7d-4e8a-9c21-7d4e5f6a8b9",   /* 35 chars          */
        "3f2a9c1e-5b7d-4e8a-9c21-7d4e5f6a8b900", /* 37 chars          */
        "3f2a9c1e5b7d-4e8a-9c21-7d4e5f6a8b90a",  /* hyphen missing    */
        "3f2a9c1e-5b7d-4e8a-9c21-7d4e5f6a8b9g",  /* non-hex digit     */
        "3f2a9c1-e5b7d-4e8a-9c21-7d4e5f6a8b90",  /* hyphen misplaced  */
        "../../api/config/../scene/a-b-c-d-e00", /* path injection    */
    };
    TEST_ASSERT_TRUE_MESSAGE(hue_build_scene_recall(SCENE_ID, 4000, &r), "valid id as control");
    TEST_ASSERT_FALSE(hue_build_scene_recall(NULL, 4000, &r));
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(hue_build_scene_recall(bad[i], 4000, &r), bad[i]);
    }
}

/* ------------------------------------------------------------------------ */
/* Pairing request (NFR-10)                                                 */
/* ------------------------------------------------------------------------ */

static void test_pairing_request(void)
{
    hue_request_t r;
    TEST_ASSERT_TRUE(hue_build_pairing("louder", &r));
    TEST_ASSERT_EQUAL_STRING("POST", r.method);
    TEST_ASSERT_EQUAL_STRING("/api", r.path);
    TEST_ASSERT_EQUAL_STRING("{\"devicetype\":\"bathroom_ambience#louder\",\"generateclientkey\":true}", r.body);
}

static void test_pairing_instance_validation(void)
{
    hue_request_t r;
    TEST_ASSERT_TRUE_MESSAGE(hue_build_pairing("abcdefghij_-012345X", &r), "19 characters are allowed");
    TEST_ASSERT_FALSE(hue_build_pairing(NULL, &r));
    TEST_ASSERT_FALSE_MESSAGE(hue_build_pairing("", &r), "empty");
    TEST_ASSERT_FALSE_MESSAGE(hue_build_pairing("abcdefghij_-012345XY", &r), "20 characters");
    TEST_ASSERT_FALSE_MESSAGE(hue_build_pairing("bad name", &r), "space");
    TEST_ASSERT_FALSE_MESSAGE(hue_build_pairing("a\"b", &r), "quote would break the JSON");
    TEST_ASSERT_FALSE_MESSAGE(hue_build_pairing("a#b", &r), "#");
}

/* ------------------------------------------------------------------------ */
/* CLIP v2 responses                                                        */
/* ------------------------------------------------------------------------ */

static void test_success_response(void)
{
    TEST_ASSERT_EQUAL_INT(HUE_OK, parse(200,
        "{\"errors\":[],\"data\":[{\"rid\":\"" SCENE_ID "\",\"rtype\":\"scene\"}]}"));
}

static void test_success_response_with_whitespace_and_other_key_order(void)
{
    TEST_ASSERT_EQUAL_INT(HUE_OK, parse(200,
        "{\n  \"data\" : [ {\"rid\": \"" SCENE_ID "\", \"rtype\": \"scene\"} ],\n"
        "  \"errors\" : [ ]\n}\n"));
}

static void test_errors_in_2xx_body_are_rejections(void)
{
    TEST_ASSERT_EQUAL_INT(HUE_ERR_REJECTED, parse(200,
        "{\"errors\":[{\"description\":\"error: 'json-schema validation', keyword: 'enum'\"}],\"data\":[]}"));
}

static void test_unparsable_2xx_bodies(void)
{
    TEST_ASSERT_EQUAL_INT(HUE_ERR_BAD_RESPONSE, parse(200, ""));
    TEST_ASSERT_EQUAL_INT(HUE_ERR_BAD_RESPONSE, parse(200, "<html>bridge</html>"));
    TEST_ASSERT_EQUAL_INT(HUE_ERR_BAD_RESPONSE, parse(200, "{\"errors\":[]}"));
    TEST_ASSERT_EQUAL_INT(HUE_ERR_BAD_RESPONSE, parse(200, "{\"errors\":[],\"data\":["));
}

static void test_status_codes(void)
{
    const char *err = "{\"errors\":[{\"description\":\"x\"}],\"data\":[]}";
    TEST_ASSERT_EQUAL_INT(HUE_ERR_TRANSPORT, hue_parse_response(0, NULL, 0));
    TEST_ASSERT_EQUAL_INT(HUE_ERR_TRANSPORT, hue_parse_response(-1, NULL, 0));
    TEST_ASSERT_EQUAL_INT(HUE_ERR_AUTH, parse(401, err));
    TEST_ASSERT_EQUAL_INT(HUE_ERR_AUTH, parse(403, err));
    TEST_ASSERT_EQUAL_INT(HUE_ERR_NOT_FOUND, parse(404, err));
    TEST_ASSERT_EQUAL_INT(HUE_ERR_RATE_LIMITED, parse(429, err));
    TEST_ASSERT_EQUAL_INT(HUE_ERR_BRIDGE_BUSY, parse(500, err));
    TEST_ASSERT_EQUAL_INT(HUE_ERR_BRIDGE_BUSY, parse(503, ""));
    TEST_ASSERT_EQUAL_INT(HUE_ERR_REJECTED, parse(400, err));
    TEST_ASSERT_EQUAL_INT(HUE_ERR_REJECTED, parse(302, ""));
}

/* The body buffer has exactly body_len bytes and no terminating NUL. */
static void test_body_is_not_nul_terminated(void)
{
    const char *text = "{\"errors\":[],\"data\":[{\"rid\":\"" SCENE_ID "\",\"rtype\":\"scene\"}]}";
    const size_t len = strlen(text);
    char *exact = malloc(len);
    TEST_ASSERT_NOT_NULL(exact);
    memcpy(exact, text, len);
    const hue_result_t r = hue_parse_response(200, exact, len);
    free(exact);
    TEST_ASSERT_EQUAL_INT(HUE_OK, r);
}

/* Only the first body_len bytes count, even if more follow. */
static void test_trailing_bytes_beyond_length_ignored(void)
{
    const char *buf = "{\"errors\":[],\"data\":[{\"rid\":\"1\",\"rtype\":\"scene\"}]}"
                      "{\"errors\":[{\"description\":\"junk\"}]}";
    const size_t len = strlen("{\"errors\":[],\"data\":[{\"rid\":\"1\",\"rtype\":\"scene\"}]}");
    TEST_ASSERT_EQUAL_INT(HUE_OK, hue_parse_response(200, buf, len));

    const char *cut = "{\"errors\":[],\"data\":[{\"rid\":\"1\"}]}";
    TEST_ASSERT_EQUAL_INT_MESSAGE(HUE_ERR_BAD_RESPONSE, hue_parse_response(200, cut, strlen(cut) - 3),
                                  "truncated body");
}

/* ------------------------------------------------------------------------ */
/* Pairing responses (NFR-10)                                               */
/* ------------------------------------------------------------------------ */

static hue_result_t pair(int status, const char *body, char *key, size_t key_size)
{
    return hue_parse_pairing(status, body, strlen(body), key, key_size);
}

static void test_pairing_success_copies_key(void)
{
    char key[64];
    memset(key, 'Z', sizeof(key));
    TEST_ASSERT_EQUAL_INT(HUE_OK, pair(200,
        "[ { \"success\" : { \"username\" : \"8x2Fk-QpLw9mZ0aB3cD\", \"clientkey\" : \"0123456789ABCDEF\" } } ]",
        key, sizeof(key)));
    TEST_ASSERT_EQUAL_STRING("8x2Fk-QpLw9mZ0aB3cD", key);
}

static void test_pairing_link_button_not_pressed(void)
{
    char key[64];
    TEST_ASSERT_EQUAL_INT(HUE_ERR_LINK_BUTTON, pair(200,
        "[{\"error\":{\"type\":101,\"address\":\"\",\"description\":\"link button not pressed\"}}]",
        key, sizeof(key)));
}

static void test_pairing_other_error_rejected(void)
{
    char key[64];
    TEST_ASSERT_EQUAL_INT(HUE_ERR_REJECTED, pair(200,
        "[{\"error\":{\"type\":7,\"address\":\"/devicetype\",\"description\":\"invalid value\"}}]",
        key, sizeof(key)));
}

static void test_pairing_key_must_fit(void)
{
    const char *body = "[{\"success\":{\"username\":\"abcdefgh\",\"clientkey\":\"00\"}}]";
    char key[9];
    TEST_ASSERT_EQUAL_INT(HUE_OK, pair(200, body, key, 9));
    TEST_ASSERT_EQUAL_STRING("abcdefgh", key);
    TEST_ASSERT_EQUAL_INT(HUE_ERR_BAD_RESPONSE, pair(200, body, key, 8));
}

static void test_pairing_garbage_and_transport(void)
{
    char key[64];
    TEST_ASSERT_EQUAL_INT(HUE_ERR_BAD_RESPONSE, pair(200, "[]", key, sizeof(key)));
    TEST_ASSERT_EQUAL_INT(HUE_ERR_BAD_RESPONSE, pair(200, "not json", key, sizeof(key)));
    TEST_ASSERT_EQUAL_INT(HUE_ERR_TRANSPORT, hue_parse_pairing(0, NULL, 0, key, sizeof(key)));
    TEST_ASSERT_EQUAL_INT(HUE_ERR_BRIDGE_BUSY, pair(503, "", key, sizeof(key)));
}

/* ------------------------------------------------------------------------ */
/* Retries (NFR-09)                                                         */
/* ------------------------------------------------------------------------ */

static void test_retryable_results(void)
{
    TEST_ASSERT_TRUE(hue_result_is_retryable(HUE_ERR_TRANSPORT));
    TEST_ASSERT_TRUE(hue_result_is_retryable(HUE_ERR_RATE_LIMITED));
    TEST_ASSERT_TRUE(hue_result_is_retryable(HUE_ERR_BRIDGE_BUSY));
    TEST_ASSERT_TRUE(hue_result_is_retryable(HUE_ERR_LINK_BUTTON));
    TEST_ASSERT_FALSE(hue_result_is_retryable(HUE_OK));
    TEST_ASSERT_FALSE(hue_result_is_retryable(HUE_ERR_AUTH));
    TEST_ASSERT_FALSE(hue_result_is_retryable(HUE_ERR_NOT_FOUND));
    TEST_ASSERT_FALSE(hue_result_is_retryable(HUE_ERR_REJECTED));
    TEST_ASSERT_FALSE(hue_result_is_retryable(HUE_ERR_BAD_RESPONSE));
}

static void test_retry_backoff(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, hue_retry_delay_ms(0));
    TEST_ASSERT_EQUAL_UINT32(500, hue_retry_delay_ms(1));
    TEST_ASSERT_EQUAL_UINT32(1000, hue_retry_delay_ms(2));
    TEST_ASSERT_EQUAL_UINT32(2000, hue_retry_delay_ms(3));
    TEST_ASSERT_EQUAL_UINT32(16000, hue_retry_delay_ms(6));
    /* capped from attempt 7 on; large attempts must not overflow a shift */
    for (uint32_t a = 7; a <= 70; a++) {
        TEST_ASSERT_EQUAL_UINT32(30000, hue_retry_delay_ms(a));
    }
    TEST_ASSERT_EQUAL_UINT32(30000, hue_retry_delay_ms(UINT32_MAX));
}

/* ------------------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_scene_recall_request);
    RUN_TEST(test_scene_recall_duration_limits);
    RUN_TEST(test_scene_id_upper_case_accepted);
    RUN_TEST(test_invalid_scene_ids_rejected);
    RUN_TEST(test_pairing_request);
    RUN_TEST(test_pairing_instance_validation);
    RUN_TEST(test_success_response);
    RUN_TEST(test_success_response_with_whitespace_and_other_key_order);
    RUN_TEST(test_errors_in_2xx_body_are_rejections);
    RUN_TEST(test_unparsable_2xx_bodies);
    RUN_TEST(test_status_codes);
    RUN_TEST(test_body_is_not_nul_terminated);
    RUN_TEST(test_trailing_bytes_beyond_length_ignored);
    RUN_TEST(test_pairing_success_copies_key);
    RUN_TEST(test_pairing_link_button_not_pressed);
    RUN_TEST(test_pairing_other_error_rejected);
    RUN_TEST(test_pairing_key_must_fit);
    RUN_TEST(test_pairing_garbage_and_transport);
    RUN_TEST(test_retryable_results);
    RUN_TEST(test_retry_backoff);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
