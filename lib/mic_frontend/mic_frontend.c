/*
 * mic_frontend – raw I2S words from the INMP441 to int16 PCM.
 *
 * The contract is in mic_frontend.h, the tests in test/test_mic_frontend/.
 */
#include "mic_frontend.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>

#define PI_D 3.14159265358979323846 /* M_PI is not part of strict C11 */

struct mic_frontend {
    mic_frontend_config_t cfg;
    bool hp_on;
    float r;       /* pole of the DC blocker */
    float x_prev;  /* x[n-1] */
    float y_prev;  /* y[n-1], unsaturated */
};

void mic_frontend_config_default(mic_frontend_config_t *cfg)
{
    if (cfg != NULL) {
        cfg->sample_rate_hz = 16000;
        cfg->shift = 16;
        cfg->hp_cutoff_hz = 20.0f;
    }
}

static bool config_valid(const mic_frontend_config_t *cfg)
{
    return cfg->sample_rate_hz != 0
        && cfg->shift <= 31
        && isfinite(cfg->hp_cutoff_hz)
        && cfg->hp_cutoff_hz >= 0.0f
        && cfg->hp_cutoff_hz < (float)cfg->sample_rate_hz / 2.0f;
}

mic_frontend_t *mic_frontend_create(const mic_frontend_config_t *cfg)
{
    if (cfg == NULL || !config_valid(cfg)) {
        return NULL;
    }

    mic_frontend_t *mf = calloc(1, sizeof(*mf));
    if (mf == NULL) {
        return NULL;
    }

    mf->cfg = *cfg;
    mf->hp_on = cfg->hp_cutoff_hz > 0.0f;
    /* Computed once in double, so the pole is as exact as a float can hold. */
    mf->r = (float)exp(-2.0 * PI_D * (double)cfg->hp_cutoff_hz / (double)cfg->sample_rate_hz);
    return mf;
}

void mic_frontend_destroy(mic_frontend_t *mf)
{
    free(mf);
}

void mic_frontend_reset(mic_frontend_t *mf)
{
    if (mf != NULL) {
        mf->x_prev = 0.0f;
        mf->y_prev = 0.0f;
    }
}

/* word >> shift as floor division. Right-shifting a negative value is
 * implementation-defined in C, so negatives go through the complement. */
static int32_t floor_shift(int32_t word, uint8_t shift)
{
    return (word >= 0) ? (word >> shift) : ~(~word >> shift);
}

static int16_t round_saturate(float y)
{
    const float r = roundf(y); /* halves away from zero */
    if (r >= 32767.0f) {
        return INT16_MAX;
    }
    if (r <= -32768.0f) {
        return INT16_MIN;
    }
    return (int16_t)r;
}

size_t mic_frontend_process(mic_frontend_t *mf, const int32_t *words, size_t n, int16_t *out)
{
    if (mf == NULL || words == NULL || out == NULL) {
        return 0;
    }

    for (size_t i = 0; i < n; i++) {
        const float x = (float)floor_shift(words[i], mf->cfg.shift);
        float y = x;
        if (mf->hp_on) {
            y = x - mf->x_prev + mf->r * mf->y_prev;
            mf->x_prev = x;
            mf->y_prev = y;
        }
        out[i] = round_saturate(y);
    }
    return n;
}
