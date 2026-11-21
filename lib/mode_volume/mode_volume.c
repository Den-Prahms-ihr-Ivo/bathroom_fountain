/*
 * mode_volume – one volume per music mode.
 * Contract in mode_volume.h, tests in test/test_mode_volume/.
 */
#include "mode_volume.h"

#include <stdlib.h>

struct mode_volume {
    size_t count;
    volume_t *vol[MODE_VOLUME_MAX];
};

mode_volume_t *mode_volume_create(const volume_config_t *cfg, size_t count, const bool *has_saved,
                                  const uint8_t *saved)
{
    if (cfg == NULL || count == 0 || count > MODE_VOLUME_MAX || (has_saved == NULL) != (saved == NULL)) {
        return NULL;
    }

    mode_volume_t *mv = calloc(1, sizeof(*mv));
    if (mv == NULL) {
        return NULL;
    }
    mv->count = count;
    for (size_t i = 0; i < count; i++) {
        const bool has = (has_saved != NULL) && has_saved[i];
        mv->vol[i] = volume_create(cfg, has, has ? saved[i] : 0);
        if (mv->vol[i] == NULL) { /* invalid cfg or out of memory */
            mode_volume_destroy(mv);
            return NULL;
        }
    }
    return mv;
}

void mode_volume_destroy(mode_volume_t *mv)
{
    if (mv == NULL) {
        return;
    }
    for (size_t i = 0; i < mv->count; i++) {
        volume_destroy(mv->vol[i]);
    }
    free(mv);
}

size_t mode_volume_count(const mode_volume_t *mv)
{
    return (mv != NULL) ? mv->count : 0;
}

static volume_t *mode_vol(const mode_volume_t *mv, int mode)
{
    if (mv == NULL || mode < 0 || (size_t)mode >= mv->count) {
        return NULL;
    }
    return mv->vol[mode];
}

bool mode_volume_turn(mode_volume_t *mv, int mode, int32_t detents, uint32_t now_ms)
{
    volume_t *v = mode_vol(mv, mode);
    if (v == NULL) {
        return false;
    }
    volume_turn(v, detents, now_ms);
    return true;
}

uint8_t mode_volume_step(const mode_volume_t *mv, int mode)
{
    const volume_t *v = mode_vol(mv, mode);
    return (v != NULL) ? volume_step(v) : 0;
}

float mode_volume_gain(const mode_volume_t *mv, int mode)
{
    const volume_t *v = mode_vol(mv, mode);
    return (v != NULL) ? volume_gain(v) : 0.0f;
}

bool mode_volume_take_save(mode_volume_t *mv, uint32_t now_ms, int *mode_out, uint8_t *step_out)
{
    if (mv == NULL || mode_out == NULL || step_out == NULL) {
        return false;
    }
    for (size_t i = 0; i < mv->count; i++) {
        if (volume_take_save(mv->vol[i], now_ms, step_out)) {
            *mode_out = (int)i;
            return true;
        }
    }
    return false;
}
