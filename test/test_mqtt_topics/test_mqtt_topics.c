/*
 * Tests for lib/mqtt_topics – requirements SRV-03, SRV-04.
 */
#include <stdbool.h>
#include <string.h>
#include <unity.h>

#include "mqtt_topics.h"

static char out[MQTT_TOPIC_MAX];

void setUp(void)
{
    memset(out, 'Z', sizeof(out));
}

void tearDown(void) {}

static void expect_build(mqtt_topic_t kind, const char *suffix, const char *expected)
{
    memset(out, 'Z', sizeof(out));
    TEST_ASSERT_TRUE_MESSAGE(mqtt_topic_build(out, sizeof(out), "bath-1", kind, suffix), expected);
    TEST_ASSERT_EQUAL_STRING(expected, out);
}

static void expect_no_build(const char *id, mqtt_topic_t kind, const char *suffix, const char *why)
{
    memset(out, 'Z', sizeof(out));
    TEST_ASSERT_FALSE_MESSAGE(mqtt_topic_build(out, sizeof(out), id, kind, suffix), why);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", out, why);
}

static void expect_parse(const char *topic, mqtt_topic_t kind, const char *suffix)
{
    mqtt_topic_t k = (mqtt_topic_t)99;
    const char *s = "untouched";
    size_t n = 99;
    TEST_ASSERT_TRUE_MESSAGE(mqtt_topic_parse(topic, strlen(topic), "bath-1", &k, &s, &n), topic);
    TEST_ASSERT_EQUAL_INT(kind, k);
    if (suffix == NULL) {
        TEST_ASSERT_NULL(s);
        TEST_ASSERT_EQUAL_size_t(0, n);
    } else {
        TEST_ASSERT_EQUAL_size_t(strlen(suffix), n);
        TEST_ASSERT_EQUAL_MEMORY(suffix, s, n);
        TEST_ASSERT_TRUE_MESSAGE(s >= topic && s + n == topic + strlen(topic), "suffix points into the topic");
    }
}

static void expect_no_parse(const char *topic)
{
    mqtt_topic_t k = (mqtt_topic_t)99;
    const char *s = "untouched";
    size_t n = 99;
    TEST_ASSERT_FALSE_MESSAGE(mqtt_topic_parse(topic, strlen(topic), "bath-1", &k, &s, &n), topic);
    TEST_ASSERT_EQUAL_INT(99, k);
    TEST_ASSERT_EQUAL_STRING("untouched", s);
    TEST_ASSERT_EQUAL_size_t(99, n);
}

static void test_device_id_rule(void)
{
    TEST_ASSERT_TRUE(mqtt_device_id_valid("bath-1"));
    TEST_ASSERT_TRUE(mqtt_device_id_valid("a"));
    TEST_ASSERT_TRUE(mqtt_device_id_valid("abcdefghijklmnopqrstuvwx"));
    TEST_ASSERT_FALSE(mqtt_device_id_valid("abcdefghijklmnopqrstuvwxy"));
    TEST_ASSERT_FALSE(mqtt_device_id_valid(""));
    TEST_ASSERT_FALSE(mqtt_device_id_valid(NULL));
    TEST_ASSERT_FALSE(mqtt_device_id_valid("Bath"));
    TEST_ASSERT_FALSE(mqtt_device_id_valid("bath/1"));
    TEST_ASSERT_FALSE(mqtt_device_id_valid("bath+"));
    TEST_ASSERT_FALSE(mqtt_device_id_valid("bath#"));
    TEST_ASSERT_FALSE(mqtt_device_id_valid("bath 1"));
    TEST_ASSERT_FALSE(mqtt_device_id_valid("bath_1"));
}

static void test_build_every_kind(void)
{
    expect_build(MQTT_TOPIC_STATUS, NULL, "shower/bath-1/status");
    expect_build(MQTT_TOPIC_DESIRED, NULL, "shower/bath-1/settings/desired");
    expect_build(MQTT_TOPIC_REPORTED, NULL, "shower/bath-1/settings/reported");
    expect_build(MQTT_TOPIC_EVENT, NULL, "shower/bath-1/event");
    expect_build(MQTT_TOPIC_LOG, NULL, "shower/bath-1/log");
    expect_build(MQTT_TOPIC_HUE_SCENES, NULL, "shower/bath-1/hue/scenes");
    expect_build(MQTT_TOPIC_CMD, "cold/start", "shower/bath-1/cmd/cold/start");
    expect_build(MQTT_TOPIC_CMD, "ota", "shower/bath-1/cmd/ota");
    expect_build(MQTT_TOPIC_CMD, "#", "shower/bath-1/cmd/#");
    expect_build(MQTT_TOPIC_NODE, "target-2", "shower/bath-1/node/target-2");
}

