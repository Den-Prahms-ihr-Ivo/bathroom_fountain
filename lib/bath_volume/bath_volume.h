/*
 * bath_volume – which of the bath's volumes the encoder sets (pure C).
 *
 * Requirements: AUD-13, AUD-14 (see the requirements doc).
 * Tests:        test/test_bath_volume/
 *
 * control_task keeps a mode_volume_t with BATH_VOL_COUNT slots and, for
 * every encoder event, calls
 *   mode_volume_turn(mv, bath_volume_mode(mode_controller_outputs(mc),
 *                                         cold_running), detents, now)
 * The amplifier gain uses mode_volume_gain() of the same slot. Saving:
 * mode_volume_take_save() -> nvs_set_u8(bath_volume_key(slot), step).
 *
 *   cold_running (cold_protocol runs, any phase, also the silent warm
 *     phase)                                   -> BATH_VOL_COLD
 *   else Ambient, or music still playing while fading out of it
 *     (outputs->mode == AMBIENCE_AMBIENT || outputs->music_playing)
 *                                              -> BATH_VOL_AMBIENT
 *   else (Normal, quiet)                       -> MODE_VOLUME_NONE
 *   outputs NULL: BATH_VOL_COLD if cold_running, else MODE_VOLUME_NONE.
 *
 * Keys: Ambient keeps the v1 key "volume" (the saved level carries over),
 * the cold run uses "vol_cold".
 */
#ifndef BATH_VOLUME_H
#define BATH_VOLUME_H

#include <stdbool.h>
#include <stddef.h>

#include "mode_controller.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BATH_VOL_AMBIENT 0
#define BATH_VOL_COLD 1
#define BATH_VOL_COUNT 2

int bath_volume_mode(const mode_outputs_t *outputs, bool cold_running);

/* "volume" for BATH_VOL_AMBIENT, "vol_cold" for BATH_VOL_COLD, else NULL. */
const char *bath_volume_key(int slot);

/* Status name of a slot: "ambient", "cold", else NULL (PROTOCOL.md,
 * status member "volumes"). */
const char *bath_volume_name(int slot);

#ifdef __cplusplus
}
#endif

#endif /* BATH_VOLUME_H */
