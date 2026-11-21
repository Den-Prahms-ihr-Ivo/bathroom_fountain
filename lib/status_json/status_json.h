/*
 * status_json – the status documents of bath, speaker and bedside, and the
 * mode / alarm / error / cmd events, as JSON text (pure C, no MQTT).
 *
 * Spec:  docs/PROTOCOL.md, "Status" and "Events".
 * Tests: test/test_status_json/
 *
 * Output is compact JSON (no spaces) with the members in the order the
 * protocol lists them. Strings are escaped: '"' and '\\' with a backslash,
 * \b \f \n \r \t as such, other bytes below 0x20 as \u00XX; bytes from 0x80
 * up are copied as they are (UTF-8 passes through). A NULL string member is
 * written as null.
 *
 * Every function returns the text length (without the NUL), or 0 if an
 * argument is NULL, a value is invalid (listed per function), the text would
 * be longer than STATUS_JSON_MAX (the device's MQTT buffer) or does not fit
 * out_size including the NUL. On 0, out[0] is '\0' when out_size > 0
 * (nothing is written for out_size 0).
 *
 * Common members (status_common_t): online (always true), node, fw, built,
 * boot, reset, uptime_s, wifi_rssi, ip, heap_min, groups, settings_version.
 * Invalid: group_count > STATUS_MAX_GROUPS.
 *
 * Optional numbers: a negative value means "absent" (written as null in a
 * status, left out of an event). Optional event members: NULL strings and
 * tri-state ints of -1 are left out; a tri-state other than -1, 0, 1 is
 * invalid.
 */
#ifndef STATUS_JSON_H
#define STATUS_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STATUS_JSON_MAX 4096 /* longest payload the device's MQTT buffer takes */
#define STATUS_MAX_GROUPS 8
#define STATUS_MAX_TRACK_FOLDERS 8
#define STATUS_TRACK_NAME_MAX 23
#define STATUS_MAX_FOLDERS 32

typedef struct {
    const char *fw;
    const char *built;
    uint32_t boot;
    const char *reset;
    uint32_t uptime_s;
    int32_t wifi_rssi;
    const char *ip;
    uint32_t heap_min;
    const char *groups[STATUS_MAX_GROUPS];
    size_t group_count;
    int32_t settings_version;
} status_common_t;

/* ---- bath ---------------------------------------------------------------- */

typedef enum {
    STATUS_BATH_NORMAL = 0,
    STATUS_BATH_AMBIENT,
    STATUS_BATH_COLD,
} status_bath_mode_t;

typedef struct {
    char name[STATUS_TRACK_NAME_MAX + 1];
    uint16_t count;
} status_track_folder_t;

/* cold: written only while cold_running (else null).
 * Invalid: an unknown mode, track_count > STATUS_MAX_TRACK_FOLDERS. */
typedef struct {
    status_bath_mode_t mode;
    uint8_t volume_ambient;
    uint8_t volume_cold;
    const char *hue;
    const char *sd;
    status_track_folder_t tracks[STATUS_MAX_TRACK_FOLDERS];
    size_t track_count;
    bool cold_running;
    const char *cold_phase;
    uint32_t cold_remaining_s;
    const char *cold_profile;
    const char *cold_switch;
} status_bath_t;

size_t status_json_bath(const status_common_t *c, const status_bath_t *b, char *out, size_t out_size);

/* ---- bedroom speaker ----------------------------------------------------- */

typedef enum {
    STATUS_PLAY_NONE = 0,
    STATUS_PLAY_ALARM,
    STATUS_PLAY_MUSIC,
} status_playing_t;

/* armed_alarm_utc < 0: null. link: "up" / "down".
 * Invalid: an unknown playing value, folder_count > STATUS_MAX_FOLDERS. */
typedef struct {
    status_playing_t playing;
    const char *folder;
    bool kept;
    uint8_t volume;
    const char *sd;
    const char *folders[STATUS_MAX_FOLDERS];
    size_t folder_count;
    const char *armed_state;
    uint32_t armed_version;
    int64_t armed_alarm_utc;
    bool link_up;
} status_speaker_t;

size_t status_json_speaker(const status_common_t *c, const status_speaker_t *s, char *out, size_t out_size);

/* ---- bedside ------------------------------------------------------------- */

typedef enum {
    STATUS_TIME_NONE = 0,
    STATUS_TIME_SNTP,
    STATUS_TIME_RTC,
} status_time_source_t;

typedef enum {
    STATUS_BED_NONE = 0,
    STATUS_BED_MEDITATION,
    STATUS_BED_MOOD,
    STATUS_BED_SLEEP,
} status_bed_mode_t;

/* next_alarm: {"slot":..,"local":..} if has_next_alarm, else null.
 * remaining_s < 0: null. speaker: "up" / "down".
 * Invalid: an unknown time_source or mode. */
typedef struct {
    const char *time;
    status_time_source_t time_source;
    bool armed_switch;
    bool has_next_alarm;
    uint8_t next_alarm_slot;
    const char *next_alarm_local;
    const char *alarm;
    uint16_t light;
    status_bed_mode_t mode;
    bool fading;
    bool candles;
    uint8_t volume_meditation;
    uint8_t volume_mood;
    uint8_t volume_sleep;
    int32_t remaining_s;
    bool speaker_up;
    const char *game_state;
    uint8_t game_hits;
    uint8_t game_needed;
} status_bedside_t;

size_t status_json_bedside(const status_common_t *c, const status_bedside_t *b, char *out, size_t out_size);

/* ---- events -------------------------------------------------------------- */
/* t: UTC seconds, < 0 = unknown (left out). */

/* {"type":"mode","mode":..,"by":..[,"continued":bool][,"folder":..][,"t":..]}
 * mode and by required; continued -1 / 0 / 1. */
size_t status_json_event_mode(const char *mode, const char *by, int continued, const char *folder, int64_t t,
                              char *out, size_t out_size);

/* {"type":"alarm","event":..[,"slot":n][,"game":bool][,"reason":..][,"t":..]}
 * event required; slot <= 0 left out; game -1 / 0 / 1. */
size_t status_json_event_alarm(const char *event, int slot, int game, const char *reason, int64_t t, char *out,
                               size_t out_size);

/* {"type":"error","where":..,"text":..[,"t":..]}  where and text required. */
size_t status_json_event_error(const char *where, const char *text, int64_t t, char *out, size_t out_size);

/* {"type":"cmd","name":..,"ok":bool[,"why":..][,"t":..]}  name required. */
size_t status_json_event_cmd(const char *name, bool ok, const char *why, int64_t t, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* STATUS_JSON_H */
