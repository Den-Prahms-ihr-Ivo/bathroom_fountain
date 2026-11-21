/*
 * status_json – status documents and events as JSON text.
 * Contract in status_json.h, tests in test/test_status_json/.
 *
 * Written by hand rather than with cJSON: the output is pinned byte for byte
 * (member order, escaping, length limit) and needs no heap.
 */
#include "status_json.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char *buf;
    size_t limit; /* most characters allowed (without the NUL) */
    size_t len;
    bool ok;
} jb_t;

static bool jb_init(jb_t *j, char *out, size_t out_size)
{
    j->buf = out;
    j->len = 0;
    j->ok = (out != NULL && out_size > 0);
    j->limit = 0;
    if (j->ok) {
        j->limit = out_size - 1;
        if (j->limit > STATUS_JSON_MAX) {
            j->limit = STATUS_JSON_MAX;
        }
        out[0] = '\0';
    }
    return j->ok;
}

/* Only called after a successful jb_init(). */
static size_t jb_fail(jb_t *j)
{
    j->buf[0] = '\0';
    j->ok = false;
    return 0;
}

static size_t jb_finish(jb_t *j)
{
    if (!j->ok) {
        return jb_fail(j);
    }
    j->buf[j->len] = '\0';
    return j->len;
}

static void put_n(jb_t *j, const char *s, size_t n)
{
    if (!j->ok) {
        return;
    }
    if (n > j->limit - j->len) {
        j->ok = false;
        return;
    }
    memcpy(j->buf + j->len, s, n);
    j->len += n;
}

static void put(jb_t *j, const char *s)
{
    put_n(j, s, strlen(s));
}

static void put_str(jb_t *j, const char *s)
{
    if (s == NULL) {
        put(j, "null");
        return;
    }
    put(j, "\"");
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0' && j->ok; p++) {
        char esc[8];
        switch (*p) {
        case '"':
            put(j, "\\\"");
            break;
        case '\\':
            put(j, "\\\\");
            break;
        case '\b':
            put(j, "\\b");
            break;
        case '\f':
            put(j, "\\f");
            break;
        case '\n':
            put(j, "\\n");
            break;
        case '\r':
            put(j, "\\r");
            break;
        case '\t':
            put(j, "\\t");
            break;
        default:
            if (*p < 0x20) {
                snprintf(esc, sizeof(esc), "\\u%04x", (unsigned)*p);
                put(j, esc);
            } else {
                put_n(j, (const char *)p, 1);
            }
            break;
        }
    }
    put(j, "\"");
}

static void put_i64(jb_t *j, int64_t v)
{
    char num[24];
    snprintf(num, sizeof(num), "%" PRId64, v);
    put(j, num);
}

static void put_bool(jb_t *j, bool v)
{
    put(j, v ? "true" : "false");
}

/* ,"key": */
static void key(jb_t *j, const char *k)
{
    put(j, ",\"");
    put(j, k);
    put(j, "\":");
}

static bool common(jb_t *j, const status_common_t *c, const char *node)
{
    if (c->group_count > STATUS_MAX_GROUPS) {
        return false;
    }
    put(j, "{\"online\":true");
    key(j, "node");
    put_str(j, node);
    key(j, "fw");
    put_str(j, c->fw);
    key(j, "built");
    put_str(j, c->built);
    key(j, "boot");
    put_i64(j, c->boot);
    key(j, "reset");
    put_str(j, c->reset);
    key(j, "uptime_s");
    put_i64(j, c->uptime_s);
    key(j, "wifi_rssi");
    put_i64(j, c->wifi_rssi);
    key(j, "ip");
    put_str(j, c->ip);
    key(j, "heap_min");
    put_i64(j, c->heap_min);
    key(j, "groups");
    put(j, "[");
    for (size_t i = 0; i < c->group_count; i++) {
        if (i > 0) {
            put(j, ",");
        }
        put_str(j, c->groups[i]);
    }
    put(j, "]");
    key(j, "settings_version");
    put_i64(j, c->settings_version);
    return true;
}

/* ---- bath ---------------------------------------------------------------- */

