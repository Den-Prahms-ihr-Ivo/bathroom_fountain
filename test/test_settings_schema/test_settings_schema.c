/*
 * Tests for lib/settings_schema – requirement SRV-14.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unity.h>

#include "cJSON.h"
#include "node_config.h"
#include "settings_schema.h"

typedef struct {
    uint16_t fade_ms;
    uint16_t warm_s;
    uint8_t snooze_min;
    uint8_t sunrise_level;
    uint8_t count;
    bool on;
    uint8_t preset;
    char name[17];
    char pass[33];
    char scene[37];
} demo_t;

static const kv_key_t demo_keys[] = {
    {"fade_ms", KV_UINT, KV_FIELD(demo_t, fade_ms), 0, 10000, "3000", NULL, NULL},
    {"warm_s", KV_UINT, KV_FIELD(demo_t, warm_s), 0, 600, "120", NULL, NULL},
    {"snooze_min", KV_UINT, KV_FIELD(demo_t, snooze_min), 1, 30, "9", NULL, NULL},
    {"sunrise_level", KV_UINT, KV_FIELD(demo_t, sunrise_level), 1, 100, "80", NULL, NULL},
    {"count", KV_UINT, KV_FIELD(demo_t, count), 0, 5, NULL, NULL, NULL},
    {"on", KV_BOOL, KV_FIELD(demo_t, on), 0, 0, "off", NULL, NULL},
    {"preset", KV_CHOICE, KV_FIELD(demo_t, preset), 0, 0, "normal", "easy|normal|hard", NULL},
    {"name", KV_TEXT, KV_FIELD(demo_t, name), 1, 16, "bath", NULL, NULL},
    {"pass", KV_SECRET, KV_FIELD(demo_t, pass), 8, 32, "hunter2hunter2", NULL, NULL},
    {"scene", KV_UUID, KV_FIELD(demo_t, scene), 0, 0, NULL, NULL, NULL},
};
static const kv_table_t demo = {"demo", demo_keys, sizeof(demo_keys) / sizeof(demo_keys[0]), sizeof(demo_t)};

static char out[SETTINGS_SCHEMA_MAX + 64];

void setUp(void)
{
    memset(out, 'Z', sizeof(out));
}

void tearDown(void) {}

static void test_every_type(void)
{
    TEST_ASSERT_TRUE_MESSAGE(kv_table_valid(&demo), "test table itself valid");
    const char *want =
        "{\"group\":\"demo\",\"keys\":["
        "{\"key\":\"fade_ms\",\"type\":\"uint\",\"min\":0,\"max\":10000,\"unit\":\"ms\",\"default\":\"3000\"},"
        "{\"key\":\"warm_s\",\"type\":\"uint\",\"min\":0,\"max\":600,\"unit\":\"s\",\"default\":\"120\"},"
        "{\"key\":\"snooze_min\",\"type\":\"uint\",\"min\":1,\"max\":30,\"unit\":\"min\",\"default\":\"9\"},"
        "{\"key\":\"sunrise_level\",\"type\":\"uint\",\"min\":1,\"max\":100,\"unit\":\"%\",\"default\":\"80\"},"
        "{\"key\":\"count\",\"type\":\"uint\",\"min\":0,\"max\":5},"
        "{\"key\":\"on\",\"type\":\"bool\",\"default\":\"off\"},"
        "{\"key\":\"preset\",\"type\":\"choice\",\"choices\":[\"easy\",\"normal\",\"hard\"],\"default\":\"normal\"},"
        "{\"key\":\"name\",\"type\":\"text\",\"min\":1,\"max\":16,\"default\":\"bath\"},"
        "{\"key\":\"pass\",\"type\":\"secret\",\"min\":8,\"max\":32},"
        "{\"key\":\"scene\",\"type\":\"uuid\"}]}";
    const size_t n = settings_schema_json(&demo, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING(want, out);
    TEST_ASSERT_EQUAL_size_t(strlen(want), n);
}

static void test_buffer_limits(void)
{
    const size_t n = settings_schema_json(&demo, out, sizeof(out));
    TEST_ASSERT_TRUE(n > 0);
    memset(out, 'Z', sizeof(out));
    TEST_ASSERT_EQUAL_size_t(n, settings_schema_json(&demo, out, n + 1));
    TEST_ASSERT_EQUAL_CHAR('Z', out[n + 1]);
    TEST_ASSERT_EQUAL_size_t(0, settings_schema_json(&demo, out, n));
    TEST_ASSERT_EQUAL_CHAR('\0', out[0]);
    TEST_ASSERT_EQUAL_size_t(0, settings_schema_json(NULL, out, sizeof(out)));
    TEST_ASSERT_EQUAL_size_t(0, settings_schema_json(&demo, NULL, 100));
}

static void test_invalid_table_refused(void)
{
    static const kv_key_t bad_keys[] = {
        {"x", KV_UINT, KV_FIELD(demo_t, count), 10, 5, NULL, NULL, NULL}, /* min > max */
    };
    static const kv_table_t bad = {"bad", bad_keys, 1, sizeof(demo_t)};
    TEST_ASSERT_EQUAL_size_t(0, settings_schema_json(&bad, out, sizeof(out)));
    TEST_ASSERT_EQUAL_CHAR('\0', out[0]);
}

