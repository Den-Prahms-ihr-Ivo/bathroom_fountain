/*
 * mode_volume – one volume per music mode, set by the encoder only while
 * that mode plays (pure C, no hardware).
 *
 * Requirements: AUD-13, AUD-14 (see the requirements doc).
 * Tests:        test/test_mode_volume/
 *
 * Bath: two modes (0 = Ambient, 1 = cold-shower run). Bedside: three
 * (0 = meditation, 1 = mood, 2 = sleep). The app numbers its modes and
 * passes the one that plays now, or MODE_VOLUME_NONE when nothing plays.
 *
 * Each mode has its own lib/volume object, all made from the same
 * volume_config_t, so steps, cap, curve and save delay are the same for all
 * modes; only the step differs.
 *
 *   turn     mode_volume_turn(mv, mode, detents, now): volume_turn() on that
 *            mode's object. MODE_VOLUME_NONE or a mode >= count: ignored
 *            (returns false, nothing changes). Otherwise returns true, also
 *            when the step was already at a limit.
 *   saving   Each mode saves on its own (volume_take_save() rules: after
 *            save_delay_ms of rest, only if the step differs from the saved
 *            one). mode_volume_take_save() returns at most one mode per
 *            call: the lowest-numbered mode that has something to save. The
 *            app stores step under that mode's NVS key and calls again.
 *   start    has_saved / saved (arrays of `count`, or NULL for "nothing
 *            saved") give each mode its NVS value; see volume_create().
 */
#ifndef MODE_VOLUME_H
#define MODE_VOLUME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "volume.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MODE_VOLUME_MAX 8
#define MODE_VOLUME_NONE (-1)

typedef struct mode_volume mode_volume_t;

/* Returns NULL if cfg is NULL or invalid for volume_create(), count is 0 or
 * above MODE_VOLUME_MAX, exactly one of has_saved / saved is NULL, or memory
 * runs out. */
mode_volume_t *mode_volume_create(const volume_config_t *cfg, size_t count, const bool *has_saved,
                                  const uint8_t *saved);

/* Accepts NULL. */
void mode_volume_destroy(mode_volume_t *mv);

/* Number of modes; 0 for NULL. */
size_t mode_volume_count(const mode_volume_t *mv);

/* false for mv NULL. */
bool mode_volume_turn(mode_volume_t *mv, int mode, int32_t detents, uint32_t now_ms);

/* Step / gain of a mode (volume_step(), volume_gain()); 0 for a mode out of
 * range or mv NULL. */
uint8_t mode_volume_step(const mode_volume_t *mv, int mode);
float mode_volume_gain(const mode_volume_t *mv, int mode);

/* false (nothing taken) if mv, mode_out or step_out is NULL. */
bool mode_volume_take_save(mode_volume_t *mv, uint32_t now_ms, int *mode_out, uint8_t *step_out);

#ifdef __cplusplus
}
#endif

#endif /* MODE_VOLUME_H */
