/*
 * Tests for lib/status_json – docs/PROTOCOL.md, Status and Events.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unity.h>

#include "status_json.h"

static status_common_t c;
static char out[STATUS_JSON_MAX + 64];

#define COMMON_BATH                                                                                             \
    "{\"online\":true,\"node\":\"bath\",\"fw\":\"2.0.0\",\"built\":\"2026-11-15T14:02:11Z\",\"boot\":17,"       \
    "\"reset\":\"poweron\",\"uptime_s\":8123,\"wifi_rssi\":-58,\"ip\":\"192.168.1.40\",\"heap_min\":141232,"    \
    "\"groups\":[\"settings\",\"net\",\"bath\"],\"settings_version\":12,"

void setUp(void)
{
    memset(&c, 0, sizeof(c));
    c.fw = "2.0.0";
    c.built = "2026-11-15T14:02:11Z";
    c.boot = 17;
    c.reset = "poweron";
    c.uptime_s = 8123;
    c.wifi_rssi = -58;
    c.ip = "192.168.1.40";
    c.heap_min = 141232;
    c.groups[0] = "settings";
    c.groups[1] = "net";
    c.groups[2] = "bath";
    c.group_count = 3;
    c.settings_version = 12;
    memset(out, 'Z', sizeof(out));
}

void tearDown(void) {}

static void expect(const char *want, size_t got)
{
    TEST_ASSERT_EQUAL_STRING(want, out);
    TEST_ASSERT_EQUAL_size_t(strlen(want), got);
}

static void expect_fail(size_t got)
{
    TEST_ASSERT_EQUAL_size_t(0, got);
    TEST_ASSERT_EQUAL_CHAR('\0', out[0]);
}

/* ---- bath --------------------------------------------------------------- */

static status_bath_t bath_example(void)
{
    status_bath_t b;
    memset(&b, 0, sizeof(b));
    b.mode = STATUS_BATH_AMBIENT;
    b.volume_ambient = 14;
    b.volume_cold = 10;
    b.hue = "ok";
    b.sd = "ok";
    const char *n[] = {"A", "B", "C", "V"};
    const uint16_t k[] = {29, 12, 8, 4};
    for (int i = 0; i < 4; i++) {
        strcpy(b.tracks[i].name, n[i]);
        b.tracks[i].count = k[i];
    }
    b.track_count = 4;
    b.cold_running = true;
    b.cold_phase = "cold";
    b.cold_remaining_s = 41;
    b.cold_profile = "hype";
    b.cold_switch = "hype";
    return b;
}

static void test_bath_protocol_example(void)
{
    const status_bath_t b = bath_example();
    expect(COMMON_BATH "\"mode\":\"ambient\",\"volumes\":{\"ambient\":14,\"cold\":10},\"hue\":\"ok\",\"sd\":\"ok\","
                       "\"tracks\":{\"A\":29,\"B\":12,\"C\":8,\"V\":4},"
                       "\"cold\":{\"phase\":\"cold\",\"remaining_s\":41,\"profile\":\"hype\",\"switch\":\"hype\"}}",
           status_json_bath(&c, &b, out, sizeof(out)));
}

static void test_bath_idle_and_nulls(void)
{
    status_bath_t b = bath_example();
    b.mode = STATUS_BATH_NORMAL;
    b.cold_running = false;
    b.hue = NULL;
    b.track_count = 0;
    expect(COMMON_BATH "\"mode\":\"normal\",\"volumes\":{\"ambient\":14,\"cold\":10},\"hue\":null,\"sd\":\"ok\","
                       "\"tracks\":{},\"cold\":null}",
           status_json_bath(&c, &b, out, sizeof(out)));
    b.mode = STATUS_BATH_COLD;
    TEST_ASSERT_TRUE(status_json_bath(&c, &b, out, sizeof(out)) > 0);
    TEST_ASSERT_NOT_NULL(strstr(out, "\"mode\":\"cold\""));
}

