/*
 * volume – STUB.
 *
 * Compiles so the tests build; every test fails until this is implemented.
 * The contract is in volume.h, the tests in test/test_volume/.
 */
#include "volume.h"

#include <string.h>

void volume_config_default(volume_config_t *cfg)
{
    if (cfg != NULL) {
        memset(cfg, 0, sizeof(*cfg)); /* TODO */
    }
}

volume_t *volume_create(const volume_config_t *cfg, bool has_saved, uint8_t saved_step)
{
    (void)cfg;
    (void)has_saved;
    (void)saved_step;
    return NULL; /* TODO */
}

void volume_destroy(volume_t *v)
{
    (void)v;
}

uint8_t volume_step(const volume_t *v)
{
    (void)v;
    return 0;
}

void volume_turn(volume_t *v, int32_t detents, uint32_t now_ms)
{
    (void)v;
    (void)detents;
    (void)now_ms;
}

float volume_gain(const volume_t *v)
{
    (void)v;
    return 0.0f;
}

bool volume_take_save(volume_t *v, uint32_t now_ms, uint8_t *step_out)
{
    (void)v;
    (void)now_ms;
    (void)step_out;
    return false;
}
