/*
 * alarm_schedule – alarms as text, and "when does the next one ring?"
 * (pure C, no clock, no time zone database).
 *
 * Requirements: BED-01 ... BED-04 (see the requirements doc).
 * Tests:        test/test_alarm_schedule/
 *
 * Only the bedside node uses this module: it owns the schedule (BED-01).
 *
 * Text form (settings keys alarm1 ... alarm6, the web app and the console)
 *     HH:MM,DDDDDDD,on      e.g.  "06:45,mtwtf--,on"   weekdays at 6:45
 *     HH:MM,DDDDDDD,off           "09:30,-----ss,off"  weekend, switched off
 *   HH 00-23 and MM 00-59, always two digits. DDDDDDD is one character per
 *   day, Monday first: the day's letter (m t w t f s s) or '-' for "not on
 *   this day". "-------" is a one-time alarm: it rings at the next HH:MM and
 *   the bedside app then switches it off. Lower case only, exactly 16 or 17
 *   characters, nothing before or after.
 *   The empty string is an unused slot; it is valid as a setting but
 *   alarm_parse() returns false for it.
 *
 * Time
 *   The caller passes local time as "local seconds": seconds since
 *   1970-01-01 00:00 on the local wall clock (UTC seconds plus the current
 *   UTC offset). Day 0 was a Thursday. The module never sees UTC; the
 *   platform converts the result back with mktime(), which also settles what
 *   an alarm inside a daylight-saving jump means.
 *
 * alarm_next()
 *   The earliest ring time strictly after after_local_s among the alarms
 *   that are used and enabled. An alarm rings at second 0 of its minute on
 *   every day whose flag is set; a one-time alarm on the first day its HH:MM
 *   comes. If two alarms ring at the same time, the lower slot wins. Returns
 *   false (out untouched) if armed is false, no alarm qualifies, or an
 *   argument is NULL. armed is the position of the "alarms armed" switch
 *   (BED-04).
 */
#ifndef ALARM_SCHEDULE_H
#define ALARM_SCHEDULE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ALARM_SLOTS 6
#define ALARM_TEXT_MAX 17 /* longest text without the NUL */

typedef struct {
    bool used;      /* false: empty slot, the other fields are ignored */
    bool enabled;
    uint8_t hour;   /* 0 ... 23 */
    uint8_t minute; /* 0 ... 59 */
    uint8_t days;   /* bit 0 = Monday ... bit 6 = Sunday; 0 = one-time */
} alarm_t;

typedef struct {
    int64_t local_s; /* ring time in local seconds */
    uint8_t slot;    /* index into the alarms array */
} alarm_next_t;

/* Parse the text form. On success *out is filled with used = true. Returns
 * false and leaves *out untouched for anything else, including "" and NULL. */
bool alarm_parse(const char *text, alarm_t *out);

/* True for "" and for every text alarm_parse() accepts (the kv_table check
 * for the alarm keys). */
bool alarm_text_valid(const char *text);

/* Write the text form ("" for an unused slot). Returns false and leaves out
 * empty if out is too small (ALARM_TEXT_MAX + 1 always fits) or a field is
 * out of range. */
bool alarm_format(const alarm_t *alarm, char *out, size_t out_size);

bool alarm_next(const alarm_t *alarms, size_t count, bool armed, int64_t after_local_s, alarm_next_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ALARM_SCHEDULE_H */