static void test_bath_refusals(void)
{
    status_bath_t b = bath_example();
    expect_fail(status_json_bath(NULL, &b, out, sizeof(out)));
    expect_fail(status_json_bath(&c, NULL, out, sizeof(out)));
    TEST_ASSERT_EQUAL_size_t(0, status_json_bath(&c, &b, NULL, 100));
    b.mode = (status_bath_mode_t)3;
    expect_fail(status_json_bath(&c, &b, out, sizeof(out)));
    b = bath_example();
    b.track_count = STATUS_MAX_TRACK_FOLDERS + 1;
    expect_fail(status_json_bath(&c, &b, out, sizeof(out)));
    b = bath_example();
    c.group_count = STATUS_MAX_GROUPS + 1;
    expect_fail(status_json_bath(&c, &b, out, sizeof(out)));
}

static void test_buffer_exactly_big_enough(void)
{
    const status_bath_t b = bath_example();
    const size_t len = status_json_bath(&c, &b, out, sizeof(out));
    TEST_ASSERT_TRUE(len > 0);
    memset(out, 'Z', sizeof(out));
    TEST_ASSERT_EQUAL_size_t(len, status_json_bath(&c, &b, out, len + 1));
    TEST_ASSERT_EQUAL_CHAR('Z', out[len + 1]);
    memset(out, 'Z', sizeof(out));
    expect_fail(status_json_bath(&c, &b, out, len));
    memset(out, 'Z', sizeof(out));
    TEST_ASSERT_EQUAL_size_t(0, status_json_bath(&c, &b, out, 0));
    TEST_ASSERT_EQUAL_CHAR_MESSAGE('Z', out[0], "out_size 0: nothing written");
}

static void test_common_numbers_and_escaping(void)
{
    status_bath_t b = bath_example();
    b.cold_running = false;
    b.track_count = 0;
    c.fw = "2.0.0-\"rc\"\\1";
    c.reset = "line\nbreak\ttab\x01";
    c.ip = "";
    c.built = "2026/11/15 \xc3\xa4";
    c.uptime_s = 4294967295u;
    c.heap_min = 3000000000u;
    c.wifi_rssi = 0;
    c.boot = 0;
    c.group_count = 0;
    c.settings_version = 2147483647;
    expect("{\"online\":true,\"node\":\"bath\",\"fw\":\"2.0.0-\\\"rc\\\"\\\\1\",\"built\":\"2026/11/15 \xc3\xa4\","
           "\"boot\":0,\"reset\":\"line\\nbreak\\ttab\\u0001\",\"uptime_s\":4294967295,\"wifi_rssi\":0,\"ip\":\"\","
           "\"heap_min\":3000000000,\"groups\":[],\"settings_version\":2147483647,"
           "\"mode\":\"ambient\",\"volumes\":{\"ambient\":14,\"cold\":10},\"hue\":\"ok\",\"sd\":\"ok\","
           "\"tracks\":{},\"cold\":null}",
           status_json_bath(&c, &b, out, sizeof(out)));
}

/* ---- speaker ------------------------------------------------------------ */

static status_speaker_t speaker_example(void)
{
    status_speaker_t s;
    memset(&s, 0, sizeof(s));
    s.playing = STATUS_PLAY_MUSIC;
    s.folder = "sleep";
    s.kept = true;
    s.volume = 8;
    s.sd = "ok";
    s.folders[0] = "meditation";
    s.folders[1] = "mood_rain";
    s.folders[2] = "sleep";
    s.folder_count = 3;
    s.armed_state = "waiting";
    s.armed_version = 41;
    s.armed_alarm_utc = 1795030000;
    s.link_up = true;
    return s;
}

