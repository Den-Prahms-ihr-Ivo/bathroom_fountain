/*
 * settings – every runtime setting as validated text key/value pairs.
 *
 * The contract is in settings.h, the tests in test/test_settings/.
 *
 * One table describes every key: its kind, where it lives in settings_t and
 * its limits. Floats are handled as exact hundredths (the syntax allows at
 * most two decimals), so range checks are exact and format() → set()
 * reproduces every value.
 */
#include "settings.h"

#include <stdio.h>
#include <string.h>

typedef enum {
    KIND_UINT,
    KIND_FLOAT,     /* limits in hundredths */
    KIND_SSID,      /* any bytes except control characters */
    KIND_PASS,      /* printable ASCII, "" or min…max chars, secret */
    KIND_HOST,      /* letters, digits, '.', '-' */
    KIND_KEY,       /* letters, digits, '_', '-', secret */
    KIND_UUID,
} kind_t;

typedef struct {
    const char *name;
    kind_t kind;
    size_t offset;
    size_t size;    /* field size: buffer size for strings, integer width for uints */
    int64_t min;    /* value (uint), hundredths (float) or length (strings) */
    int64_t max;
} key_def_t;

#define FIELD(f) offsetof(settings_t, f), sizeof(((settings_t *)0)->f)

/* Table order is the settings_key_name() order. */
static const key_def_t KEYS[] = {
    {"wifi_ssid",            KIND_SSID,  FIELD(wifi_ssid),            1,     32},
    {"wifi_pass",            KIND_PASS,  FIELD(wifi_pass),            8,     63},
    {"hue_host",             KIND_HOST,  FIELD(hue_host),             1,     63},
    {"hue_key",              KIND_KEY,   FIELD(hue_key),              1,     64},
    {"scene_normal",         KIND_UUID,  FIELD(scene_normal),         36,    36},
    {"scene_ambient",        KIND_UUID,  FIELD(scene_ambient),        36,    36},
    {"transition_ms",        KIND_UINT,  FIELD(transition_ms),        3000,  5000},
    {"volume_max",           KIND_UINT,  FIELD(volume_max),           1,     32},
    {"clap_min_gap_ms",      KIND_UINT,  FIELD(clap_min_gap_ms),      50,    1000},
    {"clap_max_gap_ms",      KIND_UINT,  FIELD(clap_max_gap_ms),      100,   2000},
    {"clap_level_diff_db",   KIND_FLOAT, FIELD(clap_level_diff_db),   0,     2000},
    {"clap_quiet_before_ms", KIND_UINT,  FIELD(clap_quiet_before_ms), 0,     2000},
    {"clap_quiet_after_ms",  KIND_UINT,  FIELD(clap_quiet_after_ms),  0,     2000},
    {"clap_gap_factor",      KIND_FLOAT, FIELD(clap_gap_factor),      0,     400},
    {"clap_drop_db",         KIND_FLOAT, FIELD(clap_drop_db),         100,   4000},
    {"clap_onset_db",        KIND_FLOAT, FIELD(clap_onset_db),        100,   4000},
    {"clap_min_peak_dbfs",   KIND_FLOAT, FIELD(clap_min_peak_dbfs),   -9000, 0},
    {"mic_hp_hz",            KIND_FLOAT, FIELD(mic_hp_hz),            0,     100000},
    {"mic_shift",            KIND_UINT,  FIELD(mic_shift),            8,     24},
};

#define KEY_COUNT    (sizeof(KEYS) / sizeof(KEYS[0]))
#define NUMBER_CAP   1000000000000LL /* parsing stops growing here: far out of every range */
#define FORMAT_MAX   80              /* longest formatted value (64-char key) + NUL */

void settings_defaults(settings_t *s)
{
    if (s == NULL) {
        return;
    }
    memset(s, 0, sizeof(*s)); /* strings "" (unset) */
    s->transition_ms = 4000;
    s->volume_max = 32;
    s->clap_min_gap_ms = 150;
    s->clap_max_gap_ms = 600;
    s->clap_level_diff_db = 6.0f;
    s->clap_quiet_before_ms = 300;
    s->clap_quiet_after_ms = 400;
    s->clap_gap_factor = 1.5f;
    s->clap_drop_db = 12.0f;
    s->clap_onset_db = 12.0f;
    s->clap_min_peak_dbfs = -40.0f;
    s->mic_hp_hz = 20.0f;
    s->mic_shift = 16;
}

