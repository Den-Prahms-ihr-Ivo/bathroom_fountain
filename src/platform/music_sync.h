/*
 * music_sync – cmd/music/sync (docs/PROTOCOL.md, Commands and HTTPS): make
 * the SD card match the server's music library.
 *
 *   1. lib/sync_manifest parses the file list (invalid entries are skipped
 *      and logged).
 *   2. For every listed file: same size and same SHA-256 on the card -> kept.
 *      Otherwise it is downloaded from base + path with HTTP basic auth
 *      (mqtt_user / mqtt_pass) into "<path>.part"; an existing .part is
 *      continued with a Range request. The finished file is verified
 *      (size, SHA-256) and renamed into place. Two tries per file.
 *   3. Audio files that are not listed are deleted: names ending in .mp3,
 *      .wav or .part, in the card's root and in its root folders. Nothing
 *      else on the card is ever touched.
 *   4. Event {"type":"music","have":..,"missing":..,"free_mb":..}.
 *
 * The player keeps playing while files are downloaded. Before the first
 * file is replaced or deleted it is held (player_hold), and released at the
 * end, which rescans the card and continues the music (a new track).
 *
 * One sync at a time; a command during a sync is refused (event cmd,
 * why "busy").
 */
#ifndef MUSIC_SYNC_H
#define MUSIC_SYNC_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start a sync with the command's payload (copied). false if one runs or
 * there is no memory. */
bool music_sync_start(const char *payload, size_t len);

/* A sync is running (crate: crate_controller_busy keeps the closed crate awake). */
bool music_sync_running(void);

#ifdef __cplusplus
}
#endif

#endif /* MUSIC_SYNC_H */
