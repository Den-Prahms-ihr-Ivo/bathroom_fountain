/*
 * speaker_task – the bedroom speaker's control loop (skeleton).
 *
 * Every 20 ms:
 *   link      LINK_AUDIO from the bedside -> speaker_session_audio();
 *             LINK_HELLO from the bedside -> send the folder list (LINK_FOLDER).
 *   actions   speaker_session_take():
 *               START(folder, resume) -> player (TODO: folder + resume API)
 *               SAVE  -> resume point of the playing track into NVS
 *                        (resume_key(folder), resume_encode) – TODO: the
 *                        player has to report seed / index / track / offset
 *               HALT  -> player_play(false)
 *               NEXT  -> player_next()
 *   amplifier on while outputs.amp_on; gain = volume_gain(outputs.volume) *
 *             fade_curve_gain(speaker_session_level(now)).
 *   button    long press: pairing window open -> enroll; else standby.
 *             (alarm ringing -> stop the alarm: TODO with armed_alarm.)
 *
 * TODO: armed_alarm / alarm sound (speaker_session_ring), RING / RING_STATE
 *       frames, status_json_speaker, speaker_session_ended() from the player.
 */
#ifndef SPEAKER_TASK_H
#define SPEAKER_TASK_H

#ifdef __cplusplus
extern "C" {
#endif

void speaker_task_start(void);

#ifdef __cplusplus
}
#endif

#endif /* SPEAKER_TASK_H */