size_t status_json_bath(const status_common_t *c, const status_bath_t *b, char *out, size_t out_size)
{
    jb_t j;
    if (!jb_init(&j, out, out_size)) {
        return 0;
    }
    static const char *const modes[] = {"normal", "ambient", "cold"};
    if (c == NULL || b == NULL || (unsigned)b->mode > STATUS_BATH_COLD || b->track_count > STATUS_MAX_TRACK_FOLDERS
        || !common(&j, c, "bath")) {
        return jb_fail(&j);
    }
    key(&j, "mode");
    put_str(&j, modes[b->mode]);
    key(&j, "volumes");
    put(&j, "{\"ambient\":");
    put_i64(&j, b->volume_ambient);
    put(&j, ",\"cold\":");
    put_i64(&j, b->volume_cold);
    put(&j, "}");
    key(&j, "hue");
    put_str(&j, b->hue);
    key(&j, "sd");
    put_str(&j, b->sd);
    key(&j, "tracks");
    put(&j, "{");
    for (size_t i = 0; i < b->track_count; i++) {
        char name[sizeof(b->tracks[i].name)];
        memcpy(name, b->tracks[i].name, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        if (i > 0) {
            put(&j, ",");
        }
        put_str(&j, name);
        put(&j, ":");
        put_i64(&j, b->tracks[i].count);
    }
    put(&j, "}");
    key(&j, "cold");
    if (b->cold_running) {
        put(&j, "{\"phase\":");
        put_str(&j, b->cold_phase);
        put(&j, ",\"remaining_s\":");
        put_i64(&j, b->cold_remaining_s);
        put(&j, ",\"profile\":");
        put_str(&j, b->cold_profile);
        put(&j, ",\"switch\":");
        put_str(&j, b->cold_switch);
        put(&j, "}");
    } else {
        put(&j, "null");
    }
    put(&j, "}");
    return jb_finish(&j);
}

/* ---- speaker ------------------------------------------------------------- */

size_t status_json_speaker(const status_common_t *c, const status_speaker_t *sp, char *out, size_t out_size)
{
    jb_t j;
    if (!jb_init(&j, out, out_size)) {
        return 0;
    }
    static const char *const playing[] = {"none", "alarm", "music"};
    if (c == NULL || sp == NULL || (unsigned)sp->playing > STATUS_PLAY_MUSIC || sp->folder_count > STATUS_MAX_FOLDERS
        || !common(&j, c, "bedroom_speaker")) {
        return jb_fail(&j);
    }
    key(&j, "playing");
    put_str(&j, playing[sp->playing]);
    key(&j, "folder");
    put_str(&j, sp->folder);
    key(&j, "kept");
    put_bool(&j, sp->kept);
    key(&j, "volume");
    put_i64(&j, sp->volume);
    key(&j, "sd");
    put_str(&j, sp->sd);
    key(&j, "folders");
    put(&j, "[");
    for (size_t i = 0; i < sp->folder_count; i++) {
        if (i > 0) {
            put(&j, ",");
        }
        put_str(&j, sp->folders[i]);
    }
    put(&j, "]");
    key(&j, "armed");
    put(&j, "{\"state\":");
    put_str(&j, sp->armed_state);
    put(&j, ",\"version\":");
    put_i64(&j, sp->armed_version);
    put(&j, ",\"alarm_utc\":");
    if (sp->armed_alarm_utc < 0) {
        put(&j, "null");
    } else {
        put_i64(&j, sp->armed_alarm_utc);
    }
    put(&j, "}");
    key(&j, "link");
    put_str(&j, sp->link_up ? "up" : "down");
    put(&j, "}");
    return jb_finish(&j);
}

/* ---- bedside ------------------------------------------------------------- */

size_t status_json_bedside(const status_common_t *c, const status_bedside_t *bs, char *out, size_t out_size)
{
    jb_t j;
    if (!jb_init(&j, out, out_size)) {
        return 0;
    }
    static const char *const sources[] = {"none", "rtc", "sntp"};
    static const char *const modes[] = {"none", "meditation", "mood", "sleep"};
    if (c == NULL || bs == NULL || (unsigned)bs->time_source > STATUS_TIME_SNTP || (unsigned)bs->mode > STATUS_BED_SLEEP
        || !common(&j, c, "bedside")) {
        return jb_fail(&j);
    }
    key(&j, "time");
    put_str(&j, bs->time);
    key(&j, "time_source");
    put_str(&j, sources[bs->time_source]);
    key(&j, "armed_switch");
    put_bool(&j, bs->armed_switch);
    key(&j, "next_alarm");
    if (bs->has_next_alarm) {
        put(&j, "{\"slot\":");
        put_i64(&j, bs->next_alarm_slot);
        put(&j, ",\"local\":");
        put_str(&j, bs->next_alarm_local);
        put(&j, "}");
    } else {
        put(&j, "null");
    }
    key(&j, "alarm");
    put_str(&j, bs->alarm);
    key(&j, "light");
    put_i64(&j, bs->light);
    key(&j, "mode");
    put_str(&j, modes[bs->mode]);
    key(&j, "fading");
    put_bool(&j, bs->fading);
    key(&j, "candles");
    put_bool(&j, bs->candles);
    key(&j, "volumes");
    put(&j, "{\"meditation\":");
    put_i64(&j, bs->volume_meditation);
    put(&j, ",\"mood\":");
    put_i64(&j, bs->volume_mood);
    put(&j, ",\"sleep\":");
    put_i64(&j, bs->volume_sleep);
    put(&j, "}");
    key(&j, "remaining_s");
    if (bs->remaining_s < 0) {
        put(&j, "null");
    } else {
        put_i64(&j, bs->remaining_s);
    }
    key(&j, "speaker");
    put_str(&j, bs->speaker_up ? "up" : "down");
    key(&j, "game");
    put(&j, "{\"state\":");
    put_str(&j, bs->game_state);
    put(&j, ",\"hits\":");
    put_i64(&j, bs->game_hits);
    put(&j, ",\"needed\":");
    put_i64(&j, bs->game_needed);
    put(&j, "}}");
    return jb_finish(&j);
}

/* ---- crate --------------------------------------------------------------- */

static const char *const battery_names[] = {"ok", "low", "empty"};

size_t status_json_crate(const status_common_t *c, const status_crate_t *cr, char *out, size_t out_size)
{
    jb_t j;
    if (!jb_init(&j, out, out_size)) {
        return 0;
    }
    static const char *const states[] = {"closed", "open", "dim", "closing"};
    if (c == NULL || cr == NULL || (unsigned)cr->state > STATUS_CRATE_CLOSING
        || (unsigned)cr->battery > STATUS_BATT_EMPTY || !common(&j, c, "crate")) {
        return jb_fail(&j);
    }
    key(&j, "state");
    put_str(&j, states[cr->state]);
    key(&j, "light");
    put_i64(&j, cr->light);
    key(&j, "playing");
    put_bool(&j, cr->playing);
    key(&j, "volume");
    put_i64(&j, cr->volume);
    key(&j, "battery_mv");
    put_i64(&j, cr->battery_mv);
    key(&j, "battery");
    put_str(&j, battery_names[cr->battery]);
    key(&j, "sd");
    put_str(&j, cr->sd);
    key(&j, "tracks");
    put_i64(&j, cr->tracks);
    put(&j, "}");
    return jb_finish(&j);
}

size_t status_json_crate_asleep(uint16_t battery_mv, status_battery_t battery, char *out, size_t out_size)
{
    jb_t j;
    if (!jb_init(&j, out, out_size)) {
        return 0;
    }
    if ((unsigned)battery > STATUS_BATT_EMPTY) {
        return jb_fail(&j);
    }
    put(&j, "{\"online\":false");
    key(&j, "node");
    put_str(&j, "crate");
    key(&j, "asleep");
    put_bool(&j, true);
    key(&j, "battery_mv");
    put_i64(&j, battery_mv);
    key(&j, "battery");
    put_str(&j, battery_names[battery]);
    put(&j, "}");
    return jb_finish(&j);
}

/* ---- events -------------------------------------------------------------- */

static bool tri_ok(int v)
{
    return v >= -1 && v <= 1;
}

static void event_start(jb_t *j, const char *type)
{
    put(j, "{\"type\":");
    put_str(j, type);
}

static size_t event_end(jb_t *j, int64_t t)
{
    if (t >= 0) {
        key(j, "t");
        put_i64(j, t);
    }
    put(j, "}");
    return jb_finish(j);
}

size_t status_json_event_mode(const char *mode, const char *by, int continued, const char *folder, int64_t t,
                              char *out, size_t out_size)
{
    jb_t j;
    if (!jb_init(&j, out, out_size)) {
        return 0;
    }
    if (mode == NULL || by == NULL || !tri_ok(continued)) {
        return jb_fail(&j);
    }
    event_start(&j, "mode");
    key(&j, "mode");
    put_str(&j, mode);
    key(&j, "by");
    put_str(&j, by);
    if (continued >= 0) {
        key(&j, "continued");
        put_bool(&j, continued == 1);
    }
    if (folder != NULL) {
        key(&j, "folder");
        put_str(&j, folder);
    }
    return event_end(&j, t);
}

size_t status_json_event_alarm(const char *event, uint8_t slot, int game, const char *reason, int64_t t, char *out,
                               size_t out_size)
{
    jb_t j;
    if (!jb_init(&j, out, out_size)) {
        return 0;
    }
    if (event == NULL || !tri_ok(game)) {
        return jb_fail(&j);
    }
    event_start(&j, "alarm");
    key(&j, "event");
    put_str(&j, event);
    if (slot > 0) {
        key(&j, "slot");
        put_i64(&j, slot);
    }
    if (game >= 0) {
        key(&j, "game");
        put_bool(&j, game == 1);
    }
    if (reason != NULL) {
        key(&j, "reason");
        put_str(&j, reason);
    }
    return event_end(&j, t);
}

size_t status_json_event_error(const char *where, const char *text, int64_t t, char *out, size_t out_size)
{
    jb_t j;
    if (!jb_init(&j, out, out_size)) {
        return 0;
    }
    if (where == NULL || text == NULL) {
        return jb_fail(&j);
    }
    event_start(&j, "error");
    key(&j, "where");
    put_str(&j, where);
    key(&j, "text");
    put_str(&j, text);
    return event_end(&j, t);
}

size_t status_json_event_cmd(const char *name, bool ok, const char *why, int64_t t, char *out, size_t out_size)
{
    jb_t j;
    if (!jb_init(&j, out, out_size)) {
        return 0;
    }
    if (name == NULL) {
        return jb_fail(&j);
    }
    event_start(&j, "cmd");
    key(&j, "name");
    put_str(&j, name);
    key(&j, "ok");
    put_bool(&j, ok);
    if (why != NULL) {
        key(&j, "why");
        put_str(&j, why);
    }
    return event_end(&j, t);
}
