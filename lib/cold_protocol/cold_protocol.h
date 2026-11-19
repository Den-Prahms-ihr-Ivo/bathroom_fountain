/*
 * cold_protocol – the timed cold-shower sequence (pure C, no hardware).
 *
 * Requirements: COLD-01 ... COLD-08, COLD-13 (see the requirements doc).
 * Tests:        test/test_cold_protocol/
 *
 * Countdown -> Warm -> Cold -> After -> idle. The module only keeps time and
 * says what should happen; the control task turns that into music folders,
 * voice clips and Hue scenes. Like mode_controller it reports levels through
 * an outputs struct and one-off events through take functions.
 *
 * Time is a wrapping uint32 millisecond counter. Every call that receives
 * now_ms first brings the state up to date: a phase change happens at its
 * planned time even if the call comes later, and the next phase is timed from
 * that planned time.
 *
 * Profile
 *   The durations and switches of one run (the hype or the zen profile). It
 *   is given at start. cold_protocol_set_profile() during a run stores a new
 *   profile that takes effect at the next phase change, for the phases that
 *   begin from then on; nothing about the running phase changes (COLD-13).
 *
 * Phases
 *   start()     Only from idle. If the GO cue is a voice clip, the phase is
 *               COUNTDOWN for the length of that clip and the first timed
 *               phase begins when the clip ends. If GO is a beep, the first
 *               timed phase begins at once.
 *   WARM        profile.warm_s seconds; skipped completely when warm_s is 0
 *               (then the first timed phase is COLD and no TO_COLD cue exists).
 *   COLD        profile.cold_s seconds.
 *   AFTER       profile.after_s seconds, only if profile.calm; otherwise the
 *               run ends with COLD.
 *   The run ends with COLD_END_FINISHED when the last phase is over.
 *
 * Cues
 *   GO at start, TO_COLD at the end of WARM, DONE at the end of COLD, and a
 *   WARNING before the end of WARM and of COLD.
 *   A cue is a voice clip when profile.voice is on and its clip length is not
 *   0; otherwise it is a beep (voice off, or the file is missing). A beep has
 *   length 0.
 *   TO_COLD and DONE end exactly on the phase change: the cue is issued its
 *   own length before the change. If the clip is longer than the phase, the
 *   cue becomes a beep at the change.
 *   WARNING is issued cfg.warning_lead_ms before the change. It is skipped
 *   unless the phase is longer than warning_lead_ms plus the warning's
 *   length, and skipped if warning length plus the following cue's length
 *   exceed warning_lead_ms (the two would overlap).
 *   The cues of a phase are planned when the phase begins.
 *   A cue is issued at the first call at or after its time. take_cue()
 *   returns it once; a newer cue replaces one that was not taken. The caller
 *   starts the clip or beep at once (a clip that is still playing is
 *   replaced).
 *
 * Scenes
 *   One scene command when a timed phase begins: WARM, COLD, AFTER. The
 *   command for the first timed phase is issued at start(), so the lights
 *   answer the button press during the countdown. Taken once; a newer command
 *   replaces one that was not taken.
 *
 * Music (outputs.music)
 *   HYPE during COLD if profile.music, CALM during AFTER, otherwise NONE.
 *
 * next()   COUNTDOWN -> first timed phase, WARM -> COLD, COLD -> AFTER or
 *          end, AFTER -> end; all immediately. The cue that belongs to the
 *          skipped change (GO, TO_COLD, DONE) is issued as a beep; leaving
 *          AFTER has no cue. Returns false when idle.
 * abort()  Ends the run at once with COLD_END_ABORTED and drops a cue or
 *          scene command that was not taken. Returns false when idle.
 *
 * outputs.remaining_ms is the time until the current phase ends (0 when
 * idle).
 */
#ifndef COLD_PROTOCOL_H
#define COLD_PROTOCOL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    COLD_IDLE = 0,
    COLD_COUNTDOWN,
    COLD_WARM,
    COLD_COLD,
    COLD_AFTER,
} cold_phase_t;

