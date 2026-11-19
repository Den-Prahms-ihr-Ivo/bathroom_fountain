/*
 * game – the alarm game master: which targets light up, what counts, when
 * the alarm may stop (pure C; runs on the bedside node).
 *
 * Requirements: GAME-01 ... GAME-09, GAME-11 ... GAME-15 (see the
 * requirements doc).
 * Tests:        test/test_game/
 *
 * The bedside app feeds in what the targets report (LINK_TARGET_EVT) and
 * which nodes are alive (lib/link_peer), and sends out the commands this
 * module queues (LINK_TARGET_CMD). When the state becomes GAME_WON the app
 * calls alarm_ring_game_won(); when it becomes GAME_UNAVAILABLE it calls
 * alarm_ring_game_unavailable(), so an alarm can always be stopped. The
 * same game without an alarm is the demo mode. Time is a wrapping uint32
 * millisecond counter. Targets are numbered 0 ... GAME_MAX_TARGETS - 1; a
 * mask has bit n set for target n.
 *
 * Rules
 *   game_rules_from_settings() turns the game_* settings into rules. With a
 *   preset other than custom, hits, lit time, targets at once, zone and
 *   penalty come from this table and the individual settings are ignored:
 *               hits  lit time  at once  zone  penalty
 *     easy       2     15 s       1       1     off
 *     normal     3      8 s       1       2     off
 *     hard       5      4 s       2       3     on
 *   escalate always comes from its setting.
 *
 * Start
 *   game_start() needs the gun alive and at least one target alive;
 *   otherwise the state becomes GAME_UNAVAILABLE and it returns false. It
 *   also returns false (changing nothing) while a game or self-test runs or
 *   if the rules are invalid. On success: GAME_RUNNING, hits 0, hits_needed
 *   = rules.hits, and min(rules.targets_at_once, targets alive) different
 *   targets are lit.
 *
 * Lighting a target
 *   A LINK_TARGET_LIGHT command with arg_ms = rules.lit_ms and a new round
 *   number (1 ... 255, then 1 again). Only an alive target that is not lit
 *   is chosen; the target that was just hit or just timed out is only
 *   chosen again if no other can be. Which of the candidates lights up is
 *   random (seeded at create).
 *
 * While running
 *   game_on_hit(target, round)      counts if that target is lit with that
 *                                   round: hits + 1, the target is no
 *                                   longer lit. When hits reaches
 *                                   hits_needed the game is GAME_WON and
 *                                   every target still lit gets
 *                                   LINK_TARGET_OFF. Otherwise another
 *                                   target lights up.
 *   game_on_timeout(target, round)  the same check; misses + 1; with
 *                                   rules.penalty hits_needed + 1 (never
 *                                   above GAME_MAX_HITS); another target
 *                                   lights up.
 *   Lost report                     a lit target that has reported nothing
 *                                   rules.lit_ms + GAME_GRACE_MS after it
 *                                   was lit is treated as timed out.
 *   Reports with another round, or from a target that is not lit, are
 *   ignored.
 *   game_on_target_lost(target)     the target is not alive any more. If it
 *                                   was lit, another lights up (no miss).
 *                                   If no target is left: all off,
 *                                   GAME_UNAVAILABLE.
 *   game_on_gun_lost()              all lit targets off, GAME_UNAVAILABLE.
 *   game_stop()                     the alarm ended some other way: all lit
 *                                   targets off, GAME_IDLE. Also leaves
 *                                   WON, UNAVAILABLE and SELFTEST_DONE.
 *   outputs.escalation              with rules.escalate: 0 at the start,
 *                                   one more every GAME_ESCALATE_MS, at
 *                                   most 5. Without: always 0. The app
 *                                   sends it to the speaker as
 *                                   LINK_RING_ESCALATE.
 *
 * Self-test (game_start_selftest)
 *   Same start conditions. Every alive target is lit once, lowest number
 *   first, for GAME_SELFTEST_LIT_MS. A hit sets its bit in
 *   outputs.selftest_ok; a timeout (reported or lost) does not. After the
 *   last target: GAME_SELFTEST_DONE.
 *
 * Commands are taken once each, in order (at most GAME_COMMANDS wait).
 */
#ifndef GAME_H
#define GAME_H

#include <stdbool.h>
#include <stdint.h>

#include "link_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GAME_MAX_TARGETS 8
#define GAME_MAX_HITS 30
#define GAME_GRACE_MS 1500
#define GAME_ESCALATE_MS 30000
#define GAME_SELFTEST_LIT_MS 30000
#define GAME_COMMANDS 16

typedef enum {
    GAME_PRESET_EASY = 0,
    GAME_PRESET_NORMAL,
    GAME_PRESET_HARD,
    GAME_PRESET_CUSTOM,
} game_preset_t;

typedef struct {
    uint8_t hits;            /* 1 ... 20      */
    uint16_t lit_ms;         /* 2000 ... 60000 */
    uint8_t targets_at_once; /* 1 ... 4       */
    uint8_t zone;            /* 1 ... 3 (sent to the targets, not used here) */
    bool penalty;
    bool escalate;
} game_rules_t;

typedef enum {
    GAME_IDLE = 0,
    GAME_RUNNING,
    GAME_WON,
    GAME_UNAVAILABLE,
    GAME_SELFTEST,
    GAME_SELFTEST_DONE,
} game_state_t;

typedef struct {
    game_state_t state;
    uint8_t hits;
    uint8_t hits_needed;
    uint8_t misses;      /* stops counting at 255 */
    uint8_t escalation;  /* 0 ... 5 */
    uint8_t lit_mask;    /* targets lit right now */
    uint8_t selftest_ok; /* targets confirmed by the self-test */
} game_outputs_t;

typedef struct {
    uint8_t target;
    link_target_cmd_t cmd;
} game_command_t;

typedef struct game game_t;

/* The settings as stored (cfg_game_t of lib/node_config: preset index, hits,
 * lit time in seconds, ...). Returns false and leaves *out untouched if the
 * preset is unknown, out is NULL, or (custom only) a value is out of range. */
bool game_rules_from_settings(uint8_t preset, uint8_t hits, uint8_t lit_s, uint8_t targets, uint8_t zone,
                              bool penalty, bool escalate, game_rules_t *out);

bool game_rules_valid(const game_rules_t *rules);

/* Returns NULL only when memory runs out. */
game_t *game_create(uint32_t seed, uint32_t now_ms);

/* Accepts NULL. */
void game_destroy(game_t *g);

bool game_start(game_t *g, const game_rules_t *rules, uint8_t alive_mask, bool gun_alive, uint32_t now_ms);
bool game_start_selftest(game_t *g, uint8_t alive_mask, bool gun_alive, uint32_t now_ms);

void game_on_hit(game_t *g, uint8_t target, uint8_t round, uint32_t now_ms);
void game_on_timeout(game_t *g, uint8_t target, uint8_t round, uint32_t now_ms);
void game_on_target_lost(game_t *g, uint8_t target, uint32_t now_ms);
void game_on_gun_lost(game_t *g, uint32_t now_ms);
void game_stop(game_t *g, uint32_t now_ms);

/* Call regularly (e.g. every 20 ms). */
void game_tick(game_t *g, uint32_t now_ms);

/* Current outputs. The pointer stays valid for the object's lifetime. */
const game_outputs_t *game_outputs(const game_t *g);

bool game_take_command(game_t *g, game_command_t *out);

#ifdef __cplusplus
}
#endif

#endif /* GAME_H */
