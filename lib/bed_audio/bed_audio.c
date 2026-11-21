/*
 * bed_audio – bed_modes commands to speaker messages, scenes, volume slots,
 * mood folder.
 * Contract in bed_audio.h, tests in test/test_bed_audio/.
 */
#include "bed_audio.h"

#include <string.h>

static uint16_t to_ds(uint32_t ms)
{
    const uint64_t ds = ((uint64_t)ms + 50u) / 100u;
    return (ds > 65535u) ? 65535u : (uint16_t)ds;
}

static const char *mode_folder(const bed_audio_ctx_t *ctx, bed_mode_t mode)
{
    switch (mode) {
    case BED_MODE_MEDITATION:
        return ctx->med_folder;
    case BED_MODE_MOOD:
        return ctx->mood_folder;
    case BED_MODE_SLEEP:
        return ctx->sleep_folder;
    default:
        return NULL;
    }
}

bool bed_audio_from_cmd(const bed_cmd_t *cmd, const bed_audio_ctx_t *ctx, link_audio_t *out)
{
    if (cmd == NULL || ctx == NULL || out == NULL) {
        return false;
    }

    link_audio_t a;
    memset(&a, 0, sizeof(a));

    switch (cmd->type) {
    case BED_CMD_PLAY: {
        const int slot = bed_audio_volume_index(cmd->folder);
        const char *folder = mode_folder(ctx, cmd->folder);
        if (slot == MODE_VOLUME_NONE || folder == NULL || folder[0] == '\0' || !cfg_folder_valid(folder)
            || ctx->volume[slot] > BED_AUDIO_VOLUME_MAX) {
            return false;
        }
        a.cmd = LINK_AUDIO_PLAY;
        a.flags = (uint8_t)((cmd->resume ? LINK_AUDIO_FLAG_RESUME : 0) | (cmd->keep ? LINK_AUDIO_FLAG_KEEP : 0));
        a.volume = ctx->volume[slot];
        a.fade_ds = to_ds(cmd->fade_ms);
        strcpy(a.folder, folder); /* fits: cfg_folder_valid() caps it at LINK_FOLDER_LEN */
        break;
    }
    case BED_CMD_STOP:
        a.cmd = LINK_AUDIO_STOP;
        a.fade_ds = to_ds(cmd->fade_ms);
        break;
    case BED_CMD_NEXT:
        a.cmd = LINK_AUDIO_NEXT;
        break;
    default:
        return false;
    }
    *out = a;
    return true;
}

bool bed_audio_volume_msg(uint8_t step, link_audio_t *out)
{
    if (out == NULL || step > BED_AUDIO_VOLUME_MAX) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->cmd = LINK_AUDIO_VOLUME;
    out->volume = step;
    return true;
}

int bed_audio_volume_index(bed_mode_t mode)
{
    switch (mode) {
    case BED_MODE_MEDITATION:
        return BED_VOL_MEDITATION;
    case BED_MODE_MOOD:
        return BED_VOL_MOOD;
    case BED_MODE_SLEEP:
        return BED_VOL_SLEEP;
    default:
        return MODE_VOLUME_NONE;
    }
}

const char *bed_audio_volume_key(int slot)
{
    switch (slot) {
    case BED_VOL_MEDITATION:
        return "vol_med";
    case BED_VOL_MOOD:
        return "vol_mood";
    case BED_VOL_SLEEP:
        return "vol_sleep";
    default:
        return NULL;
    }
}

static const char *non_empty(const char *s)
{
    return (s != NULL && s[0] != '\0') ? s : NULL;
}

const char *bed_audio_scene(bed_scene_t scene, const cfg_bed_t *bed, const char *normal_scene)
{
    if (scene == BED_SCENE_NORMAL) {
        return non_empty(normal_scene);
    }
    if (bed == NULL) {
        return NULL;
    }
    switch (scene) {
    case BED_SCENE_MEDITATION:
        return non_empty(bed->scene_med);
    case BED_SCENE_MOOD:
        return non_empty(bed->scene_mood);
    case BED_SCENE_SLEEP:
        return non_empty(bed->scene_sleep);
    default:
        return NULL;
    }
}

static bool copy_out(const char *name, char *out, size_t out_size)
{
    const size_t len = strlen(name);
    if (len + 1 > out_size) {
        if (out_size > 0) {
            out[0] = '\0';
        }
        return false;
    }
    memcpy(out, name, len + 1);
    return true;
}

static bool next_on_list(const folder_list_t *list, const char *current, char *out, size_t out_size)
{
    const char *names[FOLDER_LIST_MAX];
    const size_t n = folder_list_count(list);
    for (size_t i = 0; i < n; i++) {
        names[i] = folder_list_name(list, i);
    }
    return bed_next_folder(names, n, current, BED_AUDIO_MOOD_PREFIX, out, out_size);
}

bool bed_audio_pick_mood(const folder_list_t *list, const char *current, char *out, size_t out_size)
{
    if (out != NULL && out_size > 0) {
        out[0] = '\0';
    }
    if (list == NULL || current == NULL || out == NULL) {
        return false;
    }
    if (!folder_list_ready(list)) {
        return current[0] != '\0' && copy_out(current, out, out_size);
    }
    if (current[0] != '\0' && folder_list_has(list, current)) {
        return copy_out(current, out, out_size);
    }
    return next_on_list(list, current, out, out_size);
}

bool bed_audio_next_mood(const folder_list_t *list, const char *current, char *out, size_t out_size)
{
    if (out != NULL && out_size > 0) {
        out[0] = '\0';
    }
    if (list == NULL || current == NULL || out == NULL || !folder_list_ready(list)) {
        return false;
    }
    return next_on_list(list, current, out, out_size);
}
