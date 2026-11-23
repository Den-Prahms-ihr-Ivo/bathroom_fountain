/*
 * Tests for lib/node_config – requirements CFG-05 ... CFG-08, COLD-14,
 * SRV-02, BED-05, GAME-10. They need working lib/kv_table and
 * lib/alarm_schedule.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unity.h>

#include "node_config.h"
#include "settings.h"

#define UUID_A "3f2a9c1e-5b7d-4e8a-9c21-7d4e5f6a8b90"

/* Each struct is followed by a zero byte, so a string that was never written
 * still ends inside the box (the stub must fail, not crash). */
static struct {
    cfg_net_t v;
    char end;
} net_box;
static struct {
    cfg_bath_t v;
    char end;
} bath_box;
static struct {
    cfg_bed_t v;
    char end;
} bed_box;
static struct {
    cfg_game_t v;
    char end;
} game_box;
#define net (net_box.v)
#define bath (bath_box.v)
#define bed (bed_box.v)
#define game (game_box.v)

void setUp(void)
{
    memset(&net, 0x5A, sizeof(net));
    memset(&bath, 0x5A, sizeof(bath));
    memset(&bed, 0x5A, sizeof(bed));
    memset(&game, 0x5A, sizeof(game));
    net_box.end = bath_box.end = bed_box.end = game_box.end = '\0';
    kv_defaults(&cfg_net_table, &net);
    kv_defaults(&cfg_bath_table, &bath);
    kv_defaults(&cfg_bed_table, &bed);
    kv_defaults(&cfg_game_table, &game);
}

void tearDown(void) {}

static void expect_keys(const kv_table_t *t, const char *group, const char *const *names, size_t n)
{
    TEST_ASSERT_TRUE_MESSAGE(kv_table_valid(t), group);
    TEST_ASSERT_EQUAL_STRING(group, t->name);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(n, t->count, group);
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_STRING(names[i], t->keys[i].name);
    }
}

#define OK(table, data, key, value) TEST_ASSERT_EQUAL_INT_MESSAGE(KV_OK, kv_set(&(table), &(data), key, value), key " = " value)
#define FAILS(table, data, key, value, err) \
    TEST_ASSERT_EQUAL_INT_MESSAGE(err, kv_set(&(table), &(data), key, value), key " = " value)

static void test_key_lists(void)
{
    static const char *const net_keys[] = {"server_host", "device_id", "mqtt_host", "mqtt_port", "mqtt_user",
                                           "mqtt_pass", "web_pin",   "log_level"};
    static const char *const bath_keys[] = {
        "clap_exits",      "cold_switch",     "cold_profile",     "hype_warm_s",    "hype_cold_s",   "hype_after_s",
        "hype_voice",      "hype_music",      "hype_calm",        "hype_scene_warm", "hype_scene_cold", "hype_scene_after",
        "zen_warm_s",      "zen_cold_s",      "zen_after_s",      "zen_voice",      "zen_music",     "zen_calm",
        "zen_scene_warm",  "zen_scene_cold",  "zen_scene_after"};
    static const char *const bed_keys[] = {"tz",           "alarm1",       "alarm2",          "alarm3",       "alarm4",
                                           "alarm5",       "alarm6",       "sunrise_min",     "sunrise_level", "sound_fade_s",
                                           "alarm_volume", "snooze_min",   "ring_max_min",    "display_s",
                                           "display_sunrise", "sleep_folder",
                                           "sleep_timer_min", "sleep_fade_s", "sleep_candle_min", "scene_sleep",
                                           "med_folder",   "med_min",      "med_fade_s",      "scene_med",
                                           "mood_folder",  "scene_mood",   "scene_wake"};
    static const char *const game_keys[] = {"game_on",   "game_preset", "game_hits",    "game_lit_s",    "game_targets",
                                            "game_zone", "game_ammo",   "game_penalty", "game_escalate", "game_sights"};
    expect_keys(&cfg_net_table, "net", net_keys, sizeof(net_keys) / sizeof(net_keys[0]));
    expect_keys(&cfg_bath_table, "bath", bath_keys, sizeof(bath_keys) / sizeof(bath_keys[0]));
    expect_keys(&cfg_bed_table, "bed", bed_keys, sizeof(bed_keys) / sizeof(bed_keys[0]));
    expect_keys(&cfg_game_table, "game", game_keys, sizeof(game_keys) / sizeof(game_keys[0]));
    TEST_ASSERT_EQUAL_size_t(sizeof(cfg_net_t), cfg_net_table.data_size);
    TEST_ASSERT_EQUAL_size_t(sizeof(cfg_bath_t), cfg_bath_table.data_size);
    TEST_ASSERT_EQUAL_size_t(sizeof(cfg_bed_t), cfg_bed_table.data_size);
    TEST_ASSERT_EQUAL_size_t(sizeof(cfg_game_t), cfg_game_table.data_size);
}

