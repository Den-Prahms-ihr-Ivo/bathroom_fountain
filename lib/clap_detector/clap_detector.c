/*
 * clap_detector – STUB.
 *
 * Compiles so the tests build; every test fails until this is implemented.
 * The contract is in clap_detector.h, the tests in test/test_clap_detector/.
 */
#include "clap_detector.h"

#include <string.h>

void clap_config_default(clap_config_t *cfg)
{
    if (cfg != NULL) {
        memset(cfg, 0, sizeof(*cfg)); /* TODO: requirement defaults */
    }
}

clap_detector_t *clap_detector_create(const clap_config_t *cfg)
{
    (void)cfg;
    return NULL; /* TODO */
}

void clap_detector_destroy(clap_detector_t *det)
{
    (void)det;
}

void clap_detector_reset(clap_detector_t *det)
{
    (void)det;
}

size_t clap_detector_process(clap_detector_t *det,
                             const int16_t *pcm, size_t n_samples,
                             clap_event_t *events, size_t max_events)
{
    (void)det;
    (void)pcm;
    (void)n_samples;
    (void)events;
    (void)max_events;
    return 0;
}

uint64_t clap_detector_samples_processed(const clap_detector_t *det)
{
    (void)det;
    return 0;
}
