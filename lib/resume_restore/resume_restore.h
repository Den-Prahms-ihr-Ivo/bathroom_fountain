/*
 * resume_restore – the speaker's two steps around lib/resume_point:
 * capture where a kept session is, and find that place again
 * (pure C over lib/playlist).
 *
 * Requirements: BED-45, BED-46 (see the requirements doc).
 * Tests:        test/test_resume_restore/
 *
 * Rule for the player: a KEEP session (LINK_AUDIO_FLAG_KEEP) always gets a
 * playlist of its own, created with shuffle on and a new seed
 * (esp_random()), filled from the folder and finalized once. The player
 * counts its playlist_next() calls (index; the playing track is the
 * index-th). playlist_mark_bad() during the session is allowed, but it
 * changes the order, so a later restore may fall back to a fresh shuffle.
 *
 * Capture (on speaker_session's SAVE): resume_capture() with folder, seed,
 * index, the playing track's name and the byte offset the decoder has read
 * up to; then resume_encode() and NVS under resume_key(folder). Every
 * folder has its own resume point (sleep_rain and sleep_true_crime each
 * continue where they stopped).
 *
 * Restore (on START with resume): resume_decode() the blob under
 * resume_key(folder) (missing: fresh shuffle), create the
 * playlist with cfg.seed = rp.seed and shuffle on, fill it from the folder,
 * finalize, then resume_restore(). Non-NULL: open that track at *offset and
 * continue counting from rp.index. NULL: start a fresh shuffle (new seed,
 * new playlist).
 */
#ifndef RESUME_RESTORE_H
#define RESUME_RESTORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "playlist.h"
#include "resume_point.h"

#ifdef __cplusplus
extern "C" {
#endif

/* NVS key of a folder's resume point: "rp" + the 32-bit FNV-1a hash of the
 * folder name's bytes (offset basis 0x811c9dc5, prime 0x01000193) as 8
 * lower-case hex digits, e.g. "sleep_rain" -> "rp931cbb23" (10 characters,
 * within NVS's 15). Two folders may share a key; resume_restore() then sees
 * the other folder's name and starts fresh, and the next save takes the key
 * over. false (out = "" if out_size > 0) if folder is NULL or "", out is
 * NULL or out_size < 11. */
bool resume_key(const char *folder, char *out, size_t out_size);

/* Longest replay resume_restore() does; a larger index is refused. */
#define RESUME_REPLAY_MAX 65536u

/* Fills *rp. false (rp untouched) if rp, folder or track is NULL, the
 * result would not be valid for resume_encode() (lengths, characters,
 * index 0). */
bool resume_capture(resume_point_t *rp, const char *folder, uint32_t seed, uint32_t index, const char *track,
                    uint32_t offset);

/*
 * pl: built as described above for rp. Calls playlist_next() rp->index
 * times. If folder equals rp->folder (exact) and the last name equals
 * rp->track (exact), returns that name (the playlist's string) and sets
 * *offset_out = rp->offset. Otherwise returns NULL and leaves *offset_out
 * alone; the playlist may have been advanced.
 * Returns NULL without calling playlist_next() if an argument is NULL, the
 * folder differs, rp->index is 0 or above RESUME_REPLAY_MAX, or
 * playlist_count(pl) is 0. Stops early (NULL) if playlist_next() returns
 * NULL.
 */
const char *resume_restore(playlist_t *pl, const resume_point_t *rp, const char *folder, uint32_t *offset_out);

#ifdef __cplusplus
}
#endif

#endif /* RESUME_RESTORE_H */
