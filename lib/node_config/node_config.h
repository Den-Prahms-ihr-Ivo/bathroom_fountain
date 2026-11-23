/*
 * node_config – the v2 setting groups: structs and their kv_table tables
 * (pure C, data only).
 *
 * Requirements: CFG-05 ... CFG-08, COLD-14, SRV-02, BED-05, BED-50, GAME-10
 *               (see the requirements doc).
 * Tests:        test/test_node_config/
 *
 * lib/settings keeps the v1 keys (Wi-Fi, Hue bridge, the two v1 scenes, clap
 * and microphone). Every key that v2 adds lives in one of the groups below.
 * Each node's app picks the groups it needs:
 *
 *   node             lib/settings   net   bath   bed   game
 *   bath                  x          x     x
 *   bedroom_speaker       x          x
 *   bedside               x          x            x     x
 *   gun, target       (no settings: the bedside sends them what they need)
 *
 * Key names are unique across lib/settings and all groups, so one flat name
 * space serves the console, NVS, the fallback page and the server.
 *
 * Group "net" (cfg_net_t) – identity and server link
 *   key          type    range / syntax                         default
 *   server_host  text    1-63 of letters, digits, '.', '-'      ""  (the firmware's
 *                        build default is written here at first boot; pairing
 *                        talks HTTPS to it, lib/enroll)
 *   device_id    text    1-24 of a-z 0-9 '-'                    ""  (unset)
 *   mqtt_host    text    1-63 of letters, digits, '.', '-'      ""  (unset)
 *   mqtt_port    uint    1-65535                                8883
 *   mqtt_user    text    1-32, no spaces                        ""  (unset)
 *   mqtt_pass    secret  8-64                                   ""
 *   web_pin      secret  4-8 digits (fallback page)             ""  (page locked)
 *   log_level    choice  error|warn|info|debug (sent to server) info
 *
 * Group "bath" (cfg_bath_t) – ambience and cold shower
 *   key                type    range                            default
 *   clap_exits         bool                                     off
 *   cold_switch        bool    profile comes from the switch    off
 *   cold_profile       choice  hype|zen (used if no switch)     hype
 *   hype_warm_s        uint    0-1800                           120
 *   hype_cold_s        uint    15-600                           90
 *   hype_after_s       uint    30-1800                          300
 *   hype_voice         bool                                     on
 *   hype_music         bool                                     on
 *   hype_calm          bool                                     on
 *   hype_scene_warm    uuid    "" = fall back to scene_ambient  ""
 *   hype_scene_cold    uuid                                     ""
 *   hype_scene_after   uuid                                     ""
 *   zen_warm_s ... zen_scene_after: the same nine keys with the same ranges
 *   and defaults, except zen_music, which defaults to off.
 *
 * Group "bed" (cfg_bed_t) – bedside node
 *   key              type    range                              default
 *   tz               text    3-47, no spaces (POSIX TZ string)  CET-1CEST,M3.5.0,M10.5.0/3
 *   alarm1 ... alarm6 text   alarm_schedule text form           ""  (unused)
 *   sunrise_min      uint    0-60, light ramp before the alarm  20
 *   sunrise_level    uint    1-100 percent                      80
 *   sound_fade_s     uint    0-300, alarm sound fade-in         60
 *   alarm_volume     uint    1-32                               16
 *   snooze_min       uint    1-30                               9
 *   ring_max_min     uint    1-60                               15
 *   display_s        uint    3-120, how long a press of the     10
 *                            big button shows the display
 *   display_sunrise  bool    the display switches on at the     on
 *                            sunrise start (bed_display)
 *   sleep_folder     folder  current sleep folder; the sleep    ""
 *                            button while sleep plays moves it
 *                            to the next "sleep_" folder.
 *                            "" = the first one
 *   sleep_timer_min  uint    0-240, 0 = no timer                30
 *   sleep_fade_s     uint    0-600, fade-out after the timer    120
 *   sleep_candle_min uint    0-120, candles at the night's      5
 *                            first sleep press
 *   scene_sleep      uuid                                       ""
 *   med_folder       folder  meditation music                   meditation
 *   med_min          uint    1-180, meditation length           20
 *   med_fade_s       uint    0-600, music out / normal scene in 30
 *   scene_med        uuid                                       ""
 *   mood_folder      folder  current mood folder; the mood      ""
 *                            button moves it to the next
 *                            "mood_" folder. "" = the first one
 *   scene_mood       uuid                                       ""
 *   scene_wake       uuid                                       ""
 *   The meditation timer's end recalls scene_normal (lib/settings).
 *   folder = text, 0-23 characters of A-Z a-z 0-9 '_' '-' (cfg_folder_valid);
 *   "" means "no music" for med_folder.
 *
 * Group "game" (cfg_game_t) – alarm game, on the bedside node
 *   key            type    range                                default
 *   game_on        bool    alarms need the game                 off
 *   game_preset    choice  easy|normal|hard|custom              normal
 *   game_hits      uint    1-20                                 3
 *   game_lit_s     uint    2-60                                 8
 *   game_targets   uint    1-4, lit at once                     1
 *   game_zone      uint    1-3, 1 = whole disc, 3 = centre      1
 *   game_ammo      uint    0-30, 0 = no magazine                0
 *   game_penalty   bool    a miss adds a hit                    off
 *   game_escalate  bool    alarm gets louder                    on
 *   game_sights    uint    0-100 percent                        30
 *
 * The keys appear in each table in the order listed here. Every table passes
 * kv_table_valid().
 */
