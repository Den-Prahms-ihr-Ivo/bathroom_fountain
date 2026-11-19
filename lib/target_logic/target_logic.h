/*
 * target_logic – one target: lit, hit, timed out; and how long it may sleep
 * (pure C).
 *
 * Requirements: GAME-40 ... GAME-46 (see the requirements doc).
 * Tests:        test/test_target_logic/
 *
 * The target app receives commands from the bedside node (LINK_TARGET_CMD),
 * decodes laser bursts (lib/pulse_code) and measures how strong each burst
 * arrived. This module decides what that means and what the LED ring shows.
 * Time is a wrapping uint32 millisecond counter.
 *
 * Setup (from LINK_GAME_CFG)
 *   gun_id   only bursts of this gun count; TARGET_ANY_GUN accepts every gun
 *   zone     1 ... 3: how close to the centre a hit has to land. The diffuser
 *            disc passes less light the further out the spot is, so the zone
 *            is a minimum signal strength: cfg.zone_min[zone - 1], on a
 *            scale of 0 ... 1000 where 1000 is a hit in the centre.
 *   Before the first setup: any gun, zone 1.
 *
 * Commands
 *   LINK_TARGET_LIGHT     lit for arg_ms (100 ... 60000, otherwise refused):
 *                         LED TARGET_LED_LIT. A target that is already lit
 *                         starts over with the new round and time.
 *   LINK_TARGET_OFF       dark at once, no event.
 *   LINK_TARGET_IDENTIFY  LED TARGET_LED_IDENTIFY for arg_ms (same limits),
 *                         then dark; ends a lit phase without an event.
 *   LINK_TARGET_CALIBRATE not handled here (the app measures the room light);
 *                         returns false like every unknown command.
 *
 * Shots (target_logic_shot)
 *   Only while lit. A burst from the right gun with strength >= the zone's
 *   minimum is a hit: event LINK_TARGET_HIT with the round, the gun id and
 *   the strength; the LED shows TARGET_LED_FLASH for cfg.flash_ms, then goes
 *   dark. Anything else (other gun, too weak, not lit, flashing) is ignored
 *   and reported by a false return value only.
 *
 * Timeout
 *   If the lit time passes without a hit: event LINK_TARGET_TIMEOUT with the
 *   round, LED dark.
 *
 * Events are taken once each, in order (at most TARGET_EVENTS wait; the
 * oldest is dropped).
 *
 * Sleep (target_sleep_seconds)
 *   A target sleeps deeply and wakes shortly before the alarm:
 *     alarm not known          max_s (wake, ask the bedside node, sleep on)
 *     alarm in <= lead_s       0 (stay awake)
 *     otherwise                min(seconds_to_alarm - lead_s, max_s)
 */
#ifndef TARGET_LOGIC_H
#define TARGET_LOGIC_H

#include <stdbool.h>
#include <stdint.h>

#include "link_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TARGET_EVENTS 4
#define TARGET_ANY_GUN 255

typedef struct {
    uint16_t flash_ms;    /* 50 ... 2000                       default 300 */
    uint16_t zone_min[3]; /* each 1 ... 1000, not decreasing   default 150, 400, 700 */
} target_config_t;

typedef enum {
    TARGET_LED_OFF = 0,
    TARGET_LED_LIT,
    TARGET_LED_FLASH,
    TARGET_LED_IDENTIFY,
} target_led_t;

typedef struct target_logic target_logic_t;

void target_config_default(target_config_t *cfg);

/* Returns NULL if cfg is NULL or a value is out of range. */
target_logic_t *target_logic_create(const target_config_t *cfg, uint32_t now_ms);

/* Accepts NULL. */
void target_logic_destroy(target_logic_t *t);

/* Returns false and changes nothing if gun_id is above 15 and not
 * TARGET_ANY_GUN, or zone is not 1 ... 3. */
bool target_logic_setup(target_logic_t *t, uint8_t gun_id, uint8_t zone);

/* Returns true if the command was carried out. */
bool target_logic_command(target_logic_t *t, const link_target_cmd_t *cmd, uint32_t now_ms);

/* Returns true if the burst counted as a hit. */
bool target_logic_shot(target_logic_t *t, uint8_t gun_id, uint16_t strength, uint32_t now_ms);

/* Call regularly (e.g. every 20 ms). */
void target_logic_tick(target_logic_t *t, uint32_t now_ms);

target_led_t target_logic_led(const target_logic_t *t);

bool target_logic_take_event(target_logic_t *t, link_target_evt_t *out);

uint32_t target_sleep_seconds(bool alarm_known, uint32_t seconds_to_alarm, uint32_t lead_s, uint32_t max_s);

#ifdef __cplusplus
}
#endif

#endif /* TARGET_LOGIC_H */
