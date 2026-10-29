/*
 * clap_detector – streaming double-clap detector (pure C, no hardware).
 *
 * Requirements: CLAP-02 … CLAP-06, CLAP-13 (see the requirements doc).
 * Tests:        test/test_clap_detector/
 *
 * Input is mono 16-bit PCM at cfg.sample_rate_hz, fed in blocks of any size.
 * The detector reports a double clap once it is *confirmed*, i.e. after the
 * quiet-after window following the second clap has passed without a further
 * onset.
 *
 * Definitions used by the contract below
 * --------------------------------------
 * level   Short-term RMS of the input in dBFS: 20*log10(rms / 32768), measured
 *         over an analysis frame of your choice. The tests assume a frame of
 *         at most 10 ms.
 * onset   A sharp rise of the level to at least cfg.onset_threshold_db above
 *         the running background estimate. Energy arriving within a few tens
 *         of milliseconds of an onset (early reflections, the clap's own
 *         decay) belongs to that onset and must not produce a second one.
 * peak    The highest level reached by an onset's event.
 * gap     second_onset_sample - first_onset_sample, in milliseconds.
 *
 * A pair of consecutive onsets (A, B) is a valid double clap when ALL of
 * these hold:
 *   1. CLAP-02  cfg.min_gap_ms <= gap <= cfg.max_gap_ms
 *   2. CLAP-03  |peak(A) - peak(B)| <= cfg.max_level_diff_db
 *   3. CLAP-04  no other onset within W_before before A and none within
 *               W_after after B, where
 *                 W_before = max(cfg.quiet_before_ms, cfg.quiet_gap_factor * gap)
 *                 W_after  = max(cfg.quiet_after_ms,  cfg.quiet_gap_factor * gap)
 *   4. CLAP-05  between A and B the level falls at least cfg.min_drop_db below
 *               peak(A)
 *   5. CLAP-13  peak(A) and peak(B) are both >= cfg.min_peak_dbfs
 *
 * The background estimate (CLAP-06) adapts to steady noise such as a running
 * shower, so a loud but steady background raises the onset threshold instead
 * of producing onsets.
 *
 * Results must not depend on how the input is split into blocks.
 */
#ifndef CLAP_DETECTOR_H
#define CLAP_DETECTOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t sample_rate_hz;     /* input sample rate                     default 16000 */
    uint16_t min_gap_ms;         /* CLAP-02                               default 150   */
    uint16_t max_gap_ms;         /* CLAP-02                               default 600   */
    float    max_level_diff_db;  /* CLAP-03                               default 6.0   */
    uint16_t quiet_before_ms;    /* CLAP-04, minimum window before A      default 300   */
    uint16_t quiet_after_ms;     /* CLAP-04, minimum window after B       default 400   */
    float    quiet_gap_factor;   /* CLAP-04, windows >= factor * gap      default 1.5   */
    float    min_drop_db;        /* CLAP-05                               default 12.0  */
    float    onset_threshold_db; /* CLAP-06, rise above background        default 12.0  */
    float    min_peak_dbfs;      /* CLAP-13                               default -40.0 */
} clap_config_t;

typedef struct {
    uint64_t first_onset_sample;  /* absolute sample index of onset A           */
    uint64_t second_onset_sample; /* absolute sample index of onset B           */
    uint64_t confirmed_sample;    /* absolute sample index at confirmation:
                                     second_onset_sample + W_after, rounded up
                                     to your frame boundary                     */
} clap_event_t;

/* Absolute sample indices count from 0 at create/reset. */

typedef struct clap_detector clap_detector_t;

/* Fill cfg with the default values listed above. */
void clap_config_default(clap_config_t *cfg);

/*
 * Create a detector. Returns NULL if cfg is NULL, invalid, or memory runs out.
 * Invalid means any of:
 *   sample_rate_hz == 0, min_gap_ms >= max_gap_ms, max_level_diff_db < 0,
 *   quiet_gap_factor < 0, min_drop_db <= 0, onset_threshold_db <= 0.
 * The detector keeps its own copy of cfg.
 */
clap_detector_t *clap_detector_create(const clap_config_t *cfg);

/* Free the detector. Accepts NULL. */
void clap_detector_destroy(clap_detector_t *det);

/* Return to the freshly created state: sample counter 0, history cleared. */
void clap_detector_reset(clap_detector_t *det);

/*
 * Feed n_samples of PCM. Writes up to max_events confirmed double claps to
 * events (in confirmation order) and returns how many were written.
 * events may be NULL only if max_events is 0.
 */
size_t clap_detector_process(clap_detector_t *det,
                             const int16_t *pcm, size_t n_samples,
                             clap_event_t *events, size_t max_events);

/* Total samples fed since create/reset. */
uint64_t clap_detector_samples_processed(const clap_detector_t *det);

#ifdef __cplusplus
}
#endif

#endif /* CLAP_DETECTOR_H */
