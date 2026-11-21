/*
 * resume_restore – capture where a kept session stopped, and find that spot
 * again in a rebuilt playlist (pure C, no file system).
 *
 * Requirements: BED-45, BED-46 (see the requirements doc).
 * Tests:        test/test_resume_restore/
 *
 * Builds on lib/playlist and lib/resume_point (blob layout and validity).
 *
 *   resume_capture(out, folder, seed, index, track, offset)
 *     Fills *out. Returns false (out untouched) if any pointer is NULL or the
 *     result would not be a valid resume point (resume_point.h: folder 1-23
 *     and track 1-95 printable ASCII characters without '/', index >= 1).
 *     A captured point always encodes with resume_encode().
 *
 *   resume_restore(pl, rp, folder, offset_out)
 *     pl: a freshly finalized playlist of `folder`, built with rp->seed
 *     (nothing taken from it yet). Calls playlist_next() rp->index times;
 *     if the last name equals rp->track, sets *offset_out = rp->offset and
 *     returns that name (playlist storage). The playlist then continues in
 *     the original order.
 *     Returns NULL, offset untouched, without touching the playlist, if an
 *     argument is NULL, folder differs from rp->folder (exact match),
 *     rp->index is 0 or above RESUME_REPLAY_MAX.
 *     Returns NULL, offset untouched, if the playlist runs dry or the name
 *     does not match (the card changed): the playlist has been advanced,
 *     so the caller calls playlist_finalize() for a fresh start.
 */
#ifndef RESUME_RESTORE_H
#define RESUME_RESTORE_H

#include <stdbool.h>
#include <stdint.h>

#include "playlist.h"
#include "resume_point.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Most playlist_next() calls a restore replays (bounds the work). */
#define RESUME_REPLAY_MAX 4096u

bool resume_capture(resume_point_t *out, const char *folder, uint32_t seed, uint32_t index, const char *track,
                    uint32_t offset);

const char *resume_restore(playlist_t *pl, const resume_point_t *rp, const char *folder, uint32_t *offset_out);

#ifdef __cplusplus
}
#endif

#endif /* RESUME_RESTORE_H */
