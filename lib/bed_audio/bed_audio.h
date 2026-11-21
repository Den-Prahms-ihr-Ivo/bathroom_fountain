/*
 * bed_audio – the bedside's glue between lib/bed_modes and the speaker:
 * commands to LINK_AUDIO messages, scene ids, volume slots and their NVS
 * keys, and the mood folder (pure C, no radio).
 *
 * Requirements: BED-41 ... BED-45, BED-50, AUD-13, AUD-14 (see the
 * requirements doc).
 * Tests:        test/test_bed_audio/
 *
 * Messages (bed_audio_from_cmd)
 *   PLAY  cmd LINK_AUDIO_PLAY; flags RESUME / KEEP from the command; volume
 *         ctx->volume[slot of the mode]; folder the mode's folder from ctx;
 *         fade_ds from fade_ms.
 *   STOP  cmd LINK_AUDIO_STOP, fade_ds.     NEXT  cmd LINK_AUDIO_NEXT.
 *   Every unused byte is 0 (also the folder's padding). fade_ds =
 *   fade_ms / 100 rounded half up, at most 65535.
 *   Returns false (out untouched) for NULL arguments, SCENE,
 *   MOOD_FOLDER_NEXT and unknown commands (not speaker messages), a PLAY of
 *   BED_MODE_NONE or an unknown mode, a folder that is NULL, "" (no music)
 *   or not cfg_folder_valid(), or a volume above BED_AUDIO_VOLUME_MAX.
 *
 * bed_audio_volume_msg(step): LINK_AUDIO_VOLUME with that step, other bytes
 *   0; false (out untouched) for step > BED_AUDIO_VOLUME_MAX or out NULL.
 *
 * Volume slots (lib/mode_volume): meditation, mood, sleep; keys vol_med,
 *   vol_mood, vol_sleep. bed_audio_volume_index() gives MODE_VOLUME_NONE for
 *   BED_MODE_NONE and unknown modes; bed_audio_volume_key() NULL out of range.
 *
 * Scenes (bed_audio_scene): the Hue scene id to recall, or NULL to leave the
 *   lights alone. NORMAL is `normal_scene` (scene_normal of lib/settings;
 *   bed may be NULL); MEDITATION / MOOD / SLEEP are bed->scene_med /
 *   scene_mood / scene_sleep. "" or NULL gives NULL, and so does an unknown
 *   scene.
 *
 * Mood folder (BED-43). Both write the folder to out and return true; on
 *   false out is "" (if out_size > 0). NULL list, current or out: false.
 *   bed_audio_pick_mood(list, current): the folder to play before the first
 *     PLAY. List not ready yet: current if not "", else false. List ready:
 *     current if it is on the card, else bed_next_folder(..., current,
 *     "mood_") over the list (false if the card has no "mood_" folder).
 *   bed_audio_next_mood(list, current): the mood button pressed again:
 *     bed_next_folder(..., current, "mood_") over the list; false while the
 *     list is not ready or without "mood_" folders.
 *   Also false if out_size is too small for the name.
 */
#ifndef BED_AUDIO_H
#define BED_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bed_modes.h"
#include "folder_list.h"
#include "link_msg.h"
#include "mode_volume.h"
#include "node_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BED_AUDIO_VOLUME_MAX 32
#define BED_AUDIO_MOOD_PREFIX "mood_"

enum {
    BED_VOL_MEDITATION = 0,
    BED_VOL_MOOD = 1,
    BED_VOL_SLEEP = 2,
    BED_VOL_COUNT = 3,
};

typedef struct {
    const char *med_folder;
    const char *mood_folder;
    const char *sleep_folder;
    uint8_t volume[BED_VOL_COUNT]; /* mode_volume_step() of each slot */
} bed_audio_ctx_t;

bool bed_audio_from_cmd(const bed_cmd_t *cmd, const bed_audio_ctx_t *ctx, link_audio_t *out);

bool bed_audio_volume_msg(uint8_t step, link_audio_t *out);

int bed_audio_volume_index(bed_mode_t mode);

const char *bed_audio_volume_key(int slot);

const char *bed_audio_scene(bed_scene_t scene, const cfg_bed_t *bed, const char *normal_scene);

bool bed_audio_pick_mood(const folder_list_t *list, const char *current, char *out, size_t out_size);

bool bed_audio_next_mood(const folder_list_t *list, const char *current, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* BED_AUDIO_H */
