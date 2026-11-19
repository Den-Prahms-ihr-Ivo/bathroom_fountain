/*
 * Tests for lib/kv_table – requirements CFG-01 ... CFG-04.
 * A demo group with one field of every type stands in for the real groups.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unity.h>

#include "kv_table.h"

#define UUID_A "3f2a9c1e-5b7d-4e8a-9c21-7d4e5f6a8b90"
#define UUID_B "AB12CD34-0000-4FFF-8AAA-1234567890EF"

typedef struct {
    uint8_t level;
    uint16_t gap_ms;
    uint32_t big;
    bool voice;
    uint8_t profile;
    char host[16];
    char pass[12];
    char scene[37];
} demo_t;

static int check_calls;

static bool host_check(const char *v)
{
    check_calls++;
    for (; *v; v++) {
        if (!((*v >= 'a' && *v <= 'z') || (*v >= '0' && *v <= '9') || *v == '.' || *v == '-')) {
            return false;
        }
    }
    return true;
}

static const kv_key_t demo_keys[] = {
    {"level", KV_UINT, KV_FIELD(demo_t, level), 1, 32, "16", NULL, NULL},
    {"gap_ms", KV_UINT, KV_FIELD(demo_t, gap_ms), 0, 2000, "150", NULL, NULL},
    {"big", KV_UINT, KV_FIELD(demo_t, big), 0, 4294967295u, NULL, NULL, NULL},
    {"voice", KV_BOOL, KV_FIELD(demo_t, voice), 0, 0, "on", NULL, NULL},
    {"profile", KV_CHOICE, KV_FIELD(demo_t, profile), 0, 0, NULL, "hype|zen|off", NULL},
    {"host", KV_TEXT, KV_FIELD(demo_t, host), 3, 15, NULL, NULL, host_check},
    {"pass", KV_SECRET, KV_FIELD(demo_t, pass), 4, 11, NULL, NULL, NULL},
    {"scene", KV_UUID, KV_FIELD(demo_t, scene), 0, 0, NULL, NULL, NULL},
};
#define DEMO_COUNT (sizeof(demo_keys) / sizeof(demo_keys[0]))
static const kv_table_t demo = {"demo", demo_keys, DEMO_COUNT, sizeof(demo_t)};

static demo_t d;

void setUp(void)
{
    memset(&d, 0x5A, sizeof(d));
    kv_defaults(&demo, &d);
    check_calls = 0;
}

void tearDown(void) {}

static void set_ok(const char *key, const char *value)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(KV_OK, kv_set(&demo, &d, key, value), value);
}

typedef char snapshot_t[DEMO_COUNT][64];

static void take_snapshot(snapshot_t snap)
{
    memset(snap, 0, sizeof(snapshot_t));
    for (size_t i = 0; i < DEMO_COUNT; i++) {
        kv_format(&demo, &d, demo_keys[i].name, true, snap[i], sizeof(snap[i]));
    }
}

static void set_fails(const char *key, const char *value, kv_result_t expected)
{
    static snapshot_t before;
    static snapshot_t after;
    take_snapshot(before);
    TEST_ASSERT_EQUAL_INT_MESSAGE(expected, kv_set(&demo, &d, key, value), value ? value : "(null)");
    take_snapshot(after);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(before, after, sizeof(snapshot_t), "a rejected set must change nothing");
}

static void expect_format(const char *key, bool reveal, const char *expected)
{
    char out[64];
    memset(out, 'Z', sizeof(out));
    TEST_ASSERT_TRUE_MESSAGE(kv_format(&demo, &d, key, reveal, out, sizeof(out)), key);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(expected, out, key);
}

static void test_table_is_valid_and_keys_are_found(void)
{
    TEST_ASSERT_TRUE(kv_table_valid(&demo));
    TEST_ASSERT_EQUAL_INT(0, kv_find(&demo, "level"));
    TEST_ASSERT_EQUAL_INT(7, kv_find(&demo, "scene"));
    TEST_ASSERT_EQUAL_INT(-1, kv_find(&demo, "leve"));
    TEST_ASSERT_EQUAL_INT(-1, kv_find(&demo, "level "));
    TEST_ASSERT_EQUAL_INT(-1, kv_find(&demo, NULL));
    TEST_ASSERT_EQUAL_INT(-1, kv_find(NULL, "level"));
}

static void test_defaults(void)
{
    TEST_ASSERT_TRUE(kv_table_valid(&demo));
    TEST_ASSERT_EQUAL_UINT8(16, d.level);
    TEST_ASSERT_EQUAL_UINT16(150, d.gap_ms);
    TEST_ASSERT_EQUAL_UINT32(0, d.big);
    TEST_ASSERT_TRUE(d.voice);
    TEST_ASSERT_EQUAL_UINT8(0, d.profile);
    TEST_ASSERT_EQUAL_STRING("", d.host);
    TEST_ASSERT_EQUAL_STRING("", d.pass);
    TEST_ASSERT_EQUAL_STRING("", d.scene);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, check_calls, "the check is not called for the empty string");
}

static void test_unknown_key_and_null_arguments(void)
{
    set_ok("level", "5");
    set_fails("levels", "5", KV_ERR_UNKNOWN_KEY);
    set_fails("", "5", KV_ERR_UNKNOWN_KEY);
    set_fails("LEVEL", "5", KV_ERR_UNKNOWN_KEY);
    set_fails("nokey", "not a number", KV_ERR_UNKNOWN_KEY); /* key first */
    set_fails(NULL, "5", KV_ERR_INVALID);
    set_fails("level", NULL, KV_ERR_INVALID);
    TEST_ASSERT_EQUAL_INT(KV_ERR_INVALID, kv_set(NULL, &d, "level", "5"));
    TEST_ASSERT_EQUAL_INT(KV_ERR_INVALID, kv_set(&demo, NULL, "level", "5"));
}