typedef enum {
    COLD_MUSIC_NONE = 0,
    COLD_MUSIC_HYPE, /* folder B */
    COLD_MUSIC_CALM, /* folder C */
} cold_music_t;

typedef enum {
    COLD_SCENE_WARM = 0,
    COLD_SCENE_COLD,
    COLD_SCENE_AFTER,
} cold_scene_t;

typedef enum {
    COLD_CUE_GO = 0,  /* go.wav           "3-2-1 GO"            */
    COLD_CUE_WARNING, /* 20s_left.wav     "20 seconds left"     */
    COLD_CUE_TO_COLD, /* turn_to_cold.wav "3-2-1 turn to cold"  */
    COLD_CUE_DONE,    /* done.wav         "3-2-1 Done"          */
    COLD_CUE_COUNT,
} cold_cue_id_t;

typedef enum {
    COLD_END_FINISHED = 0,
    COLD_END_ABORTED,
} cold_end_t;

typedef struct {
    uint16_t warm_s;  /* 0 ... 1800, 0 skips the warm phase   default 120  */
    uint16_t cold_s;  /* 15 ... 600                           default 90   */
    uint16_t after_s; /* 30 ... 1800                          default 300  */
    bool voice;       /* voice clips; off = beeps             default true */
    bool music;       /* hype music during COLD               default true */
    bool calm;        /* AFTER phase with calm music          default true */
} cold_profile_t;

typedef struct {
    uint32_t warning_lead_ms; /* 5000 ... 60000   default 20000 */
} cold_config_t;

typedef struct {
    cold_cue_id_t id;
    bool beep; /* false: play the clip; true: play a short beep */
} cold_cue_t;

typedef struct {
    cold_phase_t phase;
    cold_music_t music;
    uint32_t remaining_ms;
} cold_outputs_t;

typedef struct cold_protocol cold_protocol_t;

void cold_config_default(cold_config_t *cfg);
void cold_profile_default(cold_profile_t *profile);

/* True if every value is inside the ranges above. NULL is invalid. */
bool cold_profile_valid(const cold_profile_t *profile);

/* Returns NULL if cfg is NULL or warning_lead_ms is out of range. */
cold_protocol_t *cold_protocol_create(const cold_config_t *cfg, uint32_t now_ms);

/* Accepts NULL. */
void cold_protocol_destroy(cold_protocol_t *cp);

/* clip_ms[id] is the length of each voice clip in milliseconds, 0 for a
 * missing file; NULL means no clips at all. Returns false and changes nothing
 * if a run is in progress or the profile is invalid. Events of an earlier run
 * that were not taken are dropped. */
bool cold_protocol_start(cold_protocol_t *cp, const cold_profile_t *profile,
                         const uint32_t clip_ms[COLD_CUE_COUNT], uint32_t now_ms);

/* Returns false and changes nothing when idle or the profile is invalid. */
bool cold_protocol_set_profile(cold_protocol_t *cp, const cold_profile_t *profile);

bool cold_protocol_next(cold_protocol_t *cp, uint32_t now_ms);
bool cold_protocol_abort(cold_protocol_t *cp, uint32_t now_ms);

/* Call regularly (e.g. every 20 ms). */
void cold_protocol_tick(cold_protocol_t *cp, uint32_t now_ms);

/* Current outputs. The pointer stays valid for the object's lifetime. */
const cold_outputs_t *cold_protocol_outputs(const cold_protocol_t *cp);

/* Each returns true once per event and copies it to out. */
bool cold_protocol_take_cue(cold_protocol_t *cp, cold_cue_t *out);
bool cold_protocol_take_scene(cold_protocol_t *cp, cold_scene_t *out);
bool cold_protocol_take_end(cold_protocol_t *cp, cold_end_t *out);

#ifdef __cplusplus
}
#endif

#endif /* COLD_PROTOCOL_H */
