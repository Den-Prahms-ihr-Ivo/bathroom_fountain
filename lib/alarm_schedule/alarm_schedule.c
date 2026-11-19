/*
 * alarm_schedule – alarms as text, and the next ring time.
 * Contract in alarm_schedule.h, tests in test/test_alarm_schedule/.
 */
#include "alarm_schedule.h"

#include <string.h>

static const char DAY_LETTERS[7] = {'m', 't', 'w', 't', 'f', 's', 's'};

static bool two_digits(const char *p, unsigned limit, uint8_t *out)
{
    if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9') {
        return false;
    }
    const unsigned v = (unsigned)(p[0] - '0') * 10u + (unsigned)(p[1] - '0');
    if (v > limit) {
        return false;
    }
    *out = (uint8_t)v;
    return true;
}

bool alarm_parse(const char *text, alarm_t *out)
{
    if (text == NULL || out == NULL) {
        return false;
    }
    const size_t len = strlen(text);
    if (len != 16 && len != 17) {
        return false;
    }
    alarm_t a = {.used = true};
    if (!two_digits(text, 23, &a.hour) || text[2] != ':' || !two_digits(text + 3, 59, &a.minute) ||
        text[5] != ',' || text[13] != ',') {
        return false;
    }
    for (int i = 0; i < 7; i++) {
        const char c = text[6 + i];
        if (c == DAY_LETTERS[i]) {
            a.days |= (uint8_t)(1u << i);
        } else if (c != '-') {
            return false;
        }
    }
    const char *state = text + 14;
    if (strcmp(state, "on") == 0) {
        a.enabled = true;
    } else if (strcmp(state, "off") == 0) {
        a.enabled = false;
    } else {
        return false;
    }
    *out = a;
    return true;
}

bool alarm_text_valid(const char *text)
{
    if (text == NULL) {
        return false;
    }
    alarm_t a;
    return text[0] == '\0' || alarm_parse(text, &a);
}

bool alarm_format(const alarm_t *alarm, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return false;
    }
    out[0] = '\0';
    if (alarm == NULL) {
        return false;
    }
    if (!alarm->used) {
        return true;
    }
    if (alarm->hour > 23 || alarm->minute > 59 || alarm->days > 0x7F) {
        return false;
    }
    char buf[ALARM_TEXT_MAX + 1];
    buf[0] = (char)('0' + alarm->hour / 10);
    buf[1] = (char)('0' + alarm->hour % 10);
    buf[2] = ':';
    buf[3] = (char)('0' + alarm->minute / 10);
    buf[4] = (char)('0' + alarm->minute % 10);
    buf[5] = ',';
    for (int i = 0; i < 7; i++) {
        buf[6 + i] = (alarm->days & (1u << i)) ? DAY_LETTERS[i] : '-';
    }
    buf[13] = ',';
    strcpy(buf + 14, alarm->enabled ? "on" : "off");
    const size_t len = strlen(buf);
    if (len + 1 > out_size) {
        return false;
    }
    memcpy(out, buf, len + 1);
    return true;
}

static int64_t floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) {
        q--;
    }
    return q;
}

/* Monday = 0 ... Sunday = 6; day 0 (1970-01-01) was a Thursday. */
static int weekday(int64_t day)
{
    int64_t w = (day + 3) % 7;
    if (w < 0) {
        w += 7;
    }
    return (int)w;
}

static bool next_for(const alarm_t *a, int64_t after, int64_t *ring)
{
    const int64_t day0 = floor_div(after, 86400);
    const int64_t tod = (int64_t)a->hour * 3600 + (int64_t)a->minute * 60;
    for (int64_t d = day0; d <= day0 + 7; d++) {
        const int64_t t = d * 86400 + tod;
        if (t <= after) {
            continue;
        }
        if (a->days == 0 || (a->days & (1u << weekday(d)))) {
            *ring = t;
            return true;
        }
    }
    return false;
}

bool alarm_next(const alarm_t *alarms, size_t count, bool armed, int64_t after_local_s, alarm_next_t *out)
{
    if (alarms == NULL || out == NULL || !armed) {
        return false;
    }
    bool found = false;
    alarm_next_t best = {0, 0};
    for (size_t i = 0; i < count; i++) {
        const alarm_t *a = &alarms[i];
        if (!a->used || !a->enabled || a->hour > 23 || a->minute > 59) {
            continue;
        }
        int64_t t;
        if (next_for(a, after_local_s, &t) && (!found || t < best.local_s)) {
            best.local_s = t;
            best.slot = (uint8_t)i;
            found = true;
        }
    }
    if (found) {
        *out = best;
    }
    return found;
}
