/*
 * bath_volume – which of the bath's volumes the encoder sets.
 * Contract in bath_volume.h, tests in test/test_bath_volume/.
 */
#include "bath_volume.h"

#include "mode_volume.h"

int bath_volume_mode(const mode_outputs_t *outputs, bool cold_running)
{
    if (cold_running) {
        return BATH_VOL_COLD;
    }
    if (outputs != NULL && (outputs->mode == AMBIENCE_AMBIENT || outputs->music_playing)) {
        return BATH_VOL_AMBIENT;
    }
    return MODE_VOLUME_NONE;
}

const char *bath_volume_key(int slot)
{
    switch (slot) {
    case BATH_VOL_AMBIENT:
        return "volume";
    case BATH_VOL_COLD:
        return "vol_cold";
    default:
        return NULL;
    }
}

const char *bath_volume_name(int slot)
{
    switch (slot) {
    case BATH_VOL_AMBIENT:
        return "ambient";
    case BATH_VOL_COLD:
        return "cold";
    default:
        return NULL;
    }
}
