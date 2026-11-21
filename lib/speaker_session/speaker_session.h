/*
 * speaker_session – the bedroom speaker's music session: LINK_AUDIO from the
 * bedside, the alarm and standby turned into start / save / halt / next
 * actions for the player, plus the fade level (pure C, no audio).
 *
 * Requirements: BED-44 ... BED-48, BED-52 (see the requirements doc).
 * Tests:        test/test_speaker_session/
 *
 * Time: now_ms is a wrapping uint32 millisecond counter. Every call that
 * takes now_ms first brings the state up to date (a fade-out that ended
 * since the last call halts first, at its planned time).
 *
 * States (outputs.state)
 *   SPK_IDLE        nothing plays, amplifier off.
 *   SPK_PLAYING     a session plays (also while it fades in).
 *   SPK_FADING_OUT  a STOP's fade-out runs; HALT when it is over.
 *   SPK_ALARM       the alarm rings; the session is gone, LINK_AUDIO is
 *                   ignored except VOLUME.
 *   outputs.folder / kept describe the session ("" / false without one);
 *   amp_on is true in PLAYING and FADING_OUT. outputs.volume is the last
 *   volume step received (create's start value until then).
 *
 * Actions (speaker_session_take(), oldest first; at most SPK_ACTS wait, the
 * oldest is dropped when full). folder is "" except for START.
 *   SPK_ACT_START  play `folder` from a fresh shuffle, or (resume) from its
 *                  resume point if there is one.
 *   SPK_ACT_SAVE   capture the resume point of the running kept session now
 *                  (before any fade).
 *   SPK_ACT_HALT   stop the music now and power the amplifier down.
 *   SPK_ACT_NEXT   next track.
 *
 * LINK_AUDIO (speaker_session_audio(); true if it did something)
 *   PLAY   refused (false, nothing changes) while ringing, for a folder
 *          that is "" or not cfg_folder_valid(), or a volume above 32.
 *          RESUME of the folder that plays or fades out: plays on, no
 *          action; the fade is cancelled and the level rises from where it
 *          is to 1 over fade_ds. Otherwise: SAVE first if a kept session is
 *          PLAYING (one that fades out was saved at its STOP), then
 *          START(folder, resume); the level rises from 0 to 1 over fade_ds.
 *          Both set volume and kept (from the KEEP flag).
 *   STOP   IDLE or ringing: false. PLAYING: SAVE if kept, then the level
 *          falls from where it is to 0 over fade_ds. FADING_OUT: the fade
 *          restarts from the current level (no second SAVE). With fade_ds 0
 *          the HALT comes at once.
 *   NEXT   NEXT action while PLAYING or FADING_OUT; otherwise false.
 *   VOLUME any state (also ringing); false above 32. The fade is untouched.
 *   Unknown commands: false.
 *
 * speaker_session_ring(ringing): false -> true ends the session: SAVE if a
 *   kept session was PLAYING, HALT if anything played; state ALARM.
 *   true -> false: IDLE. Repeating the same value changes nothing.
 * speaker_session_standby(): the speaker goes to standby. Like a STOP
 *   without fade (SAVE if a kept session was PLAYING, HALT); false if idle
 *   or ringing.
 * speaker_session_ended(): the player ran out of music. IDLE without any
 *   action; false if idle or ringing.
 *
 * speaker_session_level(now): fade level 0.0 ... 1.0 (linear in time),
 *   0 in IDLE and ALARM. The player multiplies it with the volume gain.
 */
#ifndef SPEAKER_SESSION_H
#define SPEAKER_SESSION_H

#include <stdbool.h>
#include <stdint.h>

#include "link_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SPK_ACTS 8
#define SPK_VOLUME_MAX 32

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
    char folder[LINK_FOLDER_LEN + 1]; /* START */
    bool resume;                      /* START */
} spk_act_t;

typedef struct {
    spk_state_t state;
    char folder[LINK_FOLDER_LEN + 1];
    bool kept;
    bool amp_on;
    uint8_t volume;
} spk_outputs_t;

typedef struct speaker_session speaker_session_t;

/* volume: the start volume step, clamped to 32. NULL if memory runs out. */
speaker_session_t *speaker_session_create(uint8_t volume, uint32_t now_ms);

/* Accepts NULL. */
void speaker_session_destroy(speaker_session_t *s);

/* false for NULL arguments. */
bool speaker_session_audio(speaker_session_t *s, const link_audio_t *m, uint32_t now_ms);

void speaker_session_ring(speaker_session_t *s, bool ringing, uint32_t now_ms);

bool speaker_session_standby(speaker_session_t *s, uint32_t now_ms);

bool speaker_session_ended(speaker_session_t *s, uint32_t now_ms);

/* Call regularly (e.g. every 100 ms). */
void speaker_session_tick(speaker_session_t *s, uint32_t now_ms);

/* NULL for s NULL. The pointer stays valid for the object's lifetime. */
const spk_outputs_t *speaker_session_outputs(const speaker_session_t *s);

/* 0 for s NULL. Does not change the state. */
float speaker_session_level(const speaker_session_t *s, uint32_t now_ms);

/* false for NULL arguments or an empty queue. */
bool speaker_session_take(speaker_session_t *s, spk_act_t *out);

#ifdef __cplusplus
}
#endif

#endif /* SPEAKER_SESSION_H */