static void check_real(const kv_table_t *t)
{
    static char big[2 * SETTINGS_SCHEMA_MAX];
    const size_t n = settings_schema_json(t, big, sizeof(big));
    TEST_ASSERT_TRUE_MESSAGE(n > 0, t->name);
    TEST_ASSERT_TRUE_MESSAGE(n <= SETTINGS_SCHEMA_MAX, "fits the MQTT buffer");
    cJSON *root = cJSON_Parse(big);
    TEST_ASSERT_NOT_NULL_MESSAGE(root, "valid JSON");
    TEST_ASSERT_EQUAL_STRING(t->name, cJSON_GetObjectItem(root, "group")->valuestring);
    cJSON *keys = cJSON_GetObjectItem(root, "keys");
    TEST_ASSERT_EQUAL_INT((int)t->count, cJSON_GetArraySize(keys));
    for (size_t i = 0; i < t->count; i++) {
        cJSON *k = cJSON_GetArrayItem(keys, (int)i);
        TEST_ASSERT_EQUAL_STRING(t->keys[i].name, cJSON_GetObjectItem(k, "key")->valuestring);
        if (t->keys[i].type == KV_SECRET) {
            TEST_ASSERT_NULL_MESSAGE(cJSON_GetObjectItem(k, "default"), "no default for a secret");
        }
    }
    cJSON_Delete(root);
}

static void test_real_groups_fit(void)
{
    check_real(&cfg_net_table);
    check_real(&cfg_bath_table);
    check_real(&cfg_bed_table);
    check_real(&cfg_game_table);
}

static void test_real_spot_checks(void)
{
    TEST_ASSERT_TRUE(settings_schema_json(&cfg_bed_table, out, sizeof(out)) > 0);
    TEST_ASSERT_NOT_NULL(strstr(out, "{\"key\":\"sleep_timer_min\",\"type\":\"uint\",\"min\":0,\"max\":240,\"unit\":\"min\",\"default\":\"30\"}"));
    TEST_ASSERT_NOT_NULL(strstr(out, "{\"key\":\"scene_mood\",\"type\":\"uuid\"}"));
    TEST_ASSERT_TRUE(settings_schema_json(&cfg_net_table, out, sizeof(out)) > 0);
    TEST_ASSERT_NOT_NULL(strstr(out, "{\"key\":\"mqtt_pass\",\"type\":\"secret\",\"min\":8,\"max\":64}"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"choices\":[\"error\",\"warn\",\"info\",\"debug\"]"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_every_type);
    RUN_TEST(test_buffer_limits);
    RUN_TEST(test_invalid_table_refused);
    RUN_TEST(test_real_groups_fit);
    RUN_TEST(test_real_spot_checks);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
