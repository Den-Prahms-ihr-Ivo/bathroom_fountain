/*
 * player_task – microSD → playlist → MP3 decoder → pcm_mix → I2S0 → TAS5825M
 * (AUD-01, AUD-02, AUD-03, AUD-09, AUD-12).
 *
 *   card    FAT32, MP3 files in the root folder; mounted at boot and again on
 *           every start when no track is known (so a swapped card is found)
 *   order   shuffled rounds (AUD-03), seed from the hardware RNG; a new
 *           Ambient session continues the current round
 *   output  mono on the left channel (one speaker), right channel silent;
 *           I2S runs all the time and sends zeros when idle, because the
 *           amplifier needs its clocks before it is powered up
 *   errors  a file that does not decode is skipped for the session (AUD-12);
 *           no card or no tracks never blocks the rest of Ambient (AUD-09)
 *
 * Volume and fades are not done here: control_task sets the amplifier's
 * digital volume.
 *
 * Boards without amplifier pins (the DevKit) play "dry": files are decoded
 * and paced in real time, but nothing is output. That checks card, playlist
 * and decoder on the bench.
 */
#ifndef PLAYER_TASK_H
#define PLAYER_TASK_H

#include <stdbool.h>

void player_task_start(void);

/* Start or stop playback. Never blocks. */
void player_play(bool on);

/* Skip to the next track (console "next"). */
void player_next(void);

/* Unmount and scan the card again (console "rescan"). */
void player_rescan(void);

#endif /* PLAYER_TASK_H */
