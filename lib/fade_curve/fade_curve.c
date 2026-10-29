/*
 * fade_curve – fade progress to a gain that changes evenly in dB.
 *
 * The contract is in fade_curve.h, the tests in test/test_fade_curve/.
 */
#include "fade_curve.h"

#include <math.h>
#include <stdbool.h>

static bool floor_valid(float floor_db)
{
    return isfinite(floor_db) && floor_db >= FADE_CURVE_FLOOR_DB_MIN && floor_db < 0.0f;
}

float fade_curve_gain(float progress, float floor_db)
{
    if (!(progress > 0.0f)) {
        return 0.0f; /* also NaN */
    }
    if (progress >= 1.0f) {
        return 1.0f;
    }
    if (!floor_valid(floor_db)) {
        floor_db = FADE_CURVE_FLOOR_DB_DEFAULT;
    }
    return powf(10.0f, floor_db * (1.0f - progress) / 20.0f);
}