static void test_speaker(void)
{
    c.groups[2] = NULL;
    c.group_count = 2;
    const status_speaker_t s = speaker_example();
    expect("{\"online\":true,\"node\":\"bedroom_speaker\",\"fw\":\"2.0.0\",\"built\":\"2026-11-15T14:02:11Z\","
           "\"boot\":17,\"reset\":\"poweron\",\"uptime_s\":8123,\"wifi_rssi\":-58,\"ip\":\"192.168.1.40\","
           "\"heap_min\":141232,\"groups\":[\"settings\",\"net\"],\"settings_version\":12,"
           "\"playing\":\"music\",\"folder\":\"sleep\",\"kept\":true,\"volume\":8,\"sd\":\"ok\","
           "\"folders\":[\"meditation\",\"mood_rain\",\"sleep\"],"
           "\"armed\":{\"state\":\"waiting\",\"version\":41,\"alarm_utc\":1795030000},\"link\":\"up\"}",
           status_json_speaker(&c, &s, out, sizeof(out)));
}

static void test_speaker_idle(void)
{
    status_speaker_t s = speaker_example();
    s.playing = STATUS_PLAY_NONE;
    s.folder = "";
    s.kept = false;
    s.folder_count = 0;
    s.armed_state = "none";
    s.armed_version = 0;
    s.armed_alarm_utc = -1;
    s.link_up = false;
    const size_t n = status_json_speaker(&c, &s, out, sizeof(out));
    TEST_ASSERT_TRUE(n > 0);
    const char *tail = "\"playing\":\"none\",\"folder\":\"\",\"kept\":false,\"volume\":8,\"sd\":\"ok\",\"folders\":[],"
                       "\"armed\":{\"state\":\"none\",\"version\":0,\"alarm_utc\":null},\"link\":\"down\"}";
    TEST_ASSERT_EQUAL_STRING(tail, out + n - strlen(tail));
    s.playing = STATUS_PLAY_ALARM;
    TEST_ASSERT_TRUE(status_json_speaker(&c, &s, out, sizeof(out)) > 0);
    TEST_ASSERT_NOT_NULL(strstr(out, "\"playing\":\"alarm\""));
}

static void test_speaker_refusals(void)
{
    status_speaker_t s = speaker_example();
    s.playing = (status_playing_t)3;
    expect_fail(status_json_speaker(&c, &s, out, sizeof(out)));
    s = speaker_example();
    s.folder_count = STATUS_MAX_FOLDERS + 1;
    expect_fail(status_json_speaker(&c, &s, out, sizeof(out)));
    expect_fail(status_json_speaker(&c, NULL, out, sizeof(out)));
    expect_fail(status_json_speaker(NULL, &s, out, sizeof(out)));
}

static void test_speaker_full_folder_list_fits(void)
{
    status_speaker_t s = speaker_example();
    static const char name[] = "abcdefghijklmnopqrstuvw"; /* 23, the longest */
    for (int i = 0; i < STATUS_MAX_FOLDERS; i++) {
        s.folders[i] = name;
    }
    s.folder_count = STATUS_MAX_FOLDERS;
    TEST_ASSERT_TRUE(status_json_speaker(&c, &s, out, sizeof(out)) > 0);
}

/* ---- bedside ------------------------------------------------------------ */

static status_bedside_t bedside_example(void)
{
    status_bedside_t b;
    memset(&b, 0, sizeof(b));
    b.time = "2026-11-16T06:30:12";
    b.time_source = STATUS_TIME_SNTP;
    b.armed_switch = true;
    b.has_next_alarm = true;
    b.next_alarm_slot = 1;
    b.next_alarm_local = "2026-11-16T06:45:00";
    b.alarm = "sunrise";
    b.light = 420;
    b.mode = STATUS_BED_SLEEP;
    b.fading = false;
    b.candles = true;
    b.volume_meditation = 10;
    b.volume_mood = 14;
    b.volume_sleep = 8;
    b.remaining_s = 1712;
    b.speaker_up = true;
    b.game_state = "idle";
    b.game_hits = 0;
    b.game_needed = 3;
    return b;
}

