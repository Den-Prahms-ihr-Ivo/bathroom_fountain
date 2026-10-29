/*
 * Tests for lib/settings – requirements CLAP-12, NFR-10, NFR-15.
 */
#include <stdint.h>
#include <string.h>
#include <unity.h>

#include "settings.h"

#define UUID_A "3f2a9c1e-5b7d-4e8a-9c21-7d4e5f6a8b90"
#define UUID_B "AB12CD34-0000-4FFF-8AAA-1234567890EF"

static settings_t s;

void setUp(void)
{
    memset(&s, 0x5A, sizeof(s));
    settings_defaults(&s);
}

void tearDown(void) {}

static void set_ok(const char *key, const char *value)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(SETTINGS_OK, settings_set(&s, key, value), value);
}

#define MAX_KEYS 32
typedef char snapshot_t[MAX_KEYS][96];

/* Every value as text (secrets revealed), so the comparison ignores padding
 * and bytes after a string's NUL. */
static void take_snapshot(snapshot_t snap)
{
    memset(snap, 0, sizeof(snapshot_t));
    for (size_t i = 0; i < settings_key_count() && i < MAX_KEYS; i++) {
        settings_format(&s, settings_key_name(i), true, snap[i], sizeof(snap[i]));
    }
}

static void set_fails(const char *key, const char *value, settings_result_t expected)
{
    static snapshot_t before;
    static snapshot_t after;
    take_snapshot(before);
    TEST_ASSERT_EQUAL_INT_MESSAGE(expected, settings_set(&s, key, value), value);
    take_snapshot(after);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(before, after, sizeof(snapshot_t), "a rejected set must change nothing");
}

static void expect_format(const char *key, bool reveal, const char *expected)
{
    char out[96];
    memset(out, 'Z', sizeof(out));
    TEST_ASSERT_TRUE_MESSAGE(settings_format(&s, key, reveal, out, sizeof(out)), key);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(expected, out, key);
}

static void fill(char *buf, size_t len, char c)
{
    memset(buf, c, len);
    buf[len] = '\0';
}

/* ------------------------------------------------------------------------ */
/* Defaults and key table                                                   */
/* ------------------------------------------------------------------------ */

static void test_defaults(void)
{
    TEST_ASSERT_EQUAL_STRING("", s.wifi_ssid);
    TEST_ASSERT_EQUAL_STRING("", s.wifi_pass);
    TEST_ASSERT_EQUAL_STRING("", s.hue_host);
    TEST_ASSERT_EQUAL_STRING("", s.hue_key);
    TEST_ASSERT_EQUAL_STRING("", s.scene_normal);
    TEST_ASSERT_EQUAL_STRING("", s.scene_ambient);
    TEST_ASSERT_EQUAL_UINT32(4000, s.transition_ms);
    TEST_ASSERT_EQUAL_UINT8(32, s.volume_max);
    TEST_ASSERT_EQUAL_UINT16(150, s.clap_min_gap_ms);
    TEST_ASSERT_EQUAL_UINT16(600, s.clap_max_gap_ms);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 6.0f, s.clap_level_diff_db);
    TEST_ASSERT_EQUAL_UINT16(300, s.clap_quiet_before_ms);
    TEST_ASSERT_EQUAL_UINT16(400, s.clap_quiet_after_ms);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.5f, s.clap_gap_factor);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 12.0f, s.clap_drop_db);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 12.0f, s.clap_onset_db);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, -40.0f, s.clap_min_peak_dbfs);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 20.0f, s.mic_hp_hz);
    TEST_ASSERT_EQUAL_UINT8(16, s.mic_shift);
    TEST_ASSERT_FALSE(settings_is_complete(&s));
}

static void test_key_table(void)
{
    const char *keys[] = {"wifi_ssid", "wifi_pass", "hue_host", "hue_key", "scene_normal", "scene_ambient",
                          "transition_ms", "volume_max", "clap_min_gap_ms", "clap_max_gap_ms",
                          "clap_level_diff_db", "clap_quiet_before_ms", "clap_quiet_after_ms", "clap_gap_factor",
                          "clap_drop_db", "clap_onset_db", "clap_min_peak_dbfs", "mic_hp_hz", "mic_shift"};
    const size_t n = sizeof(keys) / sizeof(keys[0]);
    TEST_ASSERT_EQUAL_size_t(n, settings_key_count());
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_NOT_NULL(settings_key_name(i));
        TEST_ASSERT_EQUAL_STRING(keys[i], settings_key_name(i));
    }
    TEST_ASSERT_NULL(settings_key_name(n));
}

