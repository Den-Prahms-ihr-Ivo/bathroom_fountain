/*
 * speaker_session – what the bedroom speaker plays, decided from LINK_AUDIO,
 * the alarm and standby (pure C, no hardware).
 *
 * Requirements: BED-44 ... BED-48, BED-52 (see the requirements doc).
 * Tests:        test/test_speaker_session/
 *
 * The bedside owns all timers; the speaker only plays, fades and stops.
 * speaker_task feeds every LINK_AUDIO, the alarm state (armed_alarm) and
 * standby into this module, carries out its actions in order, and sets the
 * amplifier gain every tick:
 *   gain = volume_gain(step) * fade_curve_gain(speaker_session_level(now))
 * (volume_gain of a lib/volume object with the same config as the bedside's
 * mode_volume, set to outputs.volume).
 *
 * States (outputs.state)
 *   SPK_IDLE        nothing plays, amplifier may be off.
 *   SPK_PLAYING     a session plays (fading in or at full level).
 *   SPK_FADING_OUT  a STOP's fade runs; at its end: HALT, IDLE.
 *   SPK_ALARM       the alarm owns the speaker (armed_alarm / player);
 *                   every LINK_AUDIO except VOLUME is ignored.
 *
 * Actions (speaker_session_take(), oldest first, at most SPK_ACTS; the
 * oldest is dropped when full)
 *   START(folder, resume)  open the folder: resume = try lib/resume_restore,
 *                  else (or if that fails) a fresh shuffle. Replaces whatever
 *                  the player was playing.
 *   SAVE           write the resume point of the session playing *now*
 *                  (lib/resume_restore resume_capture). Only for kept
 *                  sessions, always before the START / HALT that ends it.
 *   HALT           stop decoding at once.
 *   NEXT           next track of the session.
 *
 * Level: a linear 0 ... 1 ramp. A START ramps 0 -> 1 over the PLAY's fade;
 * a STOP ramps from the current level to 0 over its fade; a fade cancel
 * (below) ramps from the current level to 1 over the PLAY's fade. fade 0 =
 * jump. IDLE and ALARM: level 0.
 *
 * speaker_session_audio(msg, now) – returns true if the message did
 * something:
 *   PLAY (folder must pass cfg_folder_valid() and not be "", volume <= 32,
 *        else ignored)
 *     ALARM: ignored.
 *     RESUME flag, state PLAYING or FADING_OUT, folder == outputs.folder:
 *       fade cancel: state PLAYING, ramp to 1 over fade_ds; volume and kept
 *       (= KEEP flag) taken from the message. No action.
 *     otherwise: SAVE first if the state is PLAYING and the session is kept
 *       (a FADING_OUT session was saved at its STOP); then START(folder,
 *       RESUME flag). State PLAYING, outputs.folder = folder, kept = KEEP
 *       flag, volume = message volume, ramp 0 -> 1.
 *   STOP
 *     PLAYING: SAVE if kept. fade 0: HALT, IDLE. Else FADING_OUT, ramp to 0.
 *     FADING_OUT: a new ramp from the current level over the new fade
 *       (fade 0: HALT, IDLE at once). No SAVE.
 *     IDLE, ALARM: ignored.
 *   VOLUME (volume <= 32, else ignored): outputs.volume = volume, in every
 *     state (also IDLE and ALARM: it applies to what comes next).
 *   NEXT: PLAYING or FADING_OUT: NEXT. Else ignored.
 *   other cmd values, NULL: ignored.
 *   In IDLE / ALARM, outputs.folder is "" and kept is false.
 *
 * speaker_session_ring(ringing, now)
 *   false -> true: SAVE if PLAYING and kept; HALT if PLAYING or FADING_OUT;
 *     state ALARM. true -> false: ALARM -> IDLE. Same value again: nothing.
 * speaker_session_standby(now) (long press on the speaker's button)
 *   PLAYING: SAVE if kept, HALT, IDLE. FADING_OUT: HALT, IDLE. Returns true.
 *   IDLE, ALARM: nothing, false.
 * speaker_session_ended(now) (the player found nothing to play: empty or
 *   missing folder, card gone)
 *   PLAYING / FADING_OUT: IDLE, no action (nothing to save or halt). true.
 *   Else false.
 * speaker_session_tick(now): a fade-out that has run its course: HALT, IDLE.
 *   Every function taking now first does the same.
 *
 * outputs.amp_on: true in PLAYING and FADING_OUT.
 * Times: wrapping uint32 ms; fades up to 6553.5 s.
 */
#ifndef SPEAKER_SESSION_H
#define SPEAKER_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "link_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SPK_ACTS 8

typedef enum {
    SPK_IDLE = 0,
    SPK_PLAYING,
    SPK_FADING_OUT,
    SPK_ALARM,
} spk_state_t;

typedef enum {
    SPK_ACT_START = 1,
    SPK_ACT_SAVE,
    SPK_ACT_HALT,
    SPK_ACT_NEXT,
} spk_act_type_t;

typedef struct {
    spk_act_type_t type;
    bool resume;                       /* START */
    char folder[LINK_FOLDER_LEN + 1];  /* START; "" otherwise */
} spk_act_t;

typedef struct {
    spk_state_t state;
    char folder[LINK_FOLDER_LEN + 1];
    bool kept;
    uint8_t volume; /* step 0 ... 32 */
    bool amp_on;
} spk_outputs_t;

typedef struct speaker_session speaker_session_t;

/* Starts IDLE, level 0, no action. initial_volume above 32 is taken as 32.
 * NULL only when memory runs out. */
speaker_session_t *speaker_session_create(uint8_t initial_volume, uint32_t now_ms);

/* Accepts NULL. */
void speaker_session_destroy(speaker_session_t *s);

bool speaker_session_audio(speaker_session_t *s, const link_audio_t *msg, uint32_t now_ms);
void speaker_session_ring(speaker_session_t *s, bool ringing, uint32_t now_ms);
bool speaker_session_standby(speaker_session_t *s, uint32_t now_ms);
bool speaker_session_ended(speaker_session_t *s, uint32_t now_ms);
void speaker_session_tick(speaker_session_t *s, uint32_t now_ms);

/* Fade level 0 ... 1 at now (does not change the state; call tick for
 * that). 0 for NULL. */
float speaker_session_level(const speaker_session_t *s, uint32_t now_ms);

/* Valid for the object's lifetime; NULL for NULL. */
const spk_outputs_t *speaker_session_outputs(const speaker_session_t *s);

/* false if nothing waits or an argument is NULL. */
bool speaker_session_take(speaker_session_t *s, spk_act_t *out);

#ifdef __cplusplus
}
#endif

#endif /* SPEAKER_SESSION_H */
