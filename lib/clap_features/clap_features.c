/*
 * clap_features – peak, rise time and band ratio around an onset.
 * Contract in clap_features.h, tests in test/test_clap_features/.
 */
#include "clap_features.h"

#include <math.h>

#define PI_F 3.14159265358979f
#define SILENT_DBFS (-120.0f)
#define RATIO_LIMIT_DB 60.0f

typedef struct {
    float b0, b1, b2, a1, a2;
    float x1, x2, y1, y2;
} biquad_t;

/* Second-order Butterworth (Q = 1/sqrt(2)), RBJ cookbook form. */
static void biquad_init(biquad_t *q, bool high_pass, float fc, float fs)
{
    const float w0 = 2.0f * PI_F * fc / fs;
    const float cw = cosf(w0);
    const float alpha = sinf(w0) / (2.0f * 0.70710678f);
    const float a0 = 1.0f + alpha;
    if (high_pass) {
        q->b0 = (1.0f + cw) / 2.0f / a0;
        q->b1 = -(1.0f + cw) / a0;
        q->b2 = q->b0;
    } else {
        q->b0 = (1.0f - cw) / 2.0f / a0;
        q->b1 = (1.0f - cw) / a0;
        q->b2 = q->b0;
    }
    q->a1 = -2.0f * cw / a0;
    q->a2 = (1.0f - alpha) / a0;
    q->x1 = q->x2 = q->y1 = q->y2 = 0.0f;
}

static float biquad_step(biquad_t *q, float x)
{
    const float y = q->b0 * x + q->b1 * q->x1 + q->b2 * q->x2 - q->a1 * q->y1 - q->a2 * q->y2;
    q->x2 = q->x1;
    q->x1 = x;
    q->y2 = q->y1;
    q->y1 = y;
    return y;
}

void clap_features_config_default(clap_features_config_t *cfg)
{
    if (cfg != NULL) {
        cfg->sample_rate_hz = 16000;
        cfg->low_hz = 300.0f;
        cfg->high_hz = 1000.0f;
    }
}

void clap_gate_default(clap_gate_t *gate)
{
    if (gate != NULL) {
        gate->min_band_ratio_db = 6.0f;
        gate->max_rise_ms = 2.0f;
    }
}

static bool config_ok(const clap_features_config_t *c)
{
    if (c->sample_rate_hz < 8000 || c->sample_rate_hz > 48000) {
        return false;
    }
    const float nyquist = (float)c->sample_rate_hz / 2.0f;
    return c->low_hz > 0.0f && c->low_hz < c->high_hz && c->high_hz < nyquist;
}

bool clap_features_measure(const clap_features_config_t *cfg, const int16_t *x, size_t n, clap_features_t *out)
{
    if (cfg == NULL || x == NULL || n == 0 || out == NULL || !config_ok(cfg)) {
        return false;
    }
    int32_t peak = 0;
    for (size_t i = 0; i < n; i++) {
        const int32_t a = x[i] < 0 ? -(int32_t)x[i] : x[i];
        if (a > peak) {
            peak = a;
        }
    }
    clap_features_t f = {SILENT_DBFS, 0.0f, 0.0f};
    if (peak == 0) {
        *out = f;
        return true;
    }
    f.peak_dbfs = 20.0f * log10f((float)peak / 32768.0f);

    /* rise: first |x| >= 10 % of peak to first |x| >= 90 % of peak */
    size_t i10 = n;
    size_t i90 = n;
    for (size_t i = 0; i < n && i90 == n; i++) {
        const int32_t a = x[i] < 0 ? -(int32_t)x[i] : x[i];
        if (i10 == n && 10 * a >= peak) {
            i10 = i;
        }
        if (10 * a >= 9 * peak) {
            i90 = i;
        }
    }
    f.rise_ms = (float)(i90 - i10) * 1000.0f / (float)cfg->sample_rate_hz;

    /* band ratio */
    const float fs = (float)cfg->sample_rate_hz;
    biquad_t hp;
    biquad_t lp;
    biquad_init(&hp, true, cfg->high_hz, fs);
    biquad_init(&lp, false, cfg->low_hz, fs);
    float e_high = 0.0f;
    float e_low = 0.0f;
    for (size_t i = 0; i < n; i++) {
        const float s = (float)x[i];
        const float h = biquad_step(&hp, s);
        const float l = biquad_step(&lp, s);
        e_high += h * h;
        e_low += l * l;
    }
    float ratio;
    if (e_high <= 0.0f && e_low <= 0.0f) {
        ratio = 0.0f;
    } else if (e_low <= 0.0f) {
        ratio = RATIO_LIMIT_DB;
    } else if (e_high <= 0.0f) {
        ratio = -RATIO_LIMIT_DB;
    } else {
        ratio = 10.0f * log10f(e_high / e_low);
    }
    if (ratio > RATIO_LIMIT_DB) {
        ratio = RATIO_LIMIT_DB;
    } else if (ratio < -RATIO_LIMIT_DB) {
        ratio = -RATIO_LIMIT_DB;
    }
    f.band_ratio_db = ratio;
    *out = f;
    return true;
}

bool clap_features_is_clap(const clap_gate_t *gate, const clap_features_t *features)
{
    if (gate == NULL || features == NULL) {
        return false;
    }
    return features->band_ratio_db >= gate->min_band_ratio_db && features->rise_ms <= gate->max_rise_ms;
}