static void test_unknown_key_and_null_arguments(void)
{
    set_fails("transition", "4000", SETTINGS_ERR_UNKNOWN_KEY);
    set_fails("TRANSITION_MS", "4000", SETTINGS_ERR_UNKNOWN_KEY);
    set_fails(NULL, "4000", SETTINGS_ERR_INVALID);
    set_fails("transition_ms", NULL, SETTINGS_ERR_INVALID);
}

/* ------------------------------------------------------------------------ */
/* Unsigned integers                                                        */
/* ------------------------------------------------------------------------ */

static void test_uint_syntax(void)
{
    set_ok("transition_ms", "4500");
    TEST_ASSERT_EQUAL_UINT32(4500, s.transition_ms);
    set_ok("transition_ms", "0003500");
    TEST_ASSERT_EQUAL_UINT32(3500, s.transition_ms);
    const char *bad[] = {"", "45x", " 4500", "4500 ", "+4500", "-1", "4.5", "0x10"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        set_fails("transition_ms", bad[i], SETTINGS_ERR_INVALID);
    }
}

static void test_uint_range(void)
{
    set_fails("transition_ms", "2999", SETTINGS_ERR_RANGE);
    set_fails("transition_ms", "5001", SETTINGS_ERR_RANGE);
    set_fails("transition_ms", "99999999999999999999", SETTINGS_ERR_RANGE);
    set_fails("transition_ms", "4294971296", SETTINGS_ERR_RANGE); /* 2^32 + 4000 must not wrap to 4000 */
    set_ok("transition_ms", "3000");
    set_ok("transition_ms", "5000");
    TEST_ASSERT_EQUAL_UINT32(5000, s.transition_ms);

    set_fails("volume_max", "0", SETTINGS_ERR_RANGE);
    set_fails("volume_max", "33", SETTINGS_ERR_RANGE);
    set_fails("volume_max", "257", SETTINGS_ERR_RANGE); /* must not wrap to 1 */
    set_ok("volume_max", "1");
    TEST_ASSERT_EQUAL_UINT8(1, s.volume_max);

    set_fails("mic_shift", "7", SETTINGS_ERR_RANGE);
    set_fails("mic_shift", "25", SETTINGS_ERR_RANGE);
}

/* ------------------------------------------------------------------------ */
/* Floats                                                                   */
/* ------------------------------------------------------------------------ */

static void test_float_syntax(void)
{
    set_ok("clap_onset_db", "10");
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 10.0f, s.clap_onset_db);
    set_ok("clap_onset_db", "10.5");
    set_ok("clap_onset_db", "10.25");
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 10.25f, s.clap_onset_db);
    const char *bad[] = {"", "10.", ".5", "1e1", "nan", "NAN", "inf", "-", "10.125", " 10", "10 ", "10dB", "--1",
                         "+10", "1,5"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        set_fails("clap_onset_db", bad[i], SETTINGS_ERR_INVALID);
    }
}

static void test_float_range(void)
{
    set_ok("clap_min_peak_dbfs", "-90");
    set_ok("clap_min_peak_dbfs", "0");
    set_ok("clap_min_peak_dbfs", "-40.5");
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, -40.5f, s.clap_min_peak_dbfs);
    set_fails("clap_min_peak_dbfs", "0.5", SETTINGS_ERR_RANGE);
    set_fails("clap_min_peak_dbfs", "-90.01", SETTINGS_ERR_RANGE);
    set_fails("clap_drop_db", "0.99", SETTINGS_ERR_RANGE);
    set_fails("clap_gap_factor", "4.01", SETTINGS_ERR_RANGE);
    set_fails("mic_hp_hz", "99999999999999999999", SETTINGS_ERR_RANGE);
    set_ok("mic_hp_hz", "0");
    set_ok("clap_level_diff_db", "0");
}

static void test_gap_limits_must_stay_ordered(void)
{
    set_fails("clap_min_gap_ms", "600", SETTINGS_ERR_CONFLICT);
    set_ok("clap_max_gap_ms", "800");
    set_ok("clap_min_gap_ms", "600");
    set_fails("clap_max_gap_ms", "600", SETTINGS_ERR_CONFLICT);
    set_ok("clap_max_gap_ms", "601");
    set_fails("clap_min_gap_ms", "40", SETTINGS_ERR_RANGE); /* range before conflict */
}