static void test_uint_syntax_and_range(void)
{
    set_ok("level", "1");
    TEST_ASSERT_EQUAL_UINT8(1, d.level);
    set_ok("level", "32");
    TEST_ASSERT_EQUAL_UINT8(32, d.level);
    set_ok("level", "007");
    TEST_ASSERT_EQUAL_UINT8(7, d.level);
    set_fails("level", "0", KV_ERR_RANGE);
    set_fails("level", "33", KV_ERR_RANGE);
    set_fails("level", "288", KV_ERR_RANGE); /* 288 = 0x120 must not wrap to 32 */
    set_fails("level", "", KV_ERR_INVALID);
    set_fails("level", "-1", KV_ERR_INVALID);
    set_fails("level", "+1", KV_ERR_INVALID);
    set_fails("level", " 1", KV_ERR_INVALID);
    set_fails("level", "1 ", KV_ERR_INVALID);
    set_fails("level", "1.0", KV_ERR_INVALID);
    set_fails("level", "0x10", KV_ERR_INVALID);
    set_fails("level", "on", KV_ERR_INVALID);

    set_ok("gap_ms", "2000");
    TEST_ASSERT_EQUAL_UINT16(2000, d.gap_ms);
    set_ok("gap_ms", "0");
    set_fails("gap_ms", "2001", KV_ERR_RANGE);
    set_fails("gap_ms", "65686", KV_ERR_RANGE); /* 65536 + 150 */

    set_ok("big", "4294967295");
    TEST_ASSERT_EQUAL_UINT32(4294967295u, d.big);
    set_fails("big", "4294967296", KV_ERR_RANGE);
    set_fails("big", "9999999999", KV_ERR_RANGE);
    set_fails("big", "10000000000", KV_ERR_INVALID); /* eleven digits */
    TEST_ASSERT_EQUAL_UINT32(4294967295u, d.big);
}

static void test_bool(void)
{
    set_ok("voice", "off");
    TEST_ASSERT_FALSE(d.voice);
    expect_format("voice", false, "off");
    set_ok("voice", "on");
    TEST_ASSERT_TRUE(d.voice);
    expect_format("voice", false, "on");
    set_fails("voice", "1", KV_ERR_INVALID);
    set_fails("voice", "true", KV_ERR_INVALID);
    set_fails("voice", "ON", KV_ERR_INVALID);
    set_fails("voice", "", KV_ERR_INVALID);
    set_fails("voice", "on ", KV_ERR_INVALID);
}

static void test_choice(void)
{
    expect_format("profile", false, "hype");
    set_ok("profile", "zen");
    TEST_ASSERT_EQUAL_UINT8(1, d.profile);
    expect_format("profile", false, "zen");
    set_ok("profile", "off");
    TEST_ASSERT_EQUAL_UINT8(2, d.profile);
    set_fails("profile", "ze", KV_ERR_INVALID);
    set_fails("profile", "zen|off", KV_ERR_INVALID);
    set_fails("profile", "Zen", KV_ERR_INVALID);
    set_fails("profile", "", KV_ERR_INVALID);
    set_fails("profile", "1", KV_ERR_INVALID);
    set_fails("profile", "|", KV_ERR_INVALID);
}