#ifndef NODE_CONFIG_H
#define NODE_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#include "alarm_schedule.h"
#include "kv_table.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CFG_LOG_ERROR = 0,
    CFG_LOG_WARN,
    CFG_LOG_INFO,
    CFG_LOG_DEBUG,
} cfg_log_level_t;

typedef struct {
    char server_host[64];
    char device_id[25];
    char mqtt_host[64];
    uint16_t mqtt_port;
    char mqtt_user[33];
    char mqtt_pass[65];
    char web_pin[9];
    uint8_t log_level; /* cfg_log_level_t */
} cfg_net_t;

typedef enum {
    CFG_PROFILE_HYPE = 0,
    CFG_PROFILE_ZEN,
    CFG_PROFILE_COUNT,
} cfg_profile_t;

typedef struct {
    uint16_t warm_s;
    uint16_t cold_s;
    uint16_t after_s;
    bool voice;
    bool music;
    bool calm;
    char scene_warm[37];
    char scene_cold[37];
    char scene_after[37];
} cfg_cold_profile_t;

typedef struct {
    bool clap_exits;
    bool cold_switch;
    uint8_t cold_profile;                       /* cfg_profile_t */
    cfg_cold_profile_t profile[CFG_PROFILE_COUNT]; /* [CFG_PROFILE_HYPE], [CFG_PROFILE_ZEN] */
} cfg_bath_t;

typedef struct {
    char tz[48];
    char alarm[ALARM_SLOTS][ALARM_TEXT_MAX + 1];
    uint8_t sunrise_min;
    uint8_t sunrise_level;
    uint16_t sound_fade_s;
    uint8_t alarm_volume;
    uint8_t snooze_min;
    uint8_t ring_max_min;
    uint8_t display_s;
    bool display_sunrise;
    char sleep_folder[24];
    uint8_t sleep_timer_min;
    uint16_t sleep_fade_s;
    uint8_t sleep_candle_min;
    char scene_sleep[37];
    char med_folder[24];
    uint8_t med_min;
    uint16_t med_fade_s;
    char scene_med[37];
    char mood_folder[24];
    char scene_mood[37];
    char scene_wake[37];
} cfg_bed_t;

typedef enum {
    CFG_GAME_EASY = 0,
    CFG_GAME_NORMAL,
    CFG_GAME_HARD,
    CFG_GAME_CUSTOM,
} cfg_game_preset_t;

typedef struct {
    bool game_on;
    uint8_t preset; /* cfg_game_preset_t */
    uint8_t hits;
    uint8_t lit_s;
    uint8_t targets;
    uint8_t zone;
    uint8_t ammo;
    bool penalty;
    bool escalate;
    uint8_t sights;
} cfg_game_t;

/* Folder names on the speaker's SD card that the settings and the link can
 * carry: 1 ... LINK_FOLDER_LEN (23) characters of A-Z a-z 0-9 '_' '-'.
 * "" is accepted here too (the settings use it for "none"). NULL is false. */
bool cfg_folder_valid(const char *name);

extern const kv_table_t cfg_net_table;
extern const kv_table_t cfg_bath_table;
extern const kv_table_t cfg_bed_table;
extern const kv_table_t cfg_game_table;

#ifdef __cplusplus
}
#endif

#endif /* NODE_CONFIG_H */