static const key_def_t *find_key(const char *name)
{
    for (size_t i = 0; i < KEY_COUNT; i++) {
        if (strcmp(KEYS[i].name, name) == 0) {
            return &KEYS[i];
        }
    }
    return NULL;
}

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static bool is_alnum(char c)
{
    return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool is_hex(char c)
{
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* Append one digit, saturating at NUMBER_CAP so huge inputs land out of range
 * instead of wrapping. */
static int64_t push_digit(int64_t v, char c)
{
    return (v >= NUMBER_CAP) ? NUMBER_CAP : v * 10 + (c - '0');
}

/* One or more digits, nothing else. */
static bool parse_uint(const char *t, int64_t *out)
{
    int64_t v = 0;
    if (*t == '\0') {
        return false;
    }
    for (; *t != '\0'; t++) {
        if (!is_digit(*t)) {
            return false;
        }
        v = push_digit(v, *t);
    }
    *out = v;
    return true;
}

/* Optional '-', digits, optionally '.' and 1–2 digits; result in hundredths. */
static bool parse_hundredths(const char *t, int64_t *out)
{
    const bool negative = (*t == '-');
    if (negative) {
        t++;
    }
    if (!is_digit(*t)) {
        return false;
    }
    int64_t v = 0;
    for (; is_digit(*t); t++) {
        v = push_digit(v, *t);
    }
    int decimals = 0;
    if (*t == '.') {
        t++;
        for (; is_digit(*t) && decimals < 3; t++, decimals++) {
            v = push_digit(v, *t);
        }
        if (decimals == 0 || decimals > 2) {
            return false;
        }
    }
    if (*t != '\0') {
        return false;
    }
    for (; decimals < 2; decimals++) {
        v = push_digit(v, '0');
    }
    *out = negative ? -v : v;
    return true;
}

static bool uuid_valid(const char *t)
{
    for (size_t i = 0; i < 36; i++) {
        const bool hyphen_pos = (i == 8 || i == 13 || i == 18 || i == 23);
        if (hyphen_pos ? (t[i] != '-') : !is_hex(t[i])) {
            return false; /* also stops at an early NUL */
        }
    }
    return t[36] == '\0';
}

static bool string_chars_valid(kind_t kind, const char *t)
{
    for (; *t != '\0'; t++) {
        const unsigned char c = (unsigned char)*t;
        bool ok = false;
        switch (kind) {
        case KIND_SSID:
            ok = c >= 0x20 && c != 0x7F; /* UTF-8 bytes allowed */
            break;
        case KIND_PASS:
            ok = c >= 0x20 && c <= 0x7E;
            break;
        case KIND_HOST:
            ok = is_alnum(*t) || *t == '.' || *t == '-';
            break;
        case KIND_KEY:
            ok = is_alnum(*t) || *t == '_' || *t == '-';
            break;
        default:
            break;
        }
        if (!ok) {
            return false;
        }
    }
    return true;
}

static void store_uint(settings_t *s, const key_def_t *k, uint32_t v)
{
    uint8_t *field = (uint8_t *)s + k->offset;
    if (k->size == sizeof(uint8_t)) {
        *field = (uint8_t)v;
    } else if (k->size == sizeof(uint16_t)) {
        const uint16_t v16 = (uint16_t)v;
        memcpy(field, &v16, sizeof(v16));
    } else {
        memcpy(field, &v, sizeof(v));
    }
}

static uint32_t load_uint(const settings_t *s, const key_def_t *k)
{
    const uint8_t *field = (const uint8_t *)s + k->offset;
    if (k->size == sizeof(uint8_t)) {
        return *field;
    }
    if (k->size == sizeof(uint16_t)) {
        uint16_t v16;
        memcpy(&v16, field, sizeof(v16));
        return v16;
    }
    uint32_t v;
    memcpy(&v, field, sizeof(v));
    return v;
}

static float *float_field(settings_t *s, const key_def_t *k)
{
    return (float *)(void *)((uint8_t *)s + k->offset);
}

static const float *float_field_const(const settings_t *s, const key_def_t *k)
{
    return (const float *)(const void *)((const uint8_t *)s + k->offset);
}

/* Cross-key rule: clap_min_gap_ms < clap_max_gap_ms. */
static bool gap_conflict(const settings_t *s, const key_def_t *k, int64_t v)
{
    if (strcmp(k->name, "clap_min_gap_ms") == 0) {
        return v >= s->clap_max_gap_ms;
    }
    if (strcmp(k->name, "clap_max_gap_ms") == 0) {
        return v <= s->clap_min_gap_ms;
    }
    return false;
}

settings_result_t settings_set(settings_t *s, const char *key, const char *value)
{
    if (s == NULL || key == NULL || value == NULL) {
        return SETTINGS_ERR_INVALID;
    }
    const key_def_t *k = find_key(key);
    if (k == NULL) {
        return SETTINGS_ERR_UNKNOWN_KEY;
    }

    int64_t v = 0;
    switch (k->kind) {
    case KIND_UINT:
    case KIND_FLOAT: {
        const bool ok = (k->kind == KIND_UINT) ? parse_uint(value, &v) : parse_hundredths(value, &v);
        if (!ok) {
            return SETTINGS_ERR_INVALID;
        }
        if (v < k->min || v > k->max) {
            return SETTINGS_ERR_RANGE;
        }
        if (gap_conflict(s, k, v)) {
            return SETTINGS_ERR_CONFLICT;
        }
        if (k->kind == KIND_UINT) {
            store_uint(s, k, (uint32_t)v);
        } else {
            *float_field(s, k) = (float)v / 100.0f;
        }
        return SETTINGS_OK;
    }

    case KIND_UUID:
        if (!uuid_valid(value)) {
            return SETTINGS_ERR_INVALID;
        }
        break;

    default: {
        if (!string_chars_valid(k->kind, value)) {
            return SETTINGS_ERR_INVALID;
        }
        const size_t len = strlen(value);
        const bool empty_ok = (k->kind == KIND_PASS && len == 0); /* open network */
        if (!empty_ok && (len < (size_t)k->min || len > (size_t)k->max)) {
            return SETTINGS_ERR_RANGE;
        }
        break;
    }
    }

    /* The table limits fit the buffers; this guards against a future table edit. */
    const size_t len = strlen(value);
    if (len >= k->size) {
        return SETTINGS_ERR_RANGE;
    }
    memcpy((uint8_t *)s + k->offset, value, len + 1);
    return SETTINGS_OK;
}

/* Fewest decimals (1 or 2) that represent the value to 2 places. */
static void format_float(float f, char *buf, size_t size)
{
    const float scaled = f * 100.0f;
    const int64_t h = (int64_t)(scaled + (scaled < 0.0f ? -0.5f : 0.5f));
    const int64_t mag = (h < 0) ? -h : h;
    const char *sign = (h < 0) ? "-" : "";
    if (mag % 10 == 0) {
        snprintf(buf, size, "%s%lld.%lld", sign, (long long)(mag / 100), (long long)(mag % 100 / 10));
    } else {
        snprintf(buf, size, "%s%lld.%02lld", sign, (long long)(mag / 100), (long long)(mag % 100));
    }
}

bool settings_format(const settings_t *s, const char *key, bool reveal_secrets, char *out, size_t out_size)
{
    if (out != NULL && out_size > 0) {
        out[0] = '\0';
    }
    const key_def_t *k = (key != NULL) ? find_key(key) : NULL;
    if (s == NULL || k == NULL || out == NULL) {
        return false;
    }

    char buf[FORMAT_MAX];
    switch (k->kind) {
    case KIND_UINT:
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)load_uint(s, k));
        break;
    case KIND_FLOAT:
        format_float(*float_field_const(s, k), buf, sizeof(buf));
        break;
    default: {
        const char *str = (const char *)s + k->offset;
        const bool secret = (k->kind == KIND_PASS || k->kind == KIND_KEY);
        snprintf(buf, sizeof(buf), "%s", (secret && !reveal_secrets && str[0] != '\0') ? "********" : str);
        break;
    }
    }

    const size_t len = strlen(buf);
    if (len >= out_size) {
        return false; /* out stays empty */
    }
    memcpy(out, buf, len + 1);
    return true;
}

size_t settings_key_count(void)
{
    return KEY_COUNT;
}

const char *settings_key_name(size_t index)
{
    return (index < KEY_COUNT) ? KEYS[index].name : NULL;
}

bool settings_is_complete(const settings_t *s)
{
    return s != NULL
        && s->wifi_ssid[0] != '\0'
        && s->hue_host[0] != '\0'
        && s->hue_key[0] != '\0'
        && s->scene_normal[0] != '\0'
        && s->scene_ambient[0] != '\0';
}
