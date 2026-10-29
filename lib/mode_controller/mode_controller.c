/*
 * mode_controller – STUB.
 *
 * Compiles so the tests build; every test fails until this is implemented.
 * The contract is in mode_controller.h, the tests in test/test_mode_controller/.
 */
#include "mode_controller.h"

#include <string.h>

void mode_config_default(mode_config_t *cfg)
{
    if (cfg != NULL) {
        memset(cfg, 0, sizeof(*cfg)); /* TODO: requirement defaults */
    }
}

mode_controller_t *mode_controller_create(const mode_config_t *cfg, uint32_t now_ms)
{
    (void)cfg;
    (void)now_ms;
    return NULL; /* TODO */
}

void mode_controller_destroy(mode_controller_t *mc)
{
    (void)mc;
}

bool mode_controller_trigger(mode_controller_t *mc, trigger_source_t source, uint32_t now_ms)
{
    (void)mc;
    (void)source;
    (void)now_ms;
    return false;
}

void mode_controller_tick(mode_controller_t *mc, uint32_t now_ms)
{
    (void)mc;
    (void)now_ms;
}

const mode_outputs_t *mode_controller_outputs(const mode_controller_t *mc)
{
    (void)mc;
    return NULL;
}

bool mode_controller_take_hue_command(mode_controller_t *mc, hue_command_t *out)
{
    (void)mc;
    (void)out;
    return false;
}