/* ------------------------------------------------------------------------ */
/* Strings                                                                  */
/* ------------------------------------------------------------------------ */

static void test_wifi_ssid(void)
{
    char buf[40];
    fill(buf, 32, 'w');
    set_ok("wifi_ssid", buf);
    fill(buf, 33, 'w');
    set_fails("wifi_ssid", buf, SETTINGS_ERR_RANGE);
    set_fails("wifi_ssid", "", SETTINGS_ERR_RANGE);
    set_fails("wifi_ssid", "a\tb", SETTINGS_ERR_INVALID);
    set_fails("wifi_ssid", "a\x7f", SETTINGS_ERR_INVALID);
    set_ok("wifi_ssid", " My Wifi ");
    TEST_ASSERT_EQUAL_STRING(" My Wifi ", s.wifi_ssid);
    set_ok("wifi_ssid", "Caf\xc3\xa9");
    TEST_ASSERT_EQUAL_STRING("Caf\xc3\xa9", s.wifi_ssid);
}

static void test_wifi_pass(void)
{
    char buf[70];
    set_ok("wifi_pass", "");
    set_fails("wifi_pass", "1234567", SETTINGS_ERR_RANGE);
    set_ok("wifi_pass", "12345678");
    fill(buf, 63, 'p');
    set_ok("wifi_pass", buf);
    fill(buf, 64, 'p');
    set_fails("wifi_pass", buf, SETTINGS_ERR_RANGE);
    set_fails("wifi_pass", "pass\nword", SETTINGS_ERR_INVALID);
    set_fails("wifi_pass", "pass\xc3\xa9word", SETTINGS_ERR_INVALID);
    set_ok("wifi_pass", "with space & ~!");
    TEST_ASSERT_EQUAL_STRING("with space & ~!", s.wifi_pass);
}

static void test_hue_host_and_key(void)
{
    char buf[70];
    set_ok("hue_host", "192.168.1.10");
    set_ok("hue_host", "hue-bridge.local");
    set_fails("hue_host", "bad host", SETTINGS_ERR_INVALID);
    set_fails("hue_host", "a_b", SETTINGS_ERR_INVALID);
    set_fails("hue_host", "", SETTINGS_ERR_RANGE);
    fill(buf, 64, 'h');
    set_fails("hue_host", buf, SETTINGS_ERR_RANGE);

    set_ok("hue_key", "8x2Fk-QpLw9mZ0aB3cD_");
    set_fails("hue_key", "abc/def", SETTINGS_ERR_INVALID);
    set_fails("hue_key", "", SETTINGS_ERR_RANGE);
    fill(buf, 64, 'k');
    set_ok("hue_key", buf);
    fill(buf, 65, 'k');
    set_fails("hue_key", buf, SETTINGS_ERR_RANGE);
}

static void test_scene_ids(void)
{
    set_ok("scene_normal", UUID_A);
    set_ok("scene_ambient", UUID_B);
    TEST_ASSERT_EQUAL_STRING(UUID_B, s.scene_ambient);
    set_fails("scene_normal", "3f2a9c1e-5b7d-4e8a-9c21-7d4e5f6a8b9", SETTINGS_ERR_INVALID);
    set_fails("scene_normal", "3f2a9c1e-5b7d-4e8a-9c21-7d4e5f6a8b9g", SETTINGS_ERR_INVALID);
    set_fails("scene_normal", "3f2a9c1e5b7d-4e8a-9c21-7d4e5f6a8b90a", SETTINGS_ERR_INVALID);
    set_fails("scene_normal", "", SETTINGS_ERR_INVALID);
    TEST_ASSERT_EQUAL_STRING(UUID_A, s.scene_normal);
}

/* ------------------------------------------------------------------------ */
/* Formatting                                                               */
/* ------------------------------------------------------------------------ */

static void test_format_numbers(void)
{
    expect_format("transition_ms", false, "4000");
    expect_format("volume_max", false, "32");
    expect_format("clap_onset_db", false, "12.0");
    expect_format("clap_min_peak_dbfs", false, "-40.0");
    expect_format("clap_gap_factor", false, "1.5");
    expect_format("mic_shift", false, "16");
    set_ok("clap_level_diff_db", "1.25");
    expect_format("clap_level_diff_db", false, "1.25");
    set_ok("clap_level_diff_db", "0.1");
    expect_format("clap_level_diff_db", false, "0.1");
    set_ok("mic_hp_hz", "150");
    expect_format("mic_hp_hz", false, "150.0");
}