static void test_build_rejects_bad_arguments(void)
{
    expect_no_build("Bath", MQTT_TOPIC_STATUS, NULL, "bad device id");
    expect_no_build("", MQTT_TOPIC_STATUS, NULL, "empty device id");
    expect_no_build(NULL, MQTT_TOPIC_STATUS, NULL, "no device id");
    expect_no_build("bath-1", (mqtt_topic_t)8, NULL, "unknown kind");
    expect_no_build("bath-1", (mqtt_topic_t)-1, NULL, "unknown kind");
    expect_no_build("bath-1", MQTT_TOPIC_STATUS, "x", "status takes no suffix");
    expect_no_build("bath-1", MQTT_TOPIC_CMD, NULL, "cmd needs a suffix");
    expect_no_build("bath-1", MQTT_TOPIC_NODE, NULL, "node needs a suffix");
    expect_no_build("bath-1", MQTT_TOPIC_CMD, "", "empty suffix");
    expect_no_build("bath-1", MQTT_TOPIC_CMD, "/ota", "leading slash");
    expect_no_build("bath-1", MQTT_TOPIC_CMD, "ota/", "trailing slash");
    expect_no_build("bath-1", MQTT_TOPIC_CMD, "cold//start", "double slash");
    expect_no_build("bath-1", MQTT_TOPIC_CMD, "Cold", "upper case");
    expect_no_build("bath-1", MQTT_TOPIC_CMD, "cold/+", "wildcard inside");
    expect_no_build("bath-1", MQTT_TOPIC_CMD, "cold/#", "wildcard inside");
    expect_no_build("bath-1", MQTT_TOPIC_NODE, "#", "only cmd may subscribe with #");
    expect_no_build("bath-1", MQTT_TOPIC_CMD, "abcdefghijklmnopqrstuvwxyz0123456", "33 characters");
    expect_build(MQTT_TOPIC_CMD, "abcdefghijklmnopqrstuvwxyz012345", "shower/bath-1/cmd/abcdefghijklmnopqrstuvwxyz012345");

    char small[20]; /* "shower/bath-1/status" needs 21 */
    memset(small, 'Z', sizeof(small));
    TEST_ASSERT_FALSE(mqtt_topic_build(small, sizeof(small), "bath-1", MQTT_TOPIC_STATUS, NULL));
    TEST_ASSERT_EQUAL_STRING("", small);
    char exact[21];
    TEST_ASSERT_TRUE(mqtt_topic_build(exact, sizeof(exact), "bath-1", MQTT_TOPIC_STATUS, NULL));
    TEST_ASSERT_FALSE(mqtt_topic_build(NULL, 80, "bath-1", MQTT_TOPIC_STATUS, NULL));
    TEST_ASSERT_FALSE(mqtt_topic_build(exact, 0, "bath-1", MQTT_TOPIC_STATUS, NULL));
}

static void test_longest_topic_fits(void)
{
    char big[MQTT_TOPIC_MAX];
    TEST_ASSERT_TRUE(mqtt_topic_build(big, sizeof(big), "abcdefghijklmnopqrstuvwx", MQTT_TOPIC_CMD,
                                      "abcdefghijklmnopqrstuvwxyz012345"));
    TEST_ASSERT_EQUAL_size_t(7 + 24 + 5 + 32, strlen(big));
    TEST_ASSERT_TRUE(mqtt_topic_build(big, sizeof(big), "abcdefghijklmnopqrstuvwx", MQTT_TOPIC_REPORTED, NULL));
}

static void test_parse_every_kind(void)
{
    expect_parse("shower/bath-1/status", MQTT_TOPIC_STATUS, NULL);
    expect_parse("shower/bath-1/settings/desired", MQTT_TOPIC_DESIRED, NULL);
    expect_parse("shower/bath-1/settings/reported", MQTT_TOPIC_REPORTED, NULL);
    expect_parse("shower/bath-1/event", MQTT_TOPIC_EVENT, NULL);
    expect_parse("shower/bath-1/log", MQTT_TOPIC_LOG, NULL);
    expect_parse("shower/bath-1/hue/scenes", MQTT_TOPIC_HUE_SCENES, NULL);
    expect_parse("shower/bath-1/cmd/cold/start", MQTT_TOPIC_CMD, "cold/start");
    expect_parse("shower/bath-1/cmd/ota", MQTT_TOPIC_CMD, "ota");
    expect_parse("shower/bath-1/node/gun", MQTT_TOPIC_NODE, "gun");
}

