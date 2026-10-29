/*
 * button – STUB.
 *
 * Compiles so the tests build; every test fails until this is implemented.
 * The contract is in button.h, the tests in test/test_button/.
 */
#include "button.h"

#include <string.h>

void button_config_default(button_config_t *cfg)
{
    if (cfg != NULL) {
        memset(cfg, 0, sizeof(*cfg)); /* TODO */
    }
}

button_t *button_create(const button_config_t *cfg, uint32_t now_ms)
{
    (void)cfg;
    (void)now_ms;
    return NULL; /* TODO */
}

void button_destroy(button_t *b)
{
    (void)b;
}

button_event_t button_update(button_t *b, bool pressed, uint32_t now_ms)
{
    (void)b;
    (void)pressed;
    (void)now_ms;
    return BUTTON_NONE;
}

bool button_is_pressed(const button_t *b)
{
    (void)b;
    return false;
}
