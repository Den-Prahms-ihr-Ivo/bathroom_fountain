/*
 * bath_volume – which volume slot the bath's encoder sets, and the slots'
 * NVS keys (pure C, no hardware).
 *
 * Requirements: AUD-13, AUD-14 (see the requirements doc).
 * Tests:        test/test_bath_volume/
 *
 * control_task keeps a mode_volume_t of BATH_VOL_COUNT slots.
 *
 *   bath_volume_mode(o, cold_running)
 *     cold_running (cold_protocol runs, any phase)  -> BATH_VOL_COLD
 *     o->mode is Ambient                            -> BATH_VOL_AMBIENT
 *     fading out of Ambient (o->transitioning while
 *       music still plays)                          -> BATH_VOL_AMBIENT
 *     otherwise (Normal, quiet; or o is NULL)       -> MODE_VOLUME_NONE
 *
 *   bath_volume_key(slot)   NVS key: "volume" (Ambient, the v1 key, so the
 *                           saved level carries over), "vol_cold".
 *   bath_volume_name(slot)  name in status and events: "ambient", "cold".
 *   Both return NULL for a slot out of range (also MODE_VOLUME_NONE).
 */
#ifndef BATH_VOLUME_H
#define BATH_VOLUME_H

#include <stdbool.h>

#include "mode_controller.h"
#include "mode_volume.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    BATH_VOL_AMBIENT = 0,
    BATH_VOL_COLD = 1,
    BATH_VOL_COUNT = 2,
};

int bath_volume_mode(const mode_outputs_t *o, bool cold_running);

const char *bath_volume_key(int slot);

const char *bath_volume_name(int slot);

#ifdef __cplusplus
}
#endif

#endif /* BATH_VOLUME_H */
