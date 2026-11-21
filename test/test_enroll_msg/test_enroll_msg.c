/*
 * Tests for lib/enroll_msg – requirements PAIR-01 ... PAIR-08.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unity.h>

#include "enroll_msg.h"

#define CHIP "a4cf12f0e1d2"
#define SECRET "00112233445566778899aabbccddeeff"

static char out[512];
static enroll_reply_t r;

void setUp(void)
{
    memset(out, 'Z', sizeof(out));
    memset(&r, 0x5A, sizeof(r));
}

void tearDown(void) {}

static bool parse(const char *json)
{
    return enroll_msg_parse(json, strlen(json), &r);
}

static void expect_untouched(void)
{
    enroll_reply_t ref;
    memset(&ref, 0x5A, sizeof(ref));
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&ref, &r, sizeof(r), "reply must be untouched");
}

/* ---- builders ------------------------------------------------------------ */

static void test_hello(void)
{
    const char *want = "{\"chip\":\"" CHIP "\",\"node\":\"bath\",\"fw\":\"2.0.0\",\"secret\":\"" SECRET "\"}";
    TEST_ASSERT_EQUAL_size_t(strlen(want), enroll_msg_hello(CHIP, "bath", "2.0.0", SECRET, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING(want, out);
    TEST_ASSERT_TRUE(enroll_msg_hello(CHIP, "bedroom_speaker", "2.1.0-rc1 (dirty)", SECRET, out, sizeof(out)) > 0);
}

static void test_hello_refusals(void)
{
    TEST_ASSERT_EQUAL_size_t(0, enroll_msg_hello("A4CF12F0E1D2", "bath", "2.0.0", SECRET, out, sizeof(out)));
    TEST_ASSERT_EQUAL_CHAR('\0', out[0]);
    TEST_ASSERT_EQUAL_size_t(0, enroll_msg_hello("a4cf12f0e1d", "bath", "2.0.0", SECRET, out, sizeof(out)));
    TEST_ASSERT_EQUAL_size_t(0, enroll_msg_hello("a4cf12f0e1d2x", "bath", "2.0.0", SECRET, out, sizeof(out)));
    TEST_ASSERT_EQUAL_size_t(0, enroll_msg_hello(CHIP, "", "2.0.0", SECRET, out, sizeof(out)));
    TEST_ASSERT_EQUAL_size_t(0, enroll_msg_hello(CHIP, "Bath", "2.0.0", SECRET, out, sizeof(out)));
    TEST_ASSERT_EQUAL_size_t(0, enroll_msg_hello(CHIP, "bath-1", "2.0.0", SECRET, out, sizeof(out)));
    TEST_ASSERT_EQUAL_size_t(0, enroll_msg_hello(CHIP, "abcdefghijklmnopqrstuvwxy", "2.0.0", SECRET, out, sizeof(out)));
    TEST_ASSERT_EQUAL_size_t(0, enroll_msg_hello(CHIP, "bath", "", SECRET, out, sizeof(out)));
    TEST_ASSERT_EQUAL_size_t(0, enroll_msg_hello(CHIP, "bath", "2.0\n", SECRET, out, sizeof(out)));
    TEST_ASSERT_EQUAL_size_t(0, enroll_msg_hello(CHIP, "bath", "0123456789012345678901234567890x", SECRET, out, sizeof(out)));
    TEST_ASSERT_EQUAL_size_t(0, enroll_msg_hello(CHIP, "bath", "2.0.0", "00112233", out, sizeof(out)));
    TEST_ASSERT_EQUAL_size_t(0, enroll_msg_hello(CHIP, "bath", "2.0.0", "00112233445566778899aabbccddeefg", out, sizeof(out)));
    TEST_ASSERT_EQUAL_size_t(0, enroll_msg_hello(NULL, "bath", "2.0.0", SECRET, out, sizeof(out)));
    TEST_ASSERT_EQUAL_size_t(0, enroll_msg_hello(CHIP, NULL, "2.0.0", SECRET, out, sizeof(out)));
    TEST_ASSERT_EQUAL_size_t(0, enroll_msg_hello(CHIP, "bath", NULL, SECRET, out, sizeof(out)));
    TEST_ASSERT_EQUAL_size_t(0, enroll_msg_hello(CHIP, "bath", "2.0.0", NULL, out, sizeof(out)));
    TEST_ASSERT_EQUAL_size_t(0, enroll_msg_hello(CHIP, "bath", "2.0.0", SECRET, NULL, 100));
    /* too small, exactly enough */
    const size_t n = enroll_msg_hello(CHIP, "bath", "2.0.0", SECRET, out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(0, enroll_msg_hello(CHIP, "bath", "2.0.0", SECRET, out, n));
    TEST_ASSERT_EQUAL_size_t(n, enroll_msg_hello(CHIP, "bath", "2.0.0", SECRET, out, n + 1));
}

static void test_confirm(void)
{
    const char *want = "{\"chip\":\"" CHIP "\",\"secret\":\"" SECRET "\",\"confirm\":true}";
    TEST_ASSERT_EQUAL_size_t(strlen(want), enroll_msg_confirm(CHIP, SECRET, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING(want, out);
    TEST_ASSERT_EQUAL_size_t(0, enroll_msg_confirm(CHIP, "x", out, sizeof(out)));
    TEST_ASSERT_EQUAL_size_t(0, enroll_msg_confirm("x", SECRET, out, sizeof(out)));
    TEST_ASSERT_EQUAL_size_t(0, enroll_msg_confirm(CHIP, SECRET, out, 10));
}

static void test_hex(void)
{
    const uint8_t b[] = {0x00, 0x11, 0xAB, 0xFF};
    char h[9];
    TEST_ASSERT_TRUE(enroll_msg_hex(b, 4, h, sizeof(h)));
    TEST_ASSERT_EQUAL_STRING("0011abff", h);
    TEST_ASSERT_FALSE(enroll_msg_hex(b, 4, h, 8));
    TEST_ASSERT_FALSE(enroll_msg_hex(NULL, 4, h, sizeof(h)));
    TEST_ASSERT_FALSE(enroll_msg_hex(b, 4, NULL, sizeof(h)));
    char e[1];
    TEST_ASSERT_TRUE(enroll_msg_hex(b, 0, e, 1));
    TEST_ASSERT_EQUAL_STRING("", e);
}

/* ---- parser ------------------------------------------------------------- */

static void test_parse_wait(void)
{
    TEST_ASSERT_TRUE(parse("{\"state\":\"wait\"}"));
    TEST_ASSERT_EQUAL_INT(ENROLL_REPLY_WAIT, r.kind);
    TEST_ASSERT_EQUAL_UINT16(0, r.window_s);
    TEST_ASSERT_EQUAL_STRING("", r.creds.device_id);
    TEST_ASSERT_EQUAL_STRING("", r.reason);
    TEST_ASSERT_TRUE_MESSAGE(parse(" { \"extra\": [1,2], \"state\" : \"wait\" } "), "whitespace, unknown members");
}

static void test_parse_confirm(void)
{
    TEST_ASSERT_TRUE(parse("{\"state\":\"confirm\",\"window_s\":60}"));
    TEST_ASSERT_EQUAL_INT(ENROLL_REPLY_CONFIRM, r.kind);
    TEST_ASSERT_EQUAL_UINT16(60, r.window_s);
    TEST_ASSERT_TRUE(parse("{\"state\":\"confirm\",\"window_s\":10}"));
    TEST_ASSERT_TRUE(parse("{\"state\":\"confirm\",\"window_s\":300}"));
    memset(&r, 0x5A, sizeof(r));
    TEST_ASSERT_FALSE(parse("{\"state\":\"confirm\",\"window_s\":9}"));
    TEST_ASSERT_FALSE(parse("{\"state\":\"confirm\",\"window_s\":301}"));
    TEST_ASSERT_FALSE(parse("{\"state\":\"confirm\",\"window_s\":60.5}"));
    TEST_ASSERT_FALSE(parse("{\"state\":\"confirm\",\"window_s\":\"60\"}"));
    TEST_ASSERT_FALSE(parse("{\"state\":\"confirm\"}"));
    expect_untouched();
}

#define CLAIMED_OK                                                                                   \
    "{\"state\":\"claimed\",\"device_id\":\"bath-3f2a\",\"mqtt_host\":\"ambience.example.org\","      \
    "\"mqtt_port\":8883,\"mqtt_user\":\"bath-3f2a\",\"mqtt_pass\":\"s3cret-pass\"}"

static void test_parse_claimed(void)
{
    TEST_ASSERT_TRUE(parse(CLAIMED_OK));
    TEST_ASSERT_EQUAL_INT(ENROLL_REPLY_CLAIMED, r.kind);
    TEST_ASSERT_EQUAL_STRING("bath-3f2a", r.creds.device_id);
    TEST_ASSERT_EQUAL_STRING("ambience.example.org", r.creds.mqtt_host);
    TEST_ASSERT_EQUAL_UINT16(8883, r.creds.mqtt_port);
    TEST_ASSERT_EQUAL_STRING("bath-3f2a", r.creds.mqtt_user);
    TEST_ASSERT_EQUAL_STRING("s3cret-pass", r.creds.mqtt_pass);
    TEST_ASSERT_EQUAL_UINT16(0, r.window_s);
    TEST_ASSERT_EQUAL_STRING("", r.reason);
}

static void test_parse_claimed_validation(void)
{
    static const char *const bad[] = {
        /* member missing */
        "{\"state\":\"claimed\",\"mqtt_host\":\"h.org\",\"mqtt_port\":8883,\"mqtt_user\":\"u\",\"mqtt_pass\":\"12345678\"}",
        "{\"state\":\"claimed\",\"device_id\":\"d\",\"mqtt_port\":8883,\"mqtt_user\":\"u\",\"mqtt_pass\":\"12345678\"}",
        "{\"state\":\"claimed\",\"device_id\":\"d\",\"mqtt_host\":\"h.org\",\"mqtt_user\":\"u\",\"mqtt_pass\":\"12345678\"}",
        "{\"state\":\"claimed\",\"device_id\":\"d\",\"mqtt_host\":\"h.org\",\"mqtt_port\":8883,\"mqtt_pass\":\"12345678\"}",
        "{\"state\":\"claimed\",\"device_id\":\"d\",\"mqtt_host\":\"h.org\",\"mqtt_port\":8883,\"mqtt_user\":\"u\"}",
        /* wrong JSON type */
        "{\"state\":\"claimed\",\"device_id\":\"d\",\"mqtt_host\":\"h.org\",\"mqtt_port\":\"8883\",\"mqtt_user\":\"u\",\"mqtt_pass\":\"12345678\"}",
        "{\"state\":\"claimed\",\"device_id\":5,\"mqtt_host\":\"h.org\",\"mqtt_port\":8883,\"mqtt_user\":\"u\",\"mqtt_pass\":\"12345678\"}",
        /* values the net group refuses */
        "{\"state\":\"claimed\",\"device_id\":\"Bath\",\"mqtt_host\":\"h.org\",\"mqtt_port\":8883,\"mqtt_user\":\"u\",\"mqtt_pass\":\"12345678\"}",
        "{\"state\":\"claimed\",\"device_id\":\"bath/1\",\"mqtt_host\":\"h.org\",\"mqtt_port\":8883,\"mqtt_user\":\"u\",\"mqtt_pass\":\"12345678\"}",
        "{\"state\":\"claimed\",\"device_id\":\"d\",\"mqtt_host\":\"mqtts://h.org\",\"mqtt_port\":8883,\"mqtt_user\":\"u\",\"mqtt_pass\":\"12345678\"}",
        "{\"state\":\"claimed\",\"device_id\":\"d\",\"mqtt_host\":\"h.org\",\"mqtt_port\":0,\"mqtt_user\":\"u\",\"mqtt_pass\":\"12345678\"}",
        "{\"state\":\"claimed\",\"device_id\":\"d\",\"mqtt_host\":\"h.org\",\"mqtt_port\":65536,\"mqtt_user\":\"u\",\"mqtt_pass\":\"12345678\"}",
        "{\"state\":\"claimed\",\"device_id\":\"d\",\"mqtt_host\":\"h.org\",\"mqtt_port\":88.5,\"mqtt_user\":\"u\",\"mqtt_pass\":\"12345678\"}",
        "{\"state\":\"claimed\",\"device_id\":\"d\",\"mqtt_host\":\"h.org\",\"mqtt_port\":8883,\"mqtt_user\":\"a b\",\"mqtt_pass\":\"12345678\"}",
        "{\"state\":\"claimed\",\"device_id\":\"d\",\"mqtt_host\":\"h.org\",\"mqtt_port\":8883,\"mqtt_user\":\"u\",\"mqtt_pass\":\"1234567\"}",
        "{\"state\":\"claimed\",\"device_id\":\"\",\"mqtt_host\":\"h.org\",\"mqtt_port\":8883,\"mqtt_user\":\"u\",\"mqtt_pass\":\"12345678\"}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(parse(bad[i]), bad[i]);
    }
    expect_untouched();
    /* the limits themselves */
    TEST_ASSERT_TRUE(parse("{\"state\":\"claimed\",\"device_id\":\"d\",\"mqtt_host\":\"h\",\"mqtt_port\":1,"
                           "\"mqtt_user\":\"u\",\"mqtt_pass\":\"12345678\"}"));
    TEST_ASSERT_TRUE(parse("{\"state\":\"claimed\",\"device_id\":\"d\",\"mqtt_host\":\"h\",\"mqtt_port\":65535,"
                           "\"mqtt_user\":\"u\",\"mqtt_pass\":\"12345678\"}"));
    TEST_ASSERT_EQUAL_UINT16(65535, r.creds.mqtt_port);
}

static void test_parse_rejected(void)
{
    TEST_ASSERT_TRUE(parse("{\"state\":\"rejected\",\"reason\":\"conflict\"}"));
    TEST_ASSERT_EQUAL_INT(ENROLL_REPLY_REJECTED, r.kind);
    TEST_ASSERT_EQUAL_STRING("conflict", r.reason);
    TEST_ASSERT_TRUE(parse("{\"state\":\"rejected\"}"));
    TEST_ASSERT_EQUAL_STRING("", r.reason);
    TEST_ASSERT_TRUE(parse("{\"state\":\"rejected\",\"reason\":7}"));
    TEST_ASSERT_EQUAL_STRING("", r.reason);
    TEST_ASSERT_TRUE(parse("{\"state\":\"rejected\",\"reason\":\"0123456789012345678901234567890\"}")); /* 31 */
    TEST_ASSERT_EQUAL_size_t(31, strlen(r.reason));
    TEST_ASSERT_TRUE(parse("{\"state\":\"rejected\",\"reason\":\"0123456789012345678901234567890x\"}")); /* 32 */
    TEST_ASSERT_EQUAL_STRING("", r.reason);
}

static void test_parse_garbage(void)
{
    static const char *const bad[] = {"", "null", "[]", "\"wait\"", "{", "{\"state\":\"waiting\"}", "{\"state\":1}",
                                      "{\"State\":\"wait\"}", "{}"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(parse(bad[i]), bad[i]);
    }
    TEST_ASSERT_FALSE(enroll_msg_parse(NULL, 4, &r));
    TEST_ASSERT_FALSE(enroll_msg_parse("{\"state\":\"wait\"}", 16, NULL));
    expect_untouched();
}

static void test_parse_uses_len_not_nul(void)
{
    const char buf[] = "{\"state\":\"wait\"}GARBAGE";
    TEST_ASSERT_TRUE(enroll_msg_parse(buf, 16, &r));
    TEST_ASSERT_EQUAL_INT(ENROLL_REPLY_WAIT, r.kind);
    TEST_ASSERT_FALSE_MESSAGE(enroll_msg_parse(buf, 10, &r), "cut short");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_hello);
    RUN_TEST(test_hello_refusals);
    RUN_TEST(test_confirm);
    RUN_TEST(test_hex);
    RUN_TEST(test_parse_wait);
    RUN_TEST(test_parse_confirm);
    RUN_TEST(test_parse_claimed);
    RUN_TEST(test_parse_claimed_validation);
    RUN_TEST(test_parse_rejected);
    RUN_TEST(test_parse_garbage);
    RUN_TEST(test_parse_uses_len_not_nul);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
