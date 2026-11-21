/*
 * bed_audio – the bedside's glue between lib/bed_modes and the rest: speaker
 * messages, volume slots, scene ids, mood folders (pure C, no hardware).
 *
 * Requirements: BED-41 ... BED-45, BED-50, BED-53, AUD-13, AUD-14.
 * Tests:        test/test_bed_audio/
 *
 * bed_task does, for every bed_modes command:
 *   SCENE            id = bed_audio_scene(cmd.scene, &cfg_bed, scene_normal);
 *                    NULL = leave the lights, else hue_task recalls it over
 *                    cmd.fade_ms.
 *   MOOD_FOLDER_NEXT bed_audio_next_folder(&folders, cfg_bed.mood_folder,
 *                    BED_PREFIX_MOOD, ...) and store the result as
 *                    mood_folder (local settings change, reported to the
 *                    server). False = keep the old one.
 *   SLEEP_FOLDER_NEXT the same with sleep_folder and BED_PREFIX_SLEEP.
 *   PLAY of MOOD / SLEEP  first bed_audio_pick_folder() with that prefix
 *                    (the stored folder may be "" or gone from the card),
 *                    store it if it changed.
 *   PLAY, STOP, NEXT bed_audio_from_cmd() -> LINK_AUDIO to the speaker.
 *   encoder          mode_volume_turn(bed_audio_volume_index(outputs.mode))
 *                    then bed_audio_volume_msg(new step).
 *   saving volumes   mode_volume_take_save() -> NVS key
 *                    bed_audio_volume_key(mode).
 */
#ifndef BED_AUDIO_H
#define BED_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bed_modes.h"
#include "folder_list.h"
#include "link_msg.h"
#include "node_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Volume slots in lib/mode_volume, in this order. */
#define BED_VOL_MEDITATION 0
#define BED_VOL_MOOD 1
#define BED_VOL_SLEEP 2
#define BED_VOL_COUNT 3

/* Folder families the mood and sleep buttons cycle through. */
#define BED_PREFIX_MOOD "mood_"
#define BED_PREFIX_SLEEP "sleep_"

typedef struct {
    const char *med_folder;   /* cfg_bed.med_folder   */
    const char *mood_folder;  /* the picked mood folder  */
    const char *sleep_folder; /* the picked sleep folder */
    uint8_t volume[BED_VOL_COUNT]; /* steps from mode_volume */
} bed_audio_ctx_t;

/*
 * PLAY  -> LINK_AUDIO_PLAY: folder of cmd.folder from ctx, flags RESUME /
 *          KEEP from cmd.resume / cmd.keep, volume from the mode's slot,
 *          fade_ds = fade_ms / 100 rounded to nearest (50 ms rounds up),
 *          at most 65535. Refused if the folder is NULL, "" or fails
 *          cfg_folder_valid(), if cmd.folder is BED_MODE_NONE or unknown, or
 *          if the volume is above 32.
 * STOP  -> LINK_AUDIO_STOP with fade_ds as above, other fields 0.
 * NEXT  -> LINK_AUDIO_NEXT, other fields 0.
 * SCENE, MOOD_FOLDER_NEXT, unknown types: refused.
 * Returns true and fills all of *out (unused fields 0, folder NUL-padded);
 * refused or NULL arguments: false, *out untouched.
 */
bool bed_audio_from_cmd(const bed_cmd_t *cmd, const bed_audio_ctx_t *ctx, link_audio_t *out);

/* LINK_AUDIO_VOLUME with that step, other fields 0. false (out untouched) if
 * step > 32 or out is NULL. */
bool bed_audio_volume_msg(uint8_t step, link_audio_t *out);

/* Slot of a mode: MEDITATION 0, MOOD 1, SLEEP 2; NONE and unknown values
 * give MODE_VOLUME_NONE (-1). */
int bed_audio_volume_index(bed_mode_t mode);

/* NVS key of a slot: "vol_med", "vol_mood", "vol_sleep"; NULL otherwise. */
const char *bed_audio_volume_key(int index);

/* Scene id for a bed_modes scene: MEDITATION scene_med, MOOD scene_mood,
 * SLEEP scene_sleep (all from bed), NORMAL scene_normal. NULL if that id is
 * "" or NULL, bed is NULL (for the three bed scenes), or the scene value is
 * unknown. */
const char *bed_audio_scene(bed_scene_t scene, const cfg_bed_t *bed, const char *scene_normal);

/*
 * The folder to play for a family (prefix BED_PREFIX_MOOD or
 * BED_PREFIX_SLEEP; any prefix works). current = the stored setting
 * (mood_folder / sleep_folder):
 *   list not ready (no folder list from the speaker yet): current if it is
 *     not "", else none.
 *   list ready: current if it is not "", starts with the prefix (any case)
 *     and is in the list; else bed_next_folder(list names, current,
 *     prefix): the family's folder after it, or the first one.
 * Writes it to out and returns true; none: false and out = "" (if
 * out_size > 0). Also false for NULL arguments or out too small.
 */
bool bed_audio_pick_folder(const folder_list_t *list, const char *current, const char *prefix, char *out,
                           size_t out_size);

/* The family's next folder after current (bed_next_folder over the list).
 * false (out = "") if the list is not ready, has no folder of the family,
 * or an argument is NULL / out too small. */
bool bed_audio_next_folder(const folder_list_t *list, const char *current, const char *prefix, char *out,
                           size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* BED_AUDIO_H */