static void test_bedside(void)
{
    c.groups[2] = "bed";
    c.groups[3] = "game";
    c.group_count = 4;
    const status_bedside_t b = bedside_example();
    expect("{\"online\":true,\"node\":\"bedside\",\"fw\":\"2.0.0\",\"built\":\"2026-11-15T14:02:11Z\","
           "\"boot\":17,\"reset\":\"poweron\",\"uptime_s\":8123,\"wifi_rssi\":-58,\"ip\":\"192.168.1.40\","
           "\"heap_min\":141232,\"groups\":[\"settings\",\"net\",\"bed\",\"game\"],\"settings_version\":12,"
           "\"time\":\"2026-11-16T06:30:12\",\"time_source\":\"sntp\",\"armed_switch\":true,"
           "\"next_alarm\":{\"slot\":1,\"local\":\"2026-11-16T06:45:00\"},\"alarm\":\"sunrise\",\"light\":420,"
           "\"mode\":\"sleep\",\"fading\":false,\"candles\":true,"
           "\"volumes\":{\"meditation\":10,\"mood\":14,\"sleep\":8},\"remaining_s\":1712,\"speaker\":\"up\","
           "\"game\":{\"state\":\"idle\",\"hits\":0,\"needed\":3}}",
           status_json_bedside(&c, &b, out, sizeof(out)));
}

static void test_bedside_nulls_and_names(void)
{
    status_bedside_t b = bedside_example();
    b.time = NULL;
    b.time_source = STATUS_TIME_NONE;
    b.has_next_alarm = false;
    b.mode = STATUS_BED_NONE;
    b.remaining_s = -1;
    b.speaker_up = false;
    const size_t n = status_json_bedside(&c, &b, out, sizeof(out));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_NOT_NULL(strstr(out, "\"time\":null,\"time_source\":\"none\",\"armed_switch\":true,\"next_alarm\":null,"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"mode\":\"none\""));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"remaining_s\":null,\"speaker\":\"down\","));
    b.time_source = STATUS_TIME_RTC;
    b.mode = STATUS_BED_MEDITATION;
    b.remaining_s = 0;
    status_json_bedside(&c, &b, out, sizeof(out));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"time_source\":\"rtc\""));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"mode\":\"meditation\""));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"remaining_s\":0,"));
    b.mode = STATUS_BED_MOOD;
    status_json_bedside(&c, &b, out, sizeof(out));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"mode\":\"mood\""));
}

static void test_bedside_refusals(void)
{
    status_bedside_t b = bedside_example();
    b.mode = (status_bed_mode_t)4;
    expect_fail(status_json_bedside(&c, &b, out, sizeof(out)));
    b = bedside_example();
    b.time_source = (status_time_source_t)3;
    expect_fail(status_json_bedside(&c, &b, out, sizeof(out)));
    expect_fail(status_json_bedside(&c, NULL, out, sizeof(out)));
}

/* ---- events ------------------------------------------------------------- */

static void test_event_mode(void)
{
    expect("{\"type\":\"mode\",\"mode\":\"ambient\",\"by\":\"clap\"}",
           status_json_event_mode("ambient", "clap", -1, NULL, -1, out, sizeof(out)));
    expect("{\"type\":\"mode\",\"mode\":\"sleep\",\"by\":\"button\",\"continued\":true,\"t\":1795000000}",
           status_json_event_mode("sleep", "button", 1, NULL, 1795000000, out, sizeof(out)));
    expect("{\"type\":\"mode\",\"mode\":\"sleep\",\"by\":\"server\",\"continued\":false}",
           status_json_event_mode("sleep", "server", 0, NULL, -1, out, sizeof(out)));
    expect("{\"type\":\"mode\",\"mode\":\"mood\",\"by\":\"button\",\"folder\":\"mood_jazz\",\"t\":0}",
           status_json_event_mode("mood", "button", -1, "mood_jazz", 0, out, sizeof(out)));
    expect_fail(status_json_event_mode(NULL, "button", -1, NULL, -1, out, sizeof(out)));
    expect_fail(status_json_event_mode("mood", NULL, -1, NULL, -1, out, sizeof(out)));
    expect_fail(status_json_event_mode("sleep", "button", 2, NULL, -1, out, sizeof(out)));
    expect_fail(status_json_event_mode("sleep", "button", -2, NULL, -1, out, sizeof(out)));
}