static void test_names_are_unique_across_all_groups_and_settings(void)
{
    const kv_table_t *tables[] = {&cfg_net_table, &cfg_bath_table, &cfg_bed_table, &cfg_game_table};
    TEST_ASSERT_TRUE(cfg_net_table.count > 0);
    for (size_t a = 0; a < 4; a++) {
        for (size_t i = 0; i < tables[a]->count; i++) {
            const char *name = tables[a]->keys[i].name;
            for (size_t b = a + 1; b < 4; b++) {
                TEST_ASSERT_EQUAL_INT_MESSAGE(-1, kv_find(tables[b], name), name);
            }
            for (size_t k = 0; k < settings_key_count(); k++) {
                TEST_ASSERT_TRUE_MESSAGE(strcmp(settings_key_name(k), name) != 0, name);
            }
        }
    }
}

static void test_net_defaults_and_rules(void)
{
    TEST_ASSERT_EQUAL_STRING("", net.server_host);
    TEST_ASSERT_EQUAL_STRING("", net.device_id);
    TEST_ASSERT_EQUAL_STRING("", net.mqtt_host);
    TEST_ASSERT_EQUAL_UINT16(8883, net.mqtt_port);
    TEST_ASSERT_EQUAL_STRING("", net.mqtt_user);
    TEST_ASSERT_EQUAL_STRING("", net.mqtt_pass);
    TEST_ASSERT_EQUAL_STRING("", net.web_pin);
    TEST_ASSERT_EQUAL_UINT8(CFG_LOG_INFO, net.log_level);

    OK(cfg_net_table, net, "device_id", "bath-1");
    OK(cfg_net_table, net, "device_id", "abcdefghijklmnopqrstuvwx");
    FAILS(cfg_net_table, net, "device_id", "abcdefghijklmnopqrstuvwxy", KV_ERR_RANGE);
    FAILS(cfg_net_table, net, "device_id", "Bath", KV_ERR_INVALID);
    FAILS(cfg_net_table, net, "device_id", "bath/1", KV_ERR_INVALID); /* would break the topic */
    FAILS(cfg_net_table, net, "device_id", "bath+", KV_ERR_INVALID);
    FAILS(cfg_net_table, net, "device_id", "bath_1", KV_ERR_INVALID);
    TEST_ASSERT_EQUAL_STRING("abcdefghijklmnopqrstuvwx", net.device_id);

    OK(cfg_net_table, net, "mqtt_host", "Home.Example.org");
    OK(cfg_net_table, net, "mqtt_host", "192.168.1.20");
    FAILS(cfg_net_table, net, "mqtt_host", "mqtts://example.org", KV_ERR_INVALID);
    FAILS(cfg_net_table, net, "mqtt_host", "example.org:8883", KV_ERR_INVALID);
    OK(cfg_net_table, net, "mqtt_port", "1");
    OK(cfg_net_table, net, "server_host", "ambience.example.org");
    FAILS(cfg_net_table, net, "server_host", "https://ambience.example.org", KV_ERR_INVALID);
    OK(cfg_net_table, net, "mqtt_port", "65535");
    FAILS(cfg_net_table, net, "mqtt_port", "0", KV_ERR_RANGE);
    FAILS(cfg_net_table, net, "mqtt_port", "65536", KV_ERR_RANGE);
    OK(cfg_net_table, net, "mqtt_user", "bath-1");
    FAILS(cfg_net_table, net, "mqtt_user", "bath 1", KV_ERR_INVALID);
    OK(cfg_net_table, net, "mqtt_pass", "eight ch");
    FAILS(cfg_net_table, net, "mqtt_pass", "seven c", KV_ERR_RANGE);
    OK(cfg_net_table, net, "web_pin", "1234");
    OK(cfg_net_table, net, "web_pin", "12345678");
    FAILS(cfg_net_table, net, "web_pin", "123", KV_ERR_RANGE);
    FAILS(cfg_net_table, net, "web_pin", "123456789", KV_ERR_RANGE);
    FAILS(cfg_net_table, net, "web_pin", "12a4", KV_ERR_INVALID);
    OK(cfg_net_table, net, "web_pin", "");
    OK(cfg_net_table, net, "log_level", "debug");
    TEST_ASSERT_EQUAL_UINT8(CFG_LOG_DEBUG, net.log_level);
    OK(cfg_net_table, net, "log_level", "error");
    TEST_ASSERT_EQUAL_UINT8(CFG_LOG_ERROR, net.log_level);
    FAILS(cfg_net_table, net, "log_level", "verbose", KV_ERR_INVALID);

    char out[16];
    OK(cfg_net_table, net, "mqtt_pass", "top secret");
    TEST_ASSERT_TRUE(kv_format(&cfg_net_table, &net, "mqtt_pass", false, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("********", out);
    OK(cfg_net_table, net, "web_pin", "4711");
    TEST_ASSERT_TRUE(kv_format(&cfg_net_table, &net, "web_pin", false, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("********", out);
}

static void test_bath_defaults(void)
{
    TEST_ASSERT_FALSE(bath.clap_exits);
    TEST_ASSERT_FALSE(bath.cold_switch);
    TEST_ASSERT_EQUAL_UINT8(CFG_PROFILE_HYPE, bath.cold_profile);
    for (int p = 0; p < CFG_PROFILE_COUNT; p++) {
        TEST_ASSERT_EQUAL_UINT16(120, bath.profile[p].warm_s);
        TEST_ASSERT_EQUAL_UINT16(90, bath.profile[p].cold_s);
        TEST_ASSERT_EQUAL_UINT16(300, bath.profile[p].after_s);
        TEST_ASSERT_TRUE(bath.profile[p].voice);
        TEST_ASSERT_TRUE(bath.profile[p].calm);
        TEST_ASSERT_EQUAL_STRING("", bath.profile[p].scene_warm);
        TEST_ASSERT_EQUAL_STRING("", bath.profile[p].scene_cold);
        TEST_ASSERT_EQUAL_STRING("", bath.profile[p].scene_after);
    }
    TEST_ASSERT_TRUE(bath.profile[CFG_PROFILE_HYPE].music);
    TEST_ASSERT_FALSE_MESSAGE(bath.profile[CFG_PROFILE_ZEN].music, "zen has no hype music by default");
}

static void test_bath_profiles_are_separate(void)
{
    OK(cfg_bath_table, bath, "hype_cold_s", "600");
    OK(cfg_bath_table, bath, "zen_cold_s", "15");
    TEST_ASSERT_EQUAL_UINT16(600, bath.profile[CFG_PROFILE_HYPE].cold_s);
    TEST_ASSERT_EQUAL_UINT16(15, bath.profile[CFG_PROFILE_ZEN].cold_s);
    FAILS(cfg_bath_table, bath, "hype_cold_s", "14", KV_ERR_RANGE);
    FAILS(cfg_bath_table, bath, "zen_cold_s", "601", KV_ERR_RANGE);
    OK(cfg_bath_table, bath, "zen_warm_s", "0");
    OK(cfg_bath_table, bath, "hype_warm_s", "1800");
    FAILS(cfg_bath_table, bath, "hype_warm_s", "1801", KV_ERR_RANGE);
    OK(cfg_bath_table, bath, "zen_after_s", "30");
    FAILS(cfg_bath_table, bath, "zen_after_s", "29", KV_ERR_RANGE);
    FAILS(cfg_bath_table, bath, "hype_after_s", "1801", KV_ERR_RANGE);
    OK(cfg_bath_table, bath, "zen_voice", "off");
    TEST_ASSERT_FALSE(bath.profile[CFG_PROFILE_ZEN].voice);
    TEST_ASSERT_TRUE(bath.profile[CFG_PROFILE_HYPE].voice);
    OK(cfg_bath_table, bath, "hype_calm", "off");
    TEST_ASSERT_FALSE(bath.profile[CFG_PROFILE_HYPE].calm);
    TEST_ASSERT_TRUE(bath.profile[CFG_PROFILE_ZEN].calm);
    OK(cfg_bath_table, bath, "zen_scene_cold", UUID_A);
    TEST_ASSERT_EQUAL_STRING(UUID_A, bath.profile[CFG_PROFILE_ZEN].scene_cold);
    TEST_ASSERT_EQUAL_STRING("", bath.profile[CFG_PROFILE_HYPE].scene_cold);
    OK(cfg_bath_table, bath, "zen_scene_cold", ""); /* back to the fallback */
    FAILS(cfg_bath_table, bath, "hype_scene_after", "scene 3", KV_ERR_INVALID);
    OK(cfg_bath_table, bath, "cold_profile", "zen");
    TEST_ASSERT_EQUAL_UINT8(CFG_PROFILE_ZEN, bath.cold_profile);
    OK(cfg_bath_table, bath, "cold_switch", "on");
    OK(cfg_bath_table, bath, "clap_exits", "on");
    TEST_ASSERT_TRUE(bath.cold_switch);
    TEST_ASSERT_TRUE(bath.clap_exits);
}

static void test_bed_defaults_and_rules(void)
{
    TEST_ASSERT_EQUAL_STRING("CET-1CEST,M3.5.0,M10.5.0/3", bed.tz);
    for (int i = 0; i < ALARM_SLOTS; i++) {
        TEST_ASSERT_EQUAL_STRING("", bed.alarm[i]);
    }
    TEST_ASSERT_EQUAL_UINT8(20, bed.sunrise_min);
    TEST_ASSERT_EQUAL_UINT8(80, bed.sunrise_level);
    TEST_ASSERT_EQUAL_UINT16(60, bed.sound_fade_s);
    TEST_ASSERT_EQUAL_UINT8(16, bed.alarm_volume);
    TEST_ASSERT_EQUAL_UINT8(9, bed.snooze_min);
    TEST_ASSERT_EQUAL_UINT8(15, bed.ring_max_min);
    TEST_ASSERT_EQUAL_UINT8(10, bed.display_s);
    TEST_ASSERT_TRUE(bed.display_sunrise);
    TEST_ASSERT_EQUAL_STRING("", bed.sleep_folder);
    TEST_ASSERT_EQUAL_UINT8(30, bed.sleep_timer_min);
    TEST_ASSERT_EQUAL_UINT16(120, bed.sleep_fade_s);
    TEST_ASSERT_EQUAL_UINT8(5, bed.sleep_candle_min);
    TEST_ASSERT_EQUAL_STRING("", bed.scene_sleep);
    TEST_ASSERT_EQUAL_STRING("meditation", bed.med_folder);
    TEST_ASSERT_EQUAL_UINT8(20, bed.med_min);
    TEST_ASSERT_EQUAL_UINT16(30, bed.med_fade_s);
    TEST_ASSERT_EQUAL_STRING("", bed.scene_med);
    TEST_ASSERT_EQUAL_STRING("", bed.mood_folder);
    TEST_ASSERT_EQUAL_STRING("", bed.scene_mood);
    TEST_ASSERT_EQUAL_STRING("", bed.scene_wake);

    OK(cfg_bed_table, bed, "alarm1", "06:45,mtwtf--,on");
    OK(cfg_bed_table, bed, "alarm6", "09:30,-----ss,off");
    TEST_ASSERT_EQUAL_STRING("06:45,mtwtf--,on", bed.alarm[0]);
    TEST_ASSERT_EQUAL_STRING("09:30,-----ss,off", bed.alarm[5]);
    TEST_ASSERT_EQUAL_STRING("", bed.alarm[1]);
    FAILS(cfg_bed_table, bed, "alarm2", "6:45,mtwtf--,on", KV_ERR_INVALID);
    FAILS(cfg_bed_table, bed, "alarm2", "25:00,mtwtf--,on", KV_ERR_INVALID);
    FAILS(cfg_bed_table, bed, "alarm7", "06:45,mtwtf--,on", KV_ERR_UNKNOWN_KEY);
    OK(cfg_bed_table, bed, "alarm1", ""); /* slot unused again */
    OK(cfg_bed_table, bed, "tz", "UTC0");
    FAILS(cfg_bed_table, bed, "tz", "Europe Berlin", KV_ERR_INVALID);
    FAILS(cfg_bed_table, bed, "tz", "UT", KV_ERR_RANGE);
    OK(cfg_bed_table, bed, "sunrise_min", "0");
    FAILS(cfg_bed_table, bed, "sunrise_min", "61", KV_ERR_RANGE);
    FAILS(cfg_bed_table, bed, "sunrise_level", "0", KV_ERR_RANGE);
    FAILS(cfg_bed_table, bed, "sunrise_level", "101", KV_ERR_RANGE);
    FAILS(cfg_bed_table, bed, "sound_fade_s", "301", KV_ERR_RANGE);
    FAILS(cfg_bed_table, bed, "alarm_volume", "0", KV_ERR_RANGE);
    FAILS(cfg_bed_table, bed, "alarm_volume", "33", KV_ERR_RANGE);
    FAILS(cfg_bed_table, bed, "snooze_min", "0", KV_ERR_RANGE);
    FAILS(cfg_bed_table, bed, "snooze_min", "31", KV_ERR_RANGE);
    FAILS(cfg_bed_table, bed, "ring_max_min", "0", KV_ERR_RANGE);
    FAILS(cfg_bed_table, bed, "ring_max_min", "61", KV_ERR_RANGE);
    FAILS(cfg_bed_table, bed, "display_s", "2", KV_ERR_RANGE);
    FAILS(cfg_bed_table, bed, "display_s", "121", KV_ERR_RANGE);
    OK(cfg_bed_table, bed, "sleep_timer_min", "0");
    FAILS(cfg_bed_table, bed, "sleep_timer_min", "241", KV_ERR_RANGE);
    OK(cfg_bed_table, bed, "sleep_fade_s", "600");
    TEST_ASSERT_EQUAL_UINT16(600, bed.sleep_fade_s);
    FAILS(cfg_bed_table, bed, "sleep_fade_s", "601", KV_ERR_RANGE);
    OK(cfg_bed_table, bed, "sleep_candle_min", "120");
    FAILS(cfg_bed_table, bed, "sleep_candle_min", "121", KV_ERR_RANGE);
    OK(cfg_bed_table, bed, "med_min", "180");
    FAILS(cfg_bed_table, bed, "med_min", "0", KV_ERR_RANGE);
    FAILS(cfg_bed_table, bed, "med_min", "181", KV_ERR_RANGE);
    OK(cfg_bed_table, bed, "med_fade_s", "0");
    FAILS(cfg_bed_table, bed, "med_fade_s", "601", KV_ERR_RANGE);
    OK(cfg_bed_table, bed, "scene_wake", UUID_A);
    OK(cfg_bed_table, bed, "scene_sleep", UUID_A);
    OK(cfg_bed_table, bed, "scene_med", UUID_A);
    FAILS(cfg_bed_table, bed, "scene_med", "scene 3", KV_ERR_INVALID);

    /* folders */
    OK(cfg_bed_table, bed, "mood_folder", "mood_Rain-2");
    TEST_ASSERT_EQUAL_STRING("mood_Rain-2", bed.mood_folder);
    OK(cfg_bed_table, bed, "sleep_folder", "sleep_true_crime");
    OK(cfg_bed_table, bed, "med_folder", "abcdefghijklmnopqrstuvw"); /* 23 */
    FAILS(cfg_bed_table, bed, "med_folder", "abcdefghijklmnopqrstuvwx", KV_ERR_RANGE);
    FAILS(cfg_bed_table, bed, "sleep_folder", "true crime", KV_ERR_INVALID);
    FAILS(cfg_bed_table, bed, "sleep_folder", "a/b", KV_ERR_INVALID);
    FAILS(cfg_bed_table, bed, "mood_folder", "mood.rain", KV_ERR_INVALID);
    OK(cfg_bed_table, bed, "sleep_folder", ""); /* the first sleep_ folder */
    OK(cfg_bed_table, bed, "mood_folder", "");
}

static void test_folder_names(void)
{
    TEST_ASSERT_TRUE(cfg_folder_valid("sleep"));
    TEST_ASSERT_TRUE(cfg_folder_valid("Mood_Jazz-1"));
    TEST_ASSERT_TRUE(cfg_folder_valid(""));
    TEST_ASSERT_TRUE(cfg_folder_valid("abcdefghijklmnopqrstuvw"));
    TEST_ASSERT_FALSE(cfg_folder_valid("abcdefghijklmnopqrstuvwx"));
    TEST_ASSERT_FALSE(cfg_folder_valid("with space"));
    TEST_ASSERT_FALSE(cfg_folder_valid("dot.ted"));
    TEST_ASSERT_FALSE(cfg_folder_valid("sl/ash"));
    TEST_ASSERT_FALSE(cfg_folder_valid("umlaut\xc3\xa4"));
    TEST_ASSERT_FALSE(cfg_folder_valid(NULL));
}

static void test_game_defaults_and_rules(void)
{
    TEST_ASSERT_FALSE(game.game_on);
    TEST_ASSERT_EQUAL_UINT8(CFG_GAME_NORMAL, game.preset);
    TEST_ASSERT_EQUAL_UINT8(3, game.hits);
    TEST_ASSERT_EQUAL_UINT8(8, game.lit_s);
    TEST_ASSERT_EQUAL_UINT8(1, game.targets);
    TEST_ASSERT_EQUAL_UINT8(1, game.zone);
    TEST_ASSERT_EQUAL_UINT8(0, game.ammo);
    TEST_ASSERT_FALSE(game.penalty);
    TEST_ASSERT_TRUE(game.escalate);
    TEST_ASSERT_EQUAL_UINT8(30, game.sights);

    OK(cfg_game_table, game, "game_preset", "custom");
    TEST_ASSERT_EQUAL_UINT8(CFG_GAME_CUSTOM, game.preset);
    OK(cfg_game_table, game, "game_preset", "easy");
    TEST_ASSERT_EQUAL_UINT8(CFG_GAME_EASY, game.preset);
    OK(cfg_game_table, game, "game_preset", "hard");
    TEST_ASSERT_EQUAL_UINT8(CFG_GAME_HARD, game.preset);
    FAILS(cfg_game_table, game, "game_preset", "insane", KV_ERR_INVALID);
    FAILS(cfg_game_table, game, "game_hits", "0", KV_ERR_RANGE);
    FAILS(cfg_game_table, game, "game_hits", "21", KV_ERR_RANGE);
    FAILS(cfg_game_table, game, "game_lit_s", "1", KV_ERR_RANGE);
    FAILS(cfg_game_table, game, "game_lit_s", "61", KV_ERR_RANGE);
    FAILS(cfg_game_table, game, "game_targets", "0", KV_ERR_RANGE);
    FAILS(cfg_game_table, game, "game_targets", "5", KV_ERR_RANGE);
    FAILS(cfg_game_table, game, "game_zone", "0", KV_ERR_RANGE);
    FAILS(cfg_game_table, game, "game_zone", "4", KV_ERR_RANGE);
    OK(cfg_game_table, game, "game_ammo", "30");
    FAILS(cfg_game_table, game, "game_ammo", "31", KV_ERR_RANGE);
    OK(cfg_game_table, game, "game_sights", "100");
    FAILS(cfg_game_table, game, "game_sights", "101", KV_ERR_RANGE);
    OK(cfg_game_table, game, "game_on", "on");
    OK(cfg_game_table, game, "game_penalty", "on");
    OK(cfg_game_table, game, "game_escalate", "off");
    TEST_ASSERT_TRUE(game.game_on);
    TEST_ASSERT_TRUE(game.penalty);
    TEST_ASSERT_FALSE(game.escalate);
    TEST_ASSERT_EQUAL_UINT8(30, game.ammo);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_key_lists);
    RUN_TEST(test_names_are_unique_across_all_groups_and_settings);
    RUN_TEST(test_net_defaults_and_rules);
    RUN_TEST(test_bath_defaults);
    RUN_TEST(test_bath_profiles_are_separate);
    RUN_TEST(test_bed_defaults_and_rules);
    RUN_TEST(test_folder_names);
    RUN_TEST(test_game_defaults_and_rules);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
