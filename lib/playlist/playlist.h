/*
 * playlist – which file plays next (pure C, no file system).
 *
 * Requirements: AUD-02, AUD-03, AUD-09, AUD-12 (see the requirements doc).
 * Tests:        test/test_playlist/
 *
 * The player task scans the music folder and hands every directory entry to
 * playlist_add(), then calls playlist_finalize() and asks playlist_next()
 * for each track. Before a rescan (e.g. after the card was swapped) it calls
 * playlist_clear().
 *
 * Accepted entries
 *   - not a directory
 *   - name does not start with '.' (hidden files, macOS "._x.mp3" forks)
 *   - name ends in ".mp3", any case, with at least one character before it
 *   - strlen(name) < cfg.max_name_len (longer names are skipped, never
 *     truncated: a truncated name could not be opened)
 *   - fewer than cfg.max_tracks entries accepted so far
 * Checks are applied in this order; the first that fails gives the result.
 *
 * Order (finalize)
 *   Natural order: compare case-insensitively (ASCII), but where both names
 *   have a run of ASCII digits at the same position, the runs compare by
 *   numeric value ("track2" < "track10"; "01" == "1" by value). Runs can be
 *   longer than any integer type. A digit facing a non-digit compares as its
 *   character. Names that are equal under this rule are ordered by strcmp().
 *   The playlist keeps its own copy of each name.
 *
 * Playback
 *   Sequential (default): natural order, wrapping round to the first track
 *   after the last one (AUD-02 "loops").
 *   Shuffle (AUD-03): playback runs in rounds. Every round plays each
 *   playable track exactly once, in a random order. With two or more
 *   playable tracks, the first track of a round is never the last track of
 *   the previous round. The order is a deterministic function of cfg.seed.
 *
 * Unplayable files (AUD-12)
 *   playlist_mark_bad() removes a track for the rest of the session (until
 *   clear). If no playable track is left, playlist_next() returns NULL and
 *   the player reports "no music" (AUD-09) without blocking the rest of
 *   Ambient mode.
 *
 * Strings returned by playlist_next() stay valid until clear or destroy.
 */
#ifndef PLAYLIST_H
#define PLAYLIST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t max_tracks;   /* 1 … 1024,                  default 256 */
    uint16_t max_name_len; /* incl. NUL, 8 … 256,         default 96  */
    bool shuffle;          /*                            default false */
    uint32_t seed;         /* any value incl. 0,          default 1   */
} playlist_config_t;

typedef enum {
    PLAYLIST_ADDED = 0,
    PLAYLIST_SKIPPED_DIR,
    PLAYLIST_SKIPPED_HIDDEN,
    PLAYLIST_SKIPPED_NOT_AUDIO,
    PLAYLIST_SKIPPED_TOO_LONG,
    PLAYLIST_SKIPPED_FULL,
    PLAYLIST_ERR_FINALIZED, /* add after finalize without clear: nothing changes */
} playlist_add_result_t;

typedef struct playlist playlist_t;

void playlist_config_default(playlist_config_t *cfg);

/* Returns NULL if cfg is NULL, out of range, or memory runs out. Allocates
 * all storage here (max_tracks * max_name_len bytes plus bookkeeping). */
playlist_t *playlist_create(const playlist_config_t *cfg);

/* Accepts NULL. */
void playlist_destroy(playlist_t *pl);

/* Forget all tracks and bad marks; ready for a new scan. */
void playlist_clear(playlist_t *pl);

/* name must not be NULL. */
playlist_add_result_t playlist_add(playlist_t *pl, const char *name, bool is_dir);

/* Sort and start playback from the beginning (or a fresh shuffle round). */
void playlist_finalize(playlist_t *pl);

/* Number of playable tracks (accepted minus bad). */
size_t playlist_count(const playlist_t *pl);

/* Next track name, or NULL before finalize or when nothing is playable. */
const char *playlist_next(playlist_t *pl);

/* Mark a track as unplayable. Unknown names are ignored. */
void playlist_mark_bad(playlist_t *pl, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* PLAYLIST_H */