static void test_event_alarm(void)
{
    expect("{\"type\":\"alarm\",\"event\":\"ring\",\"slot\":1,\"game\":true}",
           status_json_event_alarm("ring", 1, 1, NULL, -1, out, sizeof(out)));
    expect("{\"type\":\"alarm\",\"event\":\"stop\",\"reason\":\"game\",\"t\":1795000123}",
           status_json_event_alarm("stop", 0, -1, "game", 1795000123, out, sizeof(out)));
    expect("{\"type\":\"alarm\",\"event\":\"ring\",\"slot\":6,\"game\":false}",
           status_json_event_alarm("ring", 6, 0, NULL, -1, out, sizeof(out)));
    expect_fail(status_json_event_alarm(NULL, 1, 1, NULL, -1, out, sizeof(out)));
    expect_fail(status_json_event_alarm("ring", 1, 2, NULL, -1, out, sizeof(out)));
}

static void test_event_error_and_cmd(void)
{
    expect("{\"type\":\"error\",\"where\":\"hue\",\"text\":\"bridge unreachable\"}",
           status_json_event_error("hue", "bridge unreachable", -1, out, sizeof(out)));
    expect("{\"type\":\"error\",\"where\":\"sd\",\"text\":\"no \\\"sleep\\\" folder\",\"t\":5}",
           status_json_event_error("sd", "no \"sleep\" folder", 5, out, sizeof(out)));
    expect_fail(status_json_event_error(NULL, "x", -1, out, sizeof(out)));
    expect_fail(status_json_event_error("x", NULL, -1, out, sizeof(out)));
    expect("{\"type\":\"cmd\",\"name\":\"frobnicate\",\"ok\":false,\"why\":\"unknown\"}",
           status_json_event_cmd("frobnicate", false, "unknown", -1, out, sizeof(out)));
    expect("{\"type\":\"cmd\",\"name\":\"button\",\"ok\":true,\"t\":12}",
           status_json_event_cmd("button", true, NULL, 12, out, sizeof(out)));
    expect_fail(status_json_event_cmd(NULL, true, NULL, -1, out, sizeof(out)));
}

static void test_too_long_for_mqtt(void)
{
    /* a text longer than the MQTT buffer is refused even with a big out */
    static char big_text[STATUS_JSON_MAX + 1];
    memset(big_text, 'x', sizeof(big_text) - 1);
    big_text[sizeof(big_text) - 1] = '\0';
    static char big_out[2 * STATUS_JSON_MAX];
    TEST_ASSERT_EQUAL_size_t(0, status_json_event_error("hue", big_text, -1, big_out, sizeof(big_out)));
    TEST_ASSERT_EQUAL_CHAR('\0', big_out[0]);
    /* just at the limit: {"type":"error","where":"","text":"..."} is 37 + n characters */
    big_text[STATUS_JSON_MAX - 37] = '\0';
    TEST_ASSERT_EQUAL_size_t(STATUS_JSON_MAX, status_json_event_error("", big_text, -1, big_out, sizeof(big_out)));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_bath_protocol_example);
    RUN_TEST(test_bath_idle_and_nulls);
    RUN_TEST(test_bath_refusals);
    RUN_TEST(test_buffer_exactly_big_enough);
    RUN_TEST(test_common_numbers_and_escaping);
    RUN_TEST(test_speaker);
    RUN_TEST(test_speaker_idle);
    RUN_TEST(test_speaker_refusals);
    RUN_TEST(test_speaker_full_folder_list_fits);
    RUN_TEST(test_bedside);
    RUN_TEST(test_bedside_nulls_and_names);
    RUN_TEST(test_bedside_refusals);
    RUN_TEST(test_event_mode);
    RUN_TEST(test_event_alarm);
    RUN_TEST(test_event_error_and_cmd);
    RUN_TEST(test_too_long_for_mqtt);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