static void test_parse_uses_the_length_not_a_terminator(void)
{
    const char *raw = "shower/bath-1/cmd/cold/next{\"payload\":1}";
    mqtt_topic_t k;
    const char *s;
    size_t n;
    TEST_ASSERT_TRUE(mqtt_topic_parse(raw, 27, "bath-1", &k, &s, &n));
    TEST_ASSERT_EQUAL_INT(MQTT_TOPIC_CMD, k);
    TEST_ASSERT_EQUAL_size_t(9, n);
    TEST_ASSERT_EQUAL_MEMORY("cold/next", s, 9);
    TEST_ASSERT_FALSE(mqtt_topic_parse(raw, strlen(raw), "bath-1", &k, &s, &n));
    TEST_ASSERT_TRUE(mqtt_topic_parse("shower/bath-1/statusXYZ", 20, "bath-1", &k, &s, &n));
    TEST_ASSERT_EQUAL_INT(MQTT_TOPIC_STATUS, k);
}

static void test_parse_rejects_foreign_and_malformed_topics(void)
{
    expect_parse("shower/bath-1/status", MQTT_TOPIC_STATUS, NULL); /* the stub must not pass */
    expect_no_parse("shower/bath-2/status");
    expect_no_parse("shower/bath-10/status");
    expect_no_parse("shower/bath-/status");
    expect_no_parse("shower/bath-1");
    expect_no_parse("shower/bath-1/");
    expect_no_parse("shower/bath-1/statu");
    expect_no_parse("shower/bath-1/status/");
    expect_no_parse("shower/bath-1/status/x");
    expect_no_parse("showers/bath-1/status");
    expect_no_parse("Shower/bath-1/status");
    expect_no_parse("/shower/bath-1/status");
    expect_no_parse("shower/bath-1/settings");
    expect_no_parse("shower/bath-1/settings/other");
    expect_no_parse("shower/bath-1/cmd");
    expect_no_parse("shower/bath-1/cmd/");
    expect_no_parse("shower/bath-1/cmd/#");
    expect_no_parse("shower/bath-1/cmd/Cold");
    expect_no_parse("shower/bath-1/cmd/cold//start");
    expect_no_parse("shower/bath-1/cmd/cold/");
    expect_no_parse("shower/bath-1/node/");
    expect_no_parse("shower/bath-1/cmd/abcdefghijklmnopqrstuvwxyz0123456");
    expect_no_parse("");

    mqtt_topic_t k;
    const char *s;
    size_t n;
    const char *ok = "shower/bath-1/status";
    TEST_ASSERT_FALSE(mqtt_topic_parse(NULL, 20, "bath-1", &k, &s, &n));
    TEST_ASSERT_FALSE(mqtt_topic_parse(ok, 20, NULL, &k, &s, &n));
    TEST_ASSERT_FALSE(mqtt_topic_parse(ok, 20, "", &k, &s, &n));
    TEST_ASSERT_FALSE(mqtt_topic_parse(ok, 20, "bath-1", NULL, &s, &n));
    TEST_ASSERT_FALSE(mqtt_topic_parse(ok, 20, "bath-1", &k, NULL, &n));
    TEST_ASSERT_FALSE(mqtt_topic_parse(ok, 20, "bath-1", &k, &s, NULL));
}

static void test_build_and_parse_agree(void)
{
    const char *suffixes[] = {"mode", "cold/abort", "scene/test", "music/sync", "log/verbose", "game/demo", "a_b-c/9"};
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        TEST_ASSERT_TRUE(mqtt_topic_build(out, sizeof(out), "bath-1", MQTT_TOPIC_CMD, suffixes[i]));
        expect_parse(out, MQTT_TOPIC_CMD, suffixes[i]);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_device_id_rule);
    RUN_TEST(test_build_every_kind);
    RUN_TEST(test_build_rejects_bad_arguments);
    RUN_TEST(test_longest_topic_fits);
    RUN_TEST(test_parse_every_kind);
    RUN_TEST(test_parse_uses_the_length_not_a_terminator);
    RUN_TEST(test_parse_rejects_foreign_and_malformed_topics);
    RUN_TEST(test_build_and_parse_agree);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
