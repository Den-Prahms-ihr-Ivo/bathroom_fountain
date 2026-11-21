/*
 * speaker_task – the bedroom speaker's control loop.
 *
 * Every 20 ms:
 *   link      LINK_AUDIO from the bedside -> speaker_session_audio();
 *             LINK_HELLO from the bedside -> send the folder list (LINK_FOLDER);
 *             the list is sent again when the card's folders change (sync).
 *   actions   speaker_session_take():
 *               START(folder, resume) -> player_play_folder(folder, kept,
 *                        resume ? the folder's resume point from NVS : NULL)
 *               SAVE  -> player_position() -> resume_capture -> NVS under
 *                        resume_key(folder) (lib/resume_restore)
 *               HALT  -> player_play(false)
 *               NEXT  -> player_next()
 *             player_take_ended() (nothing playable) -> speaker_session_ended()
 *   amplifier on while outputs.amp_on; gain = volume_gain(outputs.volume) *
 *             fade_curve_gain(speaker_session_level(now)); faults polled.
 *   button    long press: pairing window open -> enroll; else standby.
 *
 * Not yet (docs/ARCHITECTURE.md, Open items): the alarm (armed_alarm, the
 * alarm sound, speaker_session_ring, RING / RING_STATE frames; the button
 * stopping a ringing alarm first).
 */
#ifndef SPEAKER_TASK_H
#define SPEAKER_TASK_H

#include <stdbool.h>
#include <stdint.h>

#include "link_msg.h"
#include "status_json.h"

#ifdef __cplusplus
extern "C" {
#endif

void speaker_task_start(void);

typedef struct {
    status_playing_t playing;
    char folder[LINK_FOLDER_LEN + 1];
    bool kept;
    uint8_t volume;
    bool link_up;
} speaker_status_t;

/* Snapshot for the status document (any task). */
void speaker_task_status(speaker_status_t *out);

#ifdef __cplusplus
}
#endif

#endif /* SPEAKER_TASK_H */
