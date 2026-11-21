/*
 * status_json – the MQTT status documents and the events of the bath,
 * bedroom speaker, bedside and crate nodes, as one-line JSON (pure C, written
 * by hand: no cJSON, no heap).
 *
 * Requirements: SRV-04, BED-50, AUD-13, CRATE-12 ... CRATE-14 (docs/PROTOCOL.md, Status and
 * Events).
 * Tests:        test/test_status_json/
 *
 * Output format, the same for every function here (the tests compare whole
 * strings):
 *   - one line, no spaces outside strings (cJSON_PrintUnformatted style);
 *   - members in exactly the order this header lists them;
 *   - integers as plain digits (no ".0", no exponent), also above 2^31;
 *   - strings JSON-escaped: '"' -> \", '\' -> \\, '\n' -> \n, '\r' -> \r,
 *     '\t' -> \t, '\b' -> \b, '\f' -> \f, other bytes below 0x20 -> \u00XX
 *     (lower-case hex); all other bytes as they are (UTF-8 passes through,
 *     '/' is not escaped);
 *   - a `const char *` given as NULL is written as null, unless the member
 *     is listed as "omitted when NULL".
 * Every function writes the text and its NUL to out and returns the length
 * without the NUL. It returns 0 (out[0] = '\0' if out_size > 0) if an
 * argument it needs is NULL, an enum value is unknown, or the text would not
 * fit out_size or would be longer than STATUS_JSON_MAX.
 *
 * Status document = {"online":true,"node":<node>, common members, node members}
 *   common: fw, built, boot, reset, uptime_s, wifi_rssi, ip, heap_min,
 *           groups (array of strings), settings_version
 */
#ifndef STATUS_JSON_H
#define STATUS_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STATUS_JSON_MAX 4095 /* MQTT buffer 4096 incl. NUL */
#define STATUS_MAX_GROUPS 8
#define STATUS_MAX_TRACK_FOLDERS 8
#define STATUS_MAX_FOLDERS 32

typedef struct {
    const char *fw;          /* "2.0.0"                         */
    const char *built;       /* "2026-11-15T14:02:11Z"          */
    uint32_t boot;           /* boot counter                    */
    const char *reset;       /* "poweron", "panic", ...         */
    uint32_t uptime_s;
    int32_t wifi_rssi;       /* dBm                             */
    const char *ip;          /* "192.168.1.40"                  */
    uint32_t heap_min;
    const char *groups[STATUS_MAX_GROUPS];
    size_t group_count;      /* 0 ... STATUS_MAX_GROUPS, more: refused */
    int32_t settings_version;
} status_common_t;

/* ---- bath --------------------------------------------------------------- */

typedef enum { STATUS_BATH_NORMAL = 0, STATUS_BATH_AMBIENT, STATUS_BATH_COLD } status_bath_mode_t;

typedef struct {
    char name[4];   /* "A" */
    uint16_t count;
} status_tracks_t;

typedef struct {
    status_bath_mode_t mode;     /* "normal" / "ambient" / "cold"              */
    uint8_t volume_ambient;      /* volumes: {"ambient":..,"cold":..}           */
    uint8_t volume_cold;
    const char *hue;             /* "ok", "unreachable", ...                   */
    const char *sd;              /* "ok", "missing", ...                       */
    status_tracks_t tracks[STATUS_MAX_TRACK_FOLDERS];
    size_t track_count;          /* tracks: {"A":29,...} in this order; more than the maximum: refused */
    bool cold_running;           /* false: "cold":null                          */
    const char *cold_phase;      /* cold: {"phase","remaining_s","profile","switch"} */
    uint32_t cold_remaining_s;
    const char *cold_profile;
    const char *cold_switch;
} status_bath_t;

/* {"online":true,"node":"bath",<common>,"mode":..,"volumes":{"ambient":..,
 *  "cold":..},"hue":..,"sd":..,"tracks":{..},"cold":{..} or null} */
size_t status_json_bath(const status_common_t *c, const status_bath_t *b, char *out, size_t out_size);

/* ---- bedroom speaker ------------------------------------------------------ */

typedef enum { STATUS_PLAY_NONE = 0, STATUS_PLAY_ALARM, STATUS_PLAY_MUSIC } status_playing_t;

typedef struct {
    status_playing_t playing;    /* "none" / "alarm" / "music"                 */
    const char *folder;          /* "" when nothing plays                      */
    bool kept;
    uint8_t volume;
    const char *sd;
    const char *folders[STATUS_MAX_FOLDERS];
    size_t folder_count;         /* more than the maximum: refused             */
    const char *armed_state;     /* armed: {"state","version","alarm_utc"}     */
    uint32_t armed_version;
    int64_t armed_alarm_utc;     /* < 0: null                                  */
    bool link_up;                /* link: "up" / "down"                        */
} status_speaker_t;

/* {"online":true,"node":"bedroom_speaker",<common>,"playing":..,"folder":..,
 *  "kept":..,"volume":..,"sd":..,"folders":[..],"armed":{..},"link":..} */