static void test_format_masks_secrets(void)
{
    expect_format("wifi_pass", false, "");
    expect_format("hue_key", false, "");
    set_ok("wifi_pass", "supersecret");
    set_ok("hue_key", "abc-123");
    set_ok("wifi_ssid", "Home");
    expect_format("wifi_pass", false, "********");
    expect_format("hue_key", false, "********");
    expect_format("wifi_pass", true, "supersecret");
    expect_format("hue_key", true, "abc-123");
    expect_format("wifi_ssid", false, "Home");
}

static void test_format_errors(void)
{
    char out[8];
    memset(out, 'Z', sizeof(out));
    TEST_ASSERT_FALSE(settings_format(&s, "nope", true, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
    memset(out, 'Z', sizeof(out));
    TEST_ASSERT_FALSE_MESSAGE(settings_format(&s, "clap_min_peak_dbfs", true, out, 5), "\"-40.0\" needs 6 bytes");
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_TRUE(settings_format(&s, "clap_min_peak_dbfs", true, out, 6));
    TEST_ASSERT_EQUAL_STRING("-40.0", out);
}

static void test_round_trip_through_text(void) /* NVS persistence */
{
    set_ok("wifi_ssid", " Home net ");
    set_ok("wifi_pass", "p@ss word!");
    set_ok("hue_host", "192.168.178.20");
    set_ok("hue_key", "Ab-9_x");
    set_ok("scene_normal", UUID_A);
    set_ok("scene_ambient", UUID_B);
    set_ok("transition_ms", "3500");
    set_ok("volume_max", "24");
    set_ok("clap_max_gap_ms", "700");
    set_ok("clap_min_gap_ms", "180");
    set_ok("clap_level_diff_db", "5.25");
    set_ok("clap_quiet_before_ms", "350");
    set_ok("clap_quiet_after_ms", "450");
    set_ok("clap_gap_factor", "1.75");
    set_ok("clap_drop_db", "10.5");
    set_ok("clap_onset_db", "9.75");
    set_ok("clap_min_peak_dbfs", "-37.5");
    set_ok("mic_hp_hz", "180");
    set_ok("mic_shift", "14");

    settings_t copy;
    settings_defaults(&copy);
    char text[96];
    for (size_t i = 0; i < settings_key_count(); i++) {
        const char *key = settings_key_name(i);
        TEST_ASSERT_TRUE(settings_format(&s, key, true, text, sizeof(text)));
        TEST_ASSERT_EQUAL_INT_MESSAGE(SETTINGS_OK, settings_set(&copy, key, text), key);
    }
    char a[96];
    char b[96];
    for (size_t i = 0; i < settings_key_count(); i++) {
        const char *key = settings_key_name(i);
        settings_format(&s, key, true, a, sizeof(a));
        settings_format(&copy, key, true, b, sizeof(b));
        TEST_ASSERT_EQUAL_STRING_MESSAGE(a, b, key);
    }
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 9.75f, copy.clap_onset_db);
    TEST_ASSERT_EQUAL_STRING(" Home net ", copy.wifi_ssid);
}

static void test_complete_needs_wifi_bridge_and_scenes(void)
{
    set_ok("wifi_ssid", "Home");
    set_ok("hue_host", "192.168.1.10");
    set_ok("hue_key", "abc");
    set_ok("scene_normal", UUID_A);
    TEST_ASSERT_FALSE(settings_is_complete(&s));
    set_ok("scene_ambient", UUID_B);
    TEST_ASSERT_TRUE(settings_is_complete(&s));
}

/* ------------------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults);
    RUN_TEST(test_key_table);
    RUN_TEST(test_unknown_key_and_null_arguments);
    RUN_TEST(test_uint_syntax);
    RUN_TEST(test_uint_range);
    RUN_TEST(test_float_syntax);
    RUN_TEST(test_float_range);
    RUN_TEST(test_gap_limits_must_stay_ordered);
    RUN_TEST(test_wifi_ssid);
    RUN_TEST(test_wifi_pass);
    RUN_TEST(test_hue_host_and_key);
    RUN_TEST(test_scene_ids);
    RUN_TEST(test_format_numbers);
    RUN_TEST(test_format_masks_secrets);
    RUN_TEST(test_format_errors);
    RUN_TEST(test_round_trip_through_text);
    RUN_TEST(test_complete_needs_wifi_bridge_and_scenes);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
