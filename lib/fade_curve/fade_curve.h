/*
 * fade_curve – maps fade progress to a gain that changes evenly in dB
 * (pure C, no hardware).
 *
 * Requirements: AUD-06 (see the requirements doc).
 * Tests:        test/test_fade_curve/
 *
 * Why: mode_controller's fade_gain runs from 0 to 1 (or back) over the
 * transition. Used directly as a gain, a linear ramp spends most of its time
 * near full volume: halfway through a fade-out the level has only dropped by
 * 6 dB, and almost all the audible change happens in the last quarter.
 * The ear judges loudness in dB, so the control task treats fade_gain as
 * progress p and plays at fade_curve_gain(p) instead:
 *
 *   p <= 0 or NaN        →  0.0 (silence; the amplifier is muted)
 *   p >= 1               →  1.0 exactly
 *   0 < p < 1            →  10^(floor_db * (1 - p) / 20)
 *                           i.e. a straight line in dB from floor_db at p → 0
 *                           to 0 dB at p = 1
 *
 * The step from silence to floor_db at the very start (and back at the very
 * end) is inaudible for floor_db around -50 dB, because the user volume is
 * applied on top.
 *
 * floor_db must be finite and in [-100, 0); anything else (NaN, 0, positive,
 * below -100) is replaced by FADE_CURVE_FLOOR_DB_DEFAULT. The lower limit keeps
 * the gain far above float underflow, so the result is > 0 whenever p > 0.
 *
 * The curve is strictly increasing on (0, 1], so a monotonic fade stays
 * monotonic.
 */
#ifndef FADE_CURVE_H
#define FADE_CURVE_H

#ifdef __cplusplus
extern "C" {
#endif

#define FADE_CURVE_FLOOR_DB_DEFAULT (-50.0f)
#define FADE_CURVE_FLOOR_DB_MIN     (-100.0f)

float fade_curve_gain(float progress, float floor_db);

#ifdef __cplusplus
}
#endif

#endif /* FADE_CURVE_H */
