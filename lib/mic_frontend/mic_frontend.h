/*
 * mic_frontend – raw I2S words from the INMP441 to int16 PCM for the clap
 * detector (pure C, no hardware).
 *
 * Requirements: CLAP-01, CLAP-14 (see the requirements doc).
 * Tests:        test/test_mic_frontend/
 *
 * Input: the 32-bit words the I2S driver delivers with
 * I2S_DATA_BIT_WIDTH_32BIT. The INMP441's 24-bit sample sits left-aligned in
 * the word (bits 31…8).
 *
 * Per sample n, in this order:
 *   1. x[n] = word >> cfg.shift          arithmetic shift, i.e. floor division
 *                                        by 2^shift; as float
 *   2. high-pass (if cfg.hp_cutoff_hz > 0), one-pole DC blocker:
 *        y[n] = x[n] - x[n-1] + R * y[n-1],   R = exp(-2*pi*hp_cutoff_hz / sample_rate_hz)
 *      with x[-1] = y[-1] = 0 after create/reset. Without high-pass, y = x.
 *      The filter keeps y unsaturated; saturation is an output stage only.
 *   3. out[n] = y[n] rounded to nearest (halves away from zero), saturated to
 *      -32768 … 32767.
 *
 * Default shift 16 maps the 24-bit full scale onto the int16 full scale, so
 * dBFS values stay comparable to the INMP441 datasheet (94 dB SPL = -26 dBFS).
 * The 20 Hz default removes the DC offset; a higher cutoff (100–300 Hz)
 * also suppresses the low-frequency thump of footsteps and doors, while claps
 * keep most of their energy at 1–4 kHz. Tune it with tools/clap_lab.py.
 *
 * Results must not depend on how the input is split into blocks.
 */
#ifndef MIC_FRONTEND_H
#define MIC_FRONTEND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t sample_rate_hz; /* default 16000                                  */
    uint8_t shift;           /* 0 … 31,                       default 16       */
    float hp_cutoff_hz;      /* 0 = off, else < rate / 2,     default 20.0     */
} mic_frontend_config_t;

typedef struct mic_frontend mic_frontend_t;

void mic_frontend_config_default(mic_frontend_config_t *cfg);

/* Returns NULL if cfg is NULL or invalid (rate 0, shift > 31, cutoff < 0,
 * cutoff >= rate / 2, cutoff not a finite number). */
mic_frontend_t *mic_frontend_create(const mic_frontend_config_t *cfg);

/* Accepts NULL. */
void mic_frontend_destroy(mic_frontend_t *mf);

/* Clear the filter state (x[-1] = y[-1] = 0). */
void mic_frontend_reset(mic_frontend_t *mf);

/* Convert n words into n samples (buffers must not overlap). Returns n.
 * words and out may be NULL only if n is 0. */
size_t mic_frontend_process(mic_frontend_t *mf, const int32_t *words, size_t n, int16_t *out);

#ifdef __cplusplus
}
#endif

#endif /* MIC_FRONTEND_H */
