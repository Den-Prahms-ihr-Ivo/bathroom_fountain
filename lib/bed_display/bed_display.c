/*
 * bed_display – when the bedside OLED is on, and what it shows.
 * Contract in bed_display.h, tests in test/test_bed_display/.
 *
 * STUB: replace it with the real thing until test_bed_display passes.
 */
#include "bed_display.h"

#include <string.h>

void bed_display_init(bed_display_t *d, const bed_display_cfg_t *cfg)
{
    memset(d, 0, sizeof(*d));
    (void)cfg;
}

void bed_display_set_cfg(bed_display_t *d, const bed_display_cfg_t *cfg)
{
    (void)d;
    (void)cfg;
}

void bed_display_press(bed_display_t *d, uint32_t now_ms)
{
    (void)d;
    (void)now_ms;
}

bool bed_display_update(bed_display_t *d, uint32_t now_ms, alarm_state_t alarm)
{
    (void)d;
    (void)now_ms;
    (void)alarm;
    return false;
}

bool bed_display_on(const bed_display_t *d, uint32_t now_ms)
{
    (void)d;
    (void)now_ms;
    return false;
}

void bed_display_text(const struct tm *now, const struct tm *next_alarm, alarm_state_t alarm,
                      uint32_t snooze_left_ms, bed_display_text_t *out)
{
    (void)now;
    (void)next_alarm;
    (void)alarm;
    (void)snooze_left_ms;
    memset(out, 0, sizeof(*out));
}
