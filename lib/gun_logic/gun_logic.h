/*
 * gun_logic – what the laser gun does with its trigger: shots, ammunition,
 * glowing sights and going to sleep (pure C).
 *
 * Requirements: GAME-30 ... GAME-36 (see the requirements doc).
 * Tests:        test/test_gun_logic/
 *
 * The gun app debounces the trigger (lib/button is not needed: only the
 * level matters), reads the tilt switch and the magazine switch, and passes
 * all of it in here. What comes out are actions: fire a coded burst
 * (lib/pulse_code), click dry, tell the bedside node. Time is a wrapping
 * uint32 millisecond counter.
 *
 * Setup
 *   gun_logic_setup() takes what the bedside node sent with LINK_GAME_CFG:
 *   gun id, sight brightness, magazine size. It refills the magazine. Before
 *   the first setup: id 0, sights 30 %, no magazine.
 *
 * Trigger
 *   A press (released -> pressed) fires, unless less than cfg.cooldown_ms
 *   have passed since the last press that fired or clicked dry: then it is
 *   ignored.
 *     GUN_ACT_FIRE  shot = the shot counter, which then counts on
 *                   (0 ... 15, wrapping); with a magazine, one round less.
 *     GUN_ACT_DRY   instead of FIRE when the magazine is empty or removed.
 *                   No laser, the counter stays.
 *   Holding the trigger for cfg.demo_hold_ms gives GUN_ACT_DEMO_REQUEST once
 *   (the press itself has fired as usual).
 *
 * Magazine (the input is reserved; it only matters when setup.ammo > 0)
 *   The gun starts with the magazine in. Taking it out makes every press
 *   DRY; putting it back in refills to setup.ammo and gives GUN_ACT_RELOADED.
 *   Reporting the state it already has does nothing.
 *   outputs.ammo_left is 255 without a magazine feature.
 *
 * Sleep
 *   Every trigger change, gun_logic_motion() (tilt switch) and setup counts
 *   as activity. After cfg.idle_sleep_ms without any, and with the trigger
 *   released, outputs.sleep becomes true and the sights go to 0; the app
 *   then enters deep sleep. Activity clears it again.
 *
 * Every action carries the ammunition left after it. Actions are taken once
 * each, in order (at most GUN_ACTIONS wait; the oldest is dropped).
 */
#ifndef GUN_LOGIC_H
#define GUN_LOGIC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GUN_ACTIONS 4
#define GUN_NO_MAGAZINE 255

typedef struct {
    uint16_t cooldown_ms;   /* 50 ... 2000        default 250   */
    uint32_t idle_sleep_ms; /* 5000 ... 600000    default 60000 */
    uint16_t demo_hold_ms;  /* 1000 ... 10000     default 3000  */
} gun_config_t;

typedef struct {
    uint8_t gun_id; /* 0 ... 15           */
    uint8_t sights; /* 0 ... 100 percent  */
    uint8_t ammo;   /* 0 = no magazine, else rounds per magazine (1 ... 30) */
} gun_setup_t;

typedef enum {
    GUN_ACT_FIRE = 1,
    GUN_ACT_DRY,
    GUN_ACT_RELOADED,
    GUN_ACT_DEMO_REQUEST,
} gun_action_id_t;

typedef struct {
    gun_action_id_t id;
    uint8_t shot;      /* FIRE: the number to send in the burst */
    uint8_t ammo_left; /* after the action; GUN_NO_MAGAZINE if none */
} gun_action_t;

typedef struct {
    uint8_t gun_id;
    uint8_t sights;    /* 0 ... 100; 0 while sleep is true */
    uint8_t ammo_left; /* GUN_NO_MAGAZINE if none */
    bool sleep;
} gun_outputs_t;

typedef struct gun_logic gun_logic_t;

void gun_config_default(gun_config_t *cfg);

/* Returns NULL if cfg is NULL or a value is out of range. */
gun_logic_t *gun_logic_create(const gun_config_t *cfg, uint32_t now_ms);

/* Accepts NULL. */
void gun_logic_destroy(gun_logic_t *g);

/* Returns false and changes nothing if a value is out of range. */
bool gun_logic_setup(gun_logic_t *g, const gun_setup_t *setup, uint32_t now_ms);

/* Debounced trigger level; call on every change (repeats are harmless). */
void gun_logic_trigger(gun_logic_t *g, bool pressed, uint32_t now_ms);

void gun_logic_motion(gun_logic_t *g, uint32_t now_ms);

void gun_logic_magazine(gun_logic_t *g, bool inserted, uint32_t now_ms);

/* Call regularly (e.g. every 20 ms). */
void gun_logic_tick(gun_logic_t *g, uint32_t now_ms);

/* Current outputs. The pointer stays valid for the object's lifetime. */
const gun_outputs_t *gun_logic_outputs(const gun_logic_t *g);

bool gun_logic_take_action(gun_logic_t *g, gun_action_t *out);

#ifdef __cplusplus
}
#endif

#endif /* GUN_LOGIC_H */
