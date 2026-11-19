/*
 * clap_features – what a sound looks like around its onset, to tell a hand
 * clap from a door, a drawer or a knock on the shelf (pure C).
 *
 * Requirements: CLAP-20 ... CLAP-23 (see the requirements doc).
 * Tests:        test/test_clap_features/
 *
 * clap_detector finds two onsets with the right rhythm. Before a double clap
 * is accepted, the mic task hands the samples around each onset to this
 * module (a window from about 5 ms before the onset to about 30 ms after it,
 * 16-bit, after mic_frontend). A clap is bright and starts abruptly; a door
 * or the shelf is dull and swells. Two numbers capture that, and both go into
 * the clap event that is sent to the server, so thresholds can be tuned from
 * real recordings (tools/clap_lab.py).
 *
 * clap_features_measure()
 *   peak_dbfs      20*log10(peak / 32768) with peak = max |x|; -120 for a
 *                  window of zeros.
 *   rise_ms        time from the first sample with |x| >= 10 % of peak to the
 *                  first sample with |x| >= 90 % of peak, in milliseconds
 *                  (0 if both are the same sample or the window is silent).
 *   band_ratio_db  10*log10(E_high / E_low). E_high is the energy (sum of
 *                  squares over the window) of x after a second-order
 *                  Butterworth high-pass at high_hz; E_low the energy after a
 *                  second-order Butterworth low-pass at low_hz. Both filters
 *                  start from rest at the first sample. The result is clamped
 *                  to -60 ... +60; 0 for a window of zeros.
 *   Returns false and leaves *out untouched if an argument is NULL, n is 0,
 *   or the config is invalid: sample_rate_hz 8000 ... 48000,
 *   0 < low_hz < high_hz < sample_rate_hz / 2.
 *
 * clap_features_is_clap()
 *   true if band_ratio_db >= gate.min_band_ratio_db and
 *   rise_ms <= gate.max_rise_ms. NULL arguments give false.
 *   The defaults are starting points, not tuned values.
 */
#ifndef CLAP_FEATURES_H
#define CLAP_FEATURES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t sample_rate_hz; /* default 16000 */
    float low_hz;            /* default 300   */
    float high_hz;           /* default 1000  */
} clap_features_config_t;

typedef struct {
    float peak_dbfs;
    float rise_ms;
    float band_ratio_db;
} clap_features_t;

typedef struct {
    float min_band_ratio_db; /* default 6.0 */
    float max_rise_ms;       /* default 2.0 */
} clap_gate_t;

void clap_features_config_default(clap_features_config_t *cfg);
void clap_gate_default(clap_gate_t *gate);

bool clap_features_measure(const clap_features_config_t *cfg, const int16_t *x, size_t n, clap_features_t *out);

bool clap_features_is_clap(const clap_gate_t *gate, const clap_features_t *features);

#ifdef __cplusplus
}
#endif

#endif /* CLAP_FEATURES_H */
