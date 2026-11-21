/*
 * music_sync – cmd/music/sync (docs/PROTOCOL.md, Commands and HTTPS):
 * pull missing or changed files from the server onto the SD card, delete
 * files not listed, then publish an event "music".
 *
 * SKELETON: the contract and the busy flag are real, the download is a TODO.
 *
 * TODO (in this order):
 *   1. parse {"base","files":[{"path","size","sha256"}]} (cJSON, max ~200
 *      files; refuse paths with ".." or a leading '/', and folders
 *      cfg_folder_valid() refuses);
 *   2. for each file: stat on the SD card; same size -> sha256 of the file
 *      (mbedtls) -> skip if equal; otherwise GET base+path with HTTP basic
 *      auth (mqtt_user / mqtt_pass), Range: bytes=<have>- into "<path>.part",
 *      verify sha256, rename;
 *   3. delete files that are not listed (only *.mp3 and *.part);
 *   4. player_rescan(); event {"type":"music","ok":..,"files":..,"bytes":..};
 *   5. stop the player while a file of the playing folder is replaced.
 */
#ifndef MUSIC_SYNC_H
#define MUSIC_SYNC_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start a sync with the command's payload (copied). false if one runs. */
bool music_sync_start(const char *payload, size_t len);

/* A sync is running (crate: crate_controller_busy keeps the closed crate awake). */
bool music_sync_running(void);

#ifdef __cplusplus
}
#endif

#endif /* MUSIC_SYNC_H */
