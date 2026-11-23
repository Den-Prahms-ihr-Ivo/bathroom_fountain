/*
 * player_task – microSD → playlist → MP3 decoder → pcm_mix → voice_mix →
 * I2S0 → TAS5825M (AUD-01, AUD-02, AUD-03, AUD-09, AUD-12, COLD-09 ...
 * COLD-12, BED-45, BED-46).
 *
 *   card    FAT32. Music in root folders ("A", "sleep_rain", ...) or in the
 *           root itself (folder ""). Mounted at boot and again whenever a
 *           start finds no track (so a swapped card is found).
 *   folders player_play() plays the default folder (player_set_default_folder,
 *           "" = root; a default folder without tracks falls back to the
 *           root, so a v1 card with MP3s in the root keeps working).
 *           player_play_folder() plays any folder.
 *   order   shuffled rounds (AUD-03). The default folder keeps one playlist,
 *           so a new Ambient session continues the round. A KEEP session
 *           (speaker) gets a playlist of its own with a new seed, and can be
 *           resumed from a resume_point_t (lib/resume_restore).
 *   voice   up to PLAYER_VOICE_MAX mono 16-bit WAV clips (paths set by the
 *           app, read at every scan through lib/wav_clip) and a beep, mixed
 *           over the music with ducking (lib/voice_mix), also when no music
 *           plays (silence underneath).
 *   output  mono on the left channel (one speaker), right channel silent;
 *           I2S runs all the time and sends zeros when idle, because the
 *           amplifier needs its clocks before it is powered up
 *   errors  a file that does not decode is skipped for the session (AUD-12);
 *           no card or no tracks never blocks the rest of Ambient (AUD-09)
 *
 * Volume and fades are not done here: the app sets the amplifier's digital
 * volume.
 *
 * Boards without amplifier pins (the DevKit) play "dry": files are decoded
 * and paced in real time, but nothing is output.
 *
 * Every function here may be called from any task and never blocks on the
 * card (requests go through a queue; status reads are snapshots).
 */
#ifndef PLAYER_TASK_H
#define PLAYER_TASK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "resume_point.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PLAYER_MOUNT_POINT  "/sdcard"
#define PLAYER_FOLDER_MAX   RESUME_FOLDER_MAX /* 23 characters */
#define PLAYER_MAX_FOLDERS  32
#define PLAYER_VOICE_MAX    4

void player_task_start(void);

/* ---- music ---------------------------------------------------------------- */

/* Start (default folder) or stop playback. Never blocks. */
void player_play(bool on);

/* The folder player_play(true) plays. Call before player_task_start(), or
 * any time (takes effect at the next start). "" = root (default). */
void player_set_default_folder(const char *folder);

/* Play this folder now, replacing whatever plays ("" = root).
 *   keep = false: a shuffled playlist of the folder (the default folder's
 *                 playlist continues its round);
 *   keep = true:  a playlist of its own with a new random seed, so the place
 *                 can be captured with player_position();
 *   rp != NULL:   rebuild rp's playlist and continue at its track and byte
 *                 offset (resume_restore); if that track is not found, a
 *                 fresh keep playlist starts.
 * A folder without playable tracks raises the "ended" flag. */
void player_play_folder(const char *folder, bool keep, const resume_point_t *rp);

/* Shuffle with a known seed: rebuild the folder's playlist with this seed,
 * skip `played` tracks (the ones handed out before, as player_position()
 * reported them in seed / index) and play the one after, from its start.
 * played 0 = the first track of the round. The crate uses it so that every
 * opening plays the next track of one shuffle, also across deep sleep. A
 * card whose files changed simply gives a different order. */
void player_play_shuffle(const char *folder, uint32_t seed, uint32_t played);

/* Skip to the next track (console "next", bedside NEXT). */
void player_next(void);

/* Unmount and scan the card again (console "rescan", after a music sync). */
void player_rescan(void);

typedef struct {
    bool playing;                        /* a track is open and playing    */
    bool keep;                           /* a KEEP session                 */
    char folder[PLAYER_FOLDER_MAX + 1];
    char track[RESUME_TRACK_MAX + 1];    /* file name inside the folder    */
    uint32_t seed;
    uint32_t index;                      /* playlist_next() calls so far   */
    uint32_t offset;                     /* bytes the decoder has used     */
} player_pos_t;

/* What plays now (for resume_capture). Always fills *out. */
void player_position(player_pos_t *out);

/* true once after a start or a track change found nothing playable in the
 * folder (speaker: speaker_session_ended()). */
bool player_take_ended(void);

/* ---- card ------------------------------------------------------------------ */

bool player_sd_mounted(void);

/* Files counted at the last scan: MP3s in a root folder ("" = the root
 * itself); for the voice folder "V", WAVs. 0 for an unknown folder. */
uint16_t player_folder_tracks(const char *folder);

/* Names of the root folders at the last scan, sorted (strcmp). Returns how
 * many were written (at most max). */
size_t player_folders(char names[][PLAYER_FOLDER_MAX + 1], size_t max);

/* Free space on the card in MB, 0 if unknown. */
uint32_t player_sd_free_mb(void);

/* Music sync: hold = close every file and stay silent (the wanted state is
 * kept); release = scan the card again and continue. */
void player_hold(bool on);

/* ---- voice ----------------------------------------------------------------- */

/* Clip paths relative to the card's root, e.g. "V/go.wav". Read at the next
 * scan (call before player_task_start(), or then player_rescan()). */
void player_voice_set_clips(const char *const paths[], size_t count);

/* Length of each clip in ms, 0 when missing or not a usable WAV. */
void player_voice_lengths(uint32_t ms[PLAYER_VOICE_MAX]);

/* Play clip `index` (beep = false) or the beep. Replaces a clip that plays. */
void player_voice_cue(size_t index, bool beep);
void player_voice_stop(void);

/* ---- diagnostics ------------------------------------------------------------ */

/* While playing, replace the music with a 1 kHz test tone on both channels
 * (console "amp tone on|off"). */
void player_test_tone(bool on);

#ifdef __cplusplus
}
#endif

#endif /* PLAYER_TASK_H */
