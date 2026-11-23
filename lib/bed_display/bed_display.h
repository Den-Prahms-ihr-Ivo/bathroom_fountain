/*
 * bed_display – when the bedside OLED is on, and what it shows (pure C).
 *
 * Requirements: the bedside display, decided 2026-11-23 (not yet numbered in
 *               the requirements doc).
 * Tests:        test/test_bed_display/
 *
 * The OLED is dark (panel off, true black) unless something below switches
 * it on. This module only decides; the app switches the panel (SSD1309
 * display on/off), draws the three texts and passes the inputs. Time is a
 * wrapping uint32 millisecond counter, like alarm_ring.
 *
 * ---- On / off (bed_display_t) ----------------------------------------------
 *
 * Two independent windows; the display is on while either is open.
 *
 *   Press window   every press of the big button, short or long, in any
 *                  alarm state, opens it for cfg.press_ms from the press
 *                  (setting display_s). A press while it is open restarts it
 *                  from the new press. (The press's alarm effect, snooze or
 *                  off, is alarm_ring's business, not this module's.)
 *
 *   Auto window    opened by the alarm, for BED_DISPLAY_AUTO_MS (10 min)
 *                  from the moment it opens:
 *                    - the alarm state enters ALARM_SUNRISE, if
 *                      cfg.sunrise_auto (setting display_sunrise);
 *                    - the alarm state enters ALARM_RINGING, from any state,
 *                      also when it comes back from a snooze.
 *                  Each of these opens a fresh 10 min window. Entering
 *                  ALARM_SNOOZED does not: the countdown stays visible for
 *                  what is left of the window. When the window has run out
 *                  the display goes dark even though the sunrise, ringing or
 *                  snooze goes on; a press shows it again for press_ms.
 *                  The auto window closes at once when the alarm state
 *                  leaves SUNRISE / RINGING / SNOOZED (alarm over, or
 *                  disarmed); an open press window stays.
 *
 *   The state is only seen through bed_display_update(), so call it every
 *   tick with the current alarm state (alarm_ring_outputs()->state).
 *   bed_display_update() also forgets windows that have run out, so a
 *   window never looks open again after the counter wraps (2^31 ms).
 *   bed_display_press() and bed_display_update() in the same tick may come
 *   in either order. A cfg change applies to windows opened afterwards.
 *
 * ---- Content (bed_display_text) ----------------------------------------------
 *
 *   big       ALARM_SNOOZED: the remaining snooze time as "m:ss", seconds
 *             rounded up (8:42, 0:05, 12:00; 0 ms left shows "0:00";
 *             more than 99:59 shows "99:59").
 *             Otherwise the local time "H:MM", 24 h, no leading zero
 *             (6:05, 23:59, 0:00). Time unknown (no SNTP or RTC yet):
 *             "--:--".
 *   alarm     bottom left: the next alarm as "Ddd H:MM" ("Mon 6:00"), with
 *             alarm_icon true (the app draws the small alarm clock in front
 *             of it). No next alarm (armed switch off, empty schedule):
 *             "" and alarm_icon false.
 *   date      bottom right: today as "D Mmm" ("23 Nov", "1 Jan"). Time
 *             unknown: "".
 *   English day and month names. A struct tm with a field out of range
 *   (tm_wday 0-6, tm_mon 0-11, tm_mday 1-31, tm_hour 0-23, tm_min 0-59)
 *   counts as unknown / no alarm.
 *
 * Settings (group "bed", node_config):
 *   display_s        uint  3-120 s, how long a press shows the display   10
 *   display_sunrise  bool  the display switches on at sunrise start     on
 * The ring always switches it on; the 10 min of the auto window are fixed.
 */
#ifndef BED_DISPLAY_H
#define BED_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "alarm_ring.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BED_DISPLAY_AUTO_MS 600000u /* 10 min */

typedef struct {
    uint32_t press_ms;  /* display_s * 1000 */
    bool sunrise_auto;  /* display_sunrise  */
} bed_display_cfg_t;

/* Fields are private; allocate it statically and use the functions. */
typedef struct {
    bed_display_cfg_t cfg;
    alarm_state_t last;   /* alarm state at the previous update */
    bool press_open;
    uint32_t press_until;
    bool auto_open;
    uint32_t auto_until;
} bed_display_t;

/* Dark, no window open, last alarm state ALARM_IDLE. */
void bed_display_init(bed_display_t *d, const bed_display_cfg_t *cfg);

/* New settings for windows opened from now on. */
void bed_display_set_cfg(bed_display_t *d, const bed_display_cfg_t *cfg);

/* The big button was pressed (short or long; call once per press). */
void bed_display_press(bed_display_t *d, uint32_t now_ms);

/* Every tick: the current alarm state. Returns bed_display_on(). */
bool bed_display_update(bed_display_t *d, uint32_t now_ms, alarm_state_t alarm);

/* Whether the panel should be on now, as of the last press / update. */
bool bed_display_on(const bed_display_t *d, uint32_t now_ms);

/* ---- content ---------------------------------------------------------------- */

typedef struct {
    char big[8];      /* "23:59", "12:00", "--:--" */
    bool alarm_icon;
    char alarm[12];   /* "Mon 6:00" or "" */
    char date[8];     /* "23 Nov" or "" */
} bed_display_text_t;

/* now:        local time, NULL = unknown.
 * next_alarm: local time of the next alarm, NULL = none.
 * snooze_left_ms is used only when alarm == ALARM_SNOOZED (the app keeps
 * the snooze end from ALARM_EV_SNOOZE). Always fills *out. */
void bed_display_text(const struct tm *now, const struct tm *next_alarm, alarm_state_t alarm,
                      uint32_t snooze_left_ms, bed_display_text_t *out);

#ifdef __cplusplus
}
#endif

#endif /* BED_DISPLAY_H */