size_t status_json_speaker(const status_common_t *c, const status_speaker_t *sp, char *out, size_t out_size);

/* ---- bedside ------------------------------------------------------------- */

typedef enum { STATUS_TIME_NONE = 0, STATUS_TIME_RTC, STATUS_TIME_SNTP } status_time_source_t;
typedef enum { STATUS_BED_NONE = 0, STATUS_BED_MEDITATION, STATUS_BED_MOOD, STATUS_BED_SLEEP } status_bed_mode_t;

typedef struct {
    const char *time;            /* local ISO "2026-11-16T06:30:12"; NULL -> null */
    status_time_source_t time_source; /* "none" / "rtc" / "sntp"              */
    bool armed_switch;
    bool has_next_alarm;         /* false: "next_alarm":null                    */
    uint8_t next_alarm_slot;     /* next_alarm: {"slot":1,"local":"..."}        */
    const char *next_alarm_local;
    const char *alarm;           /* "idle", "armed", "sunrise", "ringing", "snoozed" */
    uint16_t light;              /* 0 ... 1000                                  */
    status_bed_mode_t mode;      /* "none" / "meditation" / "mood" / "sleep"   */
    bool fading;
    bool candles;
    uint8_t volume_meditation;   /* volumes: {"meditation","mood","sleep"}     */
    uint8_t volume_mood;
    uint8_t volume_sleep;
    int32_t remaining_s;         /* < 0: null                                   */
    bool speaker_up;             /* speaker: "up" / "down"                      */
    const char *game_state;      /* game: {"state","hits","needed"}             */
    uint8_t game_hits;
    uint8_t game_needed;
} status_bedside_t;

/* {"online":true,"node":"bedside",<common>,"time":..,"time_source":..,
 *  "armed_switch":..,"next_alarm":..,"alarm":..,"light":..,"mode":..,
 *  "fading":..,"candles":..,"volumes":{..},"remaining_s":..,"speaker":..,
 *  "game":{..}} */
size_t status_json_bedside(const status_common_t *c, const status_bedside_t *bs, char *out, size_t out_size);

/* ---- crate --------------------------------------------------------------- */

typedef enum { STATUS_CRATE_CLOSED = 0, STATUS_CRATE_OPEN, STATUS_CRATE_DIM, STATUS_CRATE_CLOSING } status_crate_state_t;
typedef enum { STATUS_BATT_OK = 0, STATUS_BATT_LOW, STATUS_BATT_EMPTY } status_battery_t;

typedef struct {
    status_crate_state_t state;  /* "closed" / "open" / "dim" / "closing"      */
    uint16_t light;              /* 0 ... 1000                                  */
    bool playing;
    uint8_t volume;              /* encoder step 0 ... 32                       */
    uint16_t battery_mv;
    status_battery_t battery;    /* "ok" / "low" / "empty"                      */
    const char *sd;
    uint16_t tracks;             /* MP3 files in the card's root                */
} status_crate_t;

/* {"online":true,"node":"crate",<common>,"state":..,"light":..,"playing":..,
 *  "volume":..,"battery_mv":..,"battery":..,"sd":..,"tracks":..} */
size_t status_json_crate(const status_common_t *c, const status_crate_t *cr, char *out, size_t out_size);

/* Published (retained) right before the crate goes to deep sleep, instead of
 * leaving it to the last will:
 * {"online":false,"node":"crate","asleep":true,"battery_mv":..,"battery":..} */
size_t status_json_crate_asleep(uint16_t battery_mv, status_battery_t battery, char *out, size_t out_size);

/* ---- events -------------------------------------------------------------- */
/* t: UTC seconds; t < 0 = time unknown, member omitted. "t" is always the
 * last member. */

/* {"type":"mode","mode":<mode>,"by":<by>[,"continued":true|false]
 *  [,"folder":<folder>][,"t":..]}
 * mode and by must not be NULL. continued: -1 omitted, 0 false, 1 true
 * (other values refused). folder: omitted when NULL. */
size_t status_json_event_mode(const char *mode, const char *by, int continued, const char *folder, int64_t t,
                              char *out, size_t out_size);

/* {"type":"alarm","event":<event>[,"slot":n][,"game":true|false]
 *  [,"reason":<reason>][,"t":..]}
 * event must not be NULL. slot: 0 omitted (slots are 1 ... 6). game: -1
 * omitted, 0/1. reason: omitted when NULL. */
size_t status_json_event_alarm(const char *event, uint8_t slot, int game, const char *reason, int64_t t, char *out,
                               size_t out_size);

/* {"type":"error","where":<where>,"text":<text>[,"t":..]}; both not NULL. */
size_t status_json_event_error(const char *where, const char *text, int64_t t, char *out, size_t out_size);

/* {"type":"cmd","name":<name>,"ok":true|false[,"why":<why>][,"t":..]}
 * name not NULL; why omitted when NULL. */
size_t status_json_event_cmd(const char *name, bool ok, const char *why, int64_t t, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* STATUS_JSON_H */
