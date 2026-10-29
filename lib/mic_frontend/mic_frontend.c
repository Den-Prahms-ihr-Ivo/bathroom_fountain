/*
 * mic_frontend – STUB.
 *
 * Compiles so the tests build; every test fails until this is implemented.
 * The contract is in mic_frontend.h, the tests in test/test_mic_frontend/.
 */
#include "mic_frontend.h"

#include <string.h>

void mic_frontend_config_default(mic_frontend_config_t *cfg)
{
    if (cfg != NULL) {
        memset(cfg, 0, sizeof(*cfg)); /* TODO */
    }
}

mic_frontend_t *mic_frontend_create(const mic_frontend_config_t *cfg)
{
    (void)cfg;
    return NULL; /* TODO */
}

void mic_frontend_destroy(mic_frontend_t *mf)
{
    (void)mf;
}

void mic_frontend_reset(mic_frontend_t *mf)
{
    (void)mf;
}

size_t mic_frontend_process(mic_frontend_t *mf, const int32_t *words, size_t n, int16_t *out)
{
    (void)mf;
    (void)words;
    (void)out;
    (void)n;
    return 0; /* TODO */
}