static void test_text_with_check_and_length(void)
{
    set_ok("host", "bad.example.org");
    TEST_ASSERT_EQUAL_STRING("bad.example.org", d.host);
    TEST_ASSERT_TRUE(check_calls > 0);
    set_ok("host", "a-b");
    set_fails("host", "ab", KV_ERR_RANGE);
    set_fails("host", "sixteen.chars.xx", KV_ERR_RANGE);
    set_fails("host", "has space", KV_ERR_INVALID); /* the check says no */
    set_fails("host", "UPPER", KV_ERR_INVALID);
    set_fails("host", "tab\there", KV_ERR_INVALID);
    set_fails("host", "umlaut\xC3\xA4", KV_ERR_INVALID);
    set_fails("host", "much.too.long.and.bad!", KV_ERR_INVALID); /* syntax before range */
    check_calls = 0;
    set_ok("host", ""); /* unset, although min is 3 */
    TEST_ASSERT_EQUAL_STRING("", d.host);
    TEST_ASSERT_EQUAL_INT(0, check_calls);
    expect_format("host", false, "");
}

static void test_secret_is_masked(void)
{
    set_ok("pass", "s3cr3t pw");
    TEST_ASSERT_EQUAL_STRING("s3cr3t pw", d.pass);
    expect_format("pass", false, "********");
    expect_format("pass", true, "s3cr3t pw");
    set_fails("pass", "abc", KV_ERR_RANGE);
    set_fails("pass", "twelve chars", KV_ERR_RANGE);
    set_fails("pass", "bell\x07", KV_ERR_INVALID);
    set_ok("pass", "");
    expect_format("pass", false, ""); /* nothing to hide */
    expect_format("host", true, "");
    set_ok("host", "plain.host");
    expect_format("host", false, "plain.host"); /* text is never masked */
}

static void test_uuid(void)
{
    set_ok("scene", UUID_A);
    TEST_ASSERT_EQUAL_STRING(UUID_A, d.scene);
    set_ok("scene", UUID_B);
    expect_format("scene", false, UUID_B); /* stored as given */
    set_fails("scene", "3f2a9c1e-5b7d-4e8a-9c21-7d4e5f6a8b9", KV_ERR_INVALID);
    set_fails("scene", "3f2a9c1e-5b7d-4e8a-9c21-7d4e5f6a8b901", KV_ERR_INVALID);
    set_fails("scene", "3f2a9c1e55b7d-4e8a-9c21-7d4e5f6a8b90", KV_ERR_INVALID);
    set_fails("scene", "3f2a9c1g-5b7d-4e8a-9c21-7d4e5f6a8b90", KV_ERR_INVALID);
    set_fails("scene", "3", KV_ERR_INVALID);
    set_ok("scene", ""); /* unset */
    TEST_ASSERT_EQUAL_STRING("", d.scene);
}

