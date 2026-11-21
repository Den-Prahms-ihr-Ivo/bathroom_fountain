/*
 * node_config – the v2 setting groups. Contract and key lists in
 * node_config.h, tests in test/test_node_config/.
 */
#include "node_config.h"

#include <stddef.h>

/* ---- value checks ------------------------------------------------------ */

static bool is_lower_or_digit(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

static bool device_id_check(const char *v)
{
    for (; *v; v++) {
        if (!is_lower_or_digit(*v) && *v != '-') {
            return false;
        }
    }
    return true;
}

static bool host_check(const char *v)
{
    for (; *v; v++) {
        const char c = *v;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
              c == '-')) {
            return false;
        }
    }
    return true;
}

static bool no_space_check(const char *v)
{
    for (; *v; v++) {
        if (*v == ' ') {
            return false;
        }
    }
    return true;
}

static bool digits_check(const char *v)
{
    for (; *v; v++) {
        if (*v < '0' || *v > '9') {
            return false;
        }
    }
    return true;
}

/* Characters only; the table's max gives the length (KV_ERR_RANGE). */
static bool folder_chars_check(const char *v)
{
    for (; *v; v++) {
        const char c = *v;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

bool cfg_folder_valid(const char *name)
{
    if (name == NULL || !folder_chars_check(name)) {
        return false;
    }
    size_t n = 0;
    while (name[n] != '\0' && n <= 23) {
        n++;
    }
    return n <= 23;
}

/* ---- net ---------------------------------------------------------------- */

static const kv_key_t net_keys[] = {
    {"device_id", KV_TEXT, KV_FIELD(cfg_net_t, device_id), 1, 24, NULL, NULL, device_id_check},
    {"mqtt_host", KV_TEXT, KV_FIELD(cfg_net_t, mqtt_host), 1, 63, NULL, NULL, host_check},
    {"mqtt_port", KV_UINT, KV_FIELD(cfg_net_t, mqtt_port), 1, 65535, "8883", NULL, NULL},
    {"mqtt_user", KV_TEXT, KV_FIELD(cfg_net_t, mqtt_user), 1, 32, NULL, NULL, no_space_check},
    {"mqtt_pass", KV_SECRET, KV_FIELD(cfg_net_t, mqtt_pass), 8, 64, NULL, NULL, NULL},
    {"web_pin", KV_SECRET, KV_FIELD(cfg_net_t, web_pin), 4, 8, NULL, NULL, digits_check},
    {"log_level", KV_CHOICE, KV_FIELD(cfg_net_t, log_level), 0, 0, "info", "error|warn|info|debug", NULL},
};

const kv_table_t cfg_net_table = {"net", net_keys, sizeof(net_keys) / sizeof(net_keys[0]), sizeof(cfg_net_t)};

/* ---- bath --------------------------------------------------------------- */

#define PROFILE_KEYS(prefix, p, music_def)                                                                   \
    {prefix "_warm_s", KV_UINT, KV_FIELD(cfg_bath_t, profile[p].warm_s), 0, 1800, "120", NULL, NULL},       \
        {prefix "_cold_s", KV_UINT, KV_FIELD(cfg_bath_t, profile[p].cold_s), 15, 600, "90", NULL, NULL},    \
        {prefix "_after_s", KV_UINT, KV_FIELD(cfg_bath_t, profile[p].after_s), 30, 1800, "300", NULL, NULL}, \
        {prefix "_voice", KV_BOOL, KV_FIELD(cfg_bath_t, profile[p].voice), 0, 0, "on", NULL, NULL},          \
        {prefix "_music", KV_BOOL, KV_FIELD(cfg_bath_t, profile[p].music), 0, 0, music_def, NULL, NULL},     \
        {prefix "_calm", KV_BOOL, KV_FIELD(cfg_bath_t, profile[p].calm), 0, 0, "on", NULL, NULL},            \
        {prefix "_scene_warm", KV_UUID, KV_FIELD(cfg_bath_t, profile[p].scene_warm), 0, 0, NULL, NULL, NULL}, \
        {prefix "_scene_cold", KV_UUID, KV_FIELD(cfg_bath_t, profile[p].scene_cold), 0, 0, NULL, NULL, NULL}, \
        {prefix "_scene_after", KV_UUID, KV_FIELD(cfg_bath_t, profile[p].scene_after), 0, 0, NULL, NULL, NULL}

static const kv_key_t bath_keys[] = {
    {"clap_exits", KV_BOOL, KV_FIELD(cfg_bath_t, clap_exits), 0, 0, "off", NULL, NULL},
    {"cold_switch", KV_BOOL, KV_FIELD(cfg_bath_t, cold_switch), 0, 0, "off", NULL, NULL},
    {"cold_profile", KV_CHOICE, KV_FIELD(cfg_bath_t, cold_profile), 0, 0, "hype", "hype|zen", NULL},
    PROFILE_KEYS("hype", CFG_PROFILE_HYPE, "on"),
    PROFILE_KEYS("zen", CFG_PROFILE_ZEN, "off"),
};

const kv_table_t cfg_bath_table = {"bath", bath_keys, sizeof(bath_keys) / sizeof(bath_keys[0]),
                                   sizeof(cfg_bath_t)};

/* ---- bed ---------------------------------------------------------------- */

#define ALARM_KEY(name, i) \
    {name, KV_TEXT, KV_FIELD(cfg_bed_t, alarm[i]), 0, ALARM_TEXT_MAX, NULL, NULL, alarm_text_valid}

static const kv_key_t bed_keys[] = {
    {"tz", KV_TEXT, KV_FIELD(cfg_bed_t, tz), 3, 47, "CET-1CEST,M3.5.0,M10.5.0/3", NULL, no_space_check},
    ALARM_KEY("alarm1", 0),
    ALARM_KEY("alarm2", 1),
    ALARM_KEY("alarm3", 2),
    ALARM_KEY("alarm4", 3),
    ALARM_KEY("alarm5", 4),
    ALARM_KEY("alarm6", 5),
    {"sunrise_min", KV_UINT, KV_FIELD(cfg_bed_t, sunrise_min), 0, 60, "20", NULL, NULL},
    {"sunrise_level", KV_UINT, KV_FIELD(cfg_bed_t, sunrise_level), 1, 100, "80", NULL, NULL},
    {"sound_fade_s", KV_UINT, KV_FIELD(cfg_bed_t, sound_fade_s), 0, 300, "60", NULL, NULL},
    {"alarm_volume", KV_UINT, KV_FIELD(cfg_bed_t, alarm_volume), 1, 32, "16", NULL, NULL},
    {"snooze_min", KV_UINT, KV_FIELD(cfg_bed_t, snooze_min), 1, 30, "9", NULL, NULL},
    {"ring_max_min", KV_UINT, KV_FIELD(cfg_bed_t, ring_max_min), 1, 60, "15", NULL, NULL},
    {"sleep_folder", KV_TEXT, KV_FIELD(cfg_bed_t, sleep_folder), 1, 23, "sleep", NULL, folder_chars_check},
    {"sleep_timer_min", KV_UINT, KV_FIELD(cfg_bed_t, sleep_timer_min), 0, 240, "30", NULL, NULL},
    {"sleep_fade_s", KV_UINT, KV_FIELD(cfg_bed_t, sleep_fade_s), 0, 600, "120", NULL, NULL},
    {"sleep_candle_min", KV_UINT, KV_FIELD(cfg_bed_t, sleep_candle_min), 0, 120, "5", NULL, NULL},
    {"scene_sleep", KV_UUID, KV_FIELD(cfg_bed_t, scene_sleep), 0, 0, NULL, NULL, NULL},
    {"med_folder", KV_TEXT, KV_FIELD(cfg_bed_t, med_folder), 1, 23, "meditation", NULL, folder_chars_check},
    {"med_min", KV_UINT, KV_FIELD(cfg_bed_t, med_min), 1, 180, "20", NULL, NULL},
    {"med_fade_s", KV_UINT, KV_FIELD(cfg_bed_t, med_fade_s), 0, 600, "30", NULL, NULL},
    {"scene_med", KV_UUID, KV_FIELD(cfg_bed_t, scene_med), 0, 0, NULL, NULL, NULL},
    {"mood_folder", KV_TEXT, KV_FIELD(cfg_bed_t, mood_folder), 1, 23, NULL, NULL, folder_chars_check},
    {"scene_mood", KV_UUID, KV_FIELD(cfg_bed_t, scene_mood), 0, 0, NULL, NULL, NULL},
    {"scene_wake", KV_UUID, KV_FIELD(cfg_bed_t, scene_wake), 0, 0, NULL, NULL, NULL},
};

const kv_table_t cfg_bed_table = {"bed", bed_keys, sizeof(bed_keys) / sizeof(bed_keys[0]), sizeof(cfg_bed_t)};

/* ---- game --------------------------------------------------------------- */

static const kv_key_t game_keys[] = {
    {"game_on", KV_BOOL, KV_FIELD(cfg_game_t, game_on), 0, 0, "off", NULL, NULL},
    {"game_preset", KV_CHOICE, KV_FIELD(cfg_game_t, preset), 0, 0, "normal", "easy|normal|hard|custom", NULL},
    {"game_hits", KV_UINT, KV_FIELD(cfg_game_t, hits), 1, 20, "3", NULL, NULL},
    {"game_lit_s", KV_UINT, KV_FIELD(cfg_game_t, lit_s), 2, 60, "8", NULL, NULL},
    {"game_targets", KV_UINT, KV_FIELD(cfg_game_t, targets), 1, 4, "1", NULL, NULL},
    {"game_zone", KV_UINT, KV_FIELD(cfg_game_t, zone), 1, 3, "1", NULL, NULL},
    {"game_ammo", KV_UINT, KV_FIELD(cfg_game_t, ammo), 0, 30, "0", NULL, NULL},
    {"game_penalty", KV_BOOL, KV_FIELD(cfg_game_t, penalty), 0, 0, "off", NULL, NULL},
    {"game_escalate", KV_BOOL, KV_FIELD(cfg_game_t, escalate), 0, 0, "on", NULL, NULL},
    {"game_sights", KV_UINT, KV_FIELD(cfg_game_t, sights), 0, 100, "30", NULL, NULL},
};

const kv_table_t cfg_game_table = {"game", game_keys, sizeof(game_keys) / sizeof(game_keys[0]),
                                   sizeof(cfg_game_t)};