static void test_format_errors(void)
{
    char out[8];
    set_ok("host", "longer.name");
    memset(out, 'Z', sizeof(out));
    TEST_ASSERT_FALSE(kv_format(&demo, &d, "host", false, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
    memset(out, 'Z', sizeof(out));
    TEST_ASSERT_FALSE(kv_format(&demo, &d, "nokey", false, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_FALSE(kv_format(&demo, &d, "level", false, out, 0));
    TEST_ASSERT_FALSE(kv_format(&demo, &d, "level", false, NULL, 8));
    TEST_ASSERT_FALSE(kv_format(&demo, &d, NULL, false, out, sizeof(out)));
    set_ok("gap_ms", "1234");
    char exact[5];
    TEST_ASSERT_TRUE(kv_format(&demo, &d, "gap_ms", false, exact, sizeof(exact)));
    TEST_ASSERT_EQUAL_STRING("1234", exact);
    char small[4];
    TEST_ASSERT_FALSE(kv_format(&demo, &d, "gap_ms", false, small, sizeof(small)));
    TEST_ASSERT_EQUAL_STRING("", small);
}

static void test_round_trip_through_text(void) /* NVS persistence */
{
    set_ok("level", "31");
    set_ok("gap_ms", "777");
    set_ok("big", "3000000000");
    set_ok("voice", "off");
    set_ok("profile", "off");
    set_ok("host", "srv-1.example");
    set_ok("pass", "p w!~");
    set_ok("scene", UUID_B);
    static snapshot_t saved;
    static snapshot_t again;
    take_snapshot(saved);
    const demo_t before = d;

    memset(&d, 0xA5, sizeof(d));
    kv_defaults(&demo, &d);
    for (size_t i = 0; i < DEMO_COUNT; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(KV_OK, kv_set(&demo, &d, demo_keys[i].name, saved[i]), demo_keys[i].name);
    }
    take_snapshot(again);
    TEST_ASSERT_EQUAL_MEMORY(saved, again, sizeof(snapshot_t));
    TEST_ASSERT_EQUAL_UINT8(before.level, d.level);
    TEST_ASSERT_EQUAL_UINT32(before.big, d.big);
    TEST_ASSERT_EQUAL_UINT8(before.profile, d.profile);
    TEST_ASSERT_EQUAL_STRING(before.pass, d.pass);
}

/* One broken copy of the demo table per mistake. */
static bool broken(size_t index, kv_key_t replacement)
{
    static kv_key_t keys[DEMO_COUNT];
    memcpy(keys, demo_keys, sizeof(keys));
    keys[index] = replacement;
    const kv_table_t t = {"broken", keys, DEMO_COUNT, sizeof(demo_t)};
    return !kv_table_valid(&t);
}

static void test_table_mistakes_are_found(void)
{
    TEST_ASSERT_TRUE(kv_table_valid(&demo));
    TEST_ASSERT_FALSE(kv_table_valid(NULL));
    const kv_table_t empty = {"empty", demo_keys, 0, sizeof(demo_t)};
    TEST_ASSERT_FALSE(kv_table_valid(&empty));
    const kv_table_t small = {"small", demo_keys, DEMO_COUNT, offsetof(demo_t, scene) + 36};
    TEST_ASSERT_FALSE_MESSAGE(kv_table_valid(&small), "last field outside the struct");

    kv_key_t k = demo_keys[0];
    k.name = "gap_ms";
    TEST_ASSERT_TRUE_MESSAGE(broken(0, k), "duplicate name");
    k = demo_keys[0];
    k.name = "Level";
    TEST_ASSERT_TRUE_MESSAGE(broken(0, k), "upper case in a name");
    k.name = "";
    TEST_ASSERT_TRUE_MESSAGE(broken(0, k), "empty name");
    k.name = "a_name_that_is_longer_than_31_chars";
    TEST_ASSERT_TRUE_MESSAGE(broken(0, k), "name too long");
    k = demo_keys[0];
    k.max = 256;
    TEST_ASSERT_TRUE_MESSAGE(broken(0, k), "max does not fit uint8");
    k = demo_keys[0];
    k.min = 40;
    TEST_ASSERT_TRUE_MESSAGE(broken(0, k), "min > max");
    k = demo_keys[0];
    k.def = "0";
    TEST_ASSERT_TRUE_MESSAGE(broken(0, k), "default out of range");
    k = demo_keys[0];
    k.def = NULL;
    TEST_ASSERT_TRUE_MESSAGE(broken(0, k), "no default but min is 1");
    k = demo_keys[0];
    k.size = 3;
    TEST_ASSERT_TRUE_MESSAGE(broken(0, k), "uint of 3 bytes");
    k = demo_keys[3];
    k.def = "yes";
    TEST_ASSERT_TRUE_MESSAGE(broken(3, k), "bad bool default");
    k = demo_keys[4];
    k.choices = NULL;
    TEST_ASSERT_TRUE_MESSAGE(broken(4, k), "choice without names");
    k.choices = "a||b";
    TEST_ASSERT_TRUE_MESSAGE(broken(4, k), "empty choice name");
    k.choices = "a|b|a";
    TEST_ASSERT_TRUE_MESSAGE(broken(4, k), "duplicate choice");
    k.choices = "a|b";
    k.def = "c";
    TEST_ASSERT_TRUE_MESSAGE(broken(4, k), "default is not a choice");
    k = demo_keys[5];
    k.max = 16;
    TEST_ASSERT_TRUE_MESSAGE(broken(5, k), "text max leaves no room for the NUL");
    k = demo_keys[5];
    k.def = "NOT.OK";
    TEST_ASSERT_TRUE_MESSAGE(broken(5, k), "default fails the check");
    k = demo_keys[7];
    k.size = 36;
    TEST_ASSERT_TRUE_MESSAGE(broken(7, k), "uuid field too small");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_table_is_valid_and_keys_are_found);
    RUN_TEST(test_defaults);
    RUN_TEST(test_unknown_key_and_null_arguments);
    RUN_TEST(test_uint_syntax_and_range);
    RUN_TEST(test_bool);
    RUN_TEST(test_choice);
    RUN_TEST(test_text_with_check_and_length);
    RUN_TEST(test_secret_is_masked);
    RUN_TEST(test_uuid);
    RUN_TEST(test_format_errors);
    RUN_TEST(test_round_trip_through_text);
    RUN_TEST(test_table_mistakes_are_found);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
