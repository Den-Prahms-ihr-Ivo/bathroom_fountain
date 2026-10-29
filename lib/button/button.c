/*
 * button – debounced push button with short and long press.
 *
 * The contract is in button.h, the tests in test/test_button/.
 * Time differences are unsigned, so they survive the counter wrap.
 */
#include "button.h"

#include <stdlib.h>

struct button {
    button_config_t cfg;
    bool pressed;           /* debounced state */
    bool candidate;         /* raw level differs from the debounced state */
    uint32_t candidate_ms;  /* first update that showed the other level */
    uint32_t press_ms;      /* transition time of the current press */
    bool long_sent;         /* LONG already emitted for the current press */
};

void button_config_default(button_config_t *cfg)
{
    if (cfg != NULL) {
        cfg->debounce_ms = 30;
        cfg->long_press_ms = 3000;
    }
}

button_t *button_create(const button_config_t *cfg, uint32_t now_ms)
{
    (void)now_ms; /* the state starts released; nothing is timed yet */

    if (cfg == NULL || (cfg->long_press_ms != 0 && cfg->long_press_ms <= cfg->debounce_ms)) {
        return NULL;
    }

    button_t *b = calloc(1, sizeof(*b));
    if (b == NULL) {
        return NULL;
    }
    b->cfg = *cfg;
    return b;
}

void button_destroy(button_t *b)
{
    free(b);
}

button_event_t button_update(button_t *b, bool pressed, uint32_t now_ms)
{
    if (b == NULL) {
        return BUTTON_NONE;
    }

    if (pressed == b->pressed) {
        b->candidate = false; /* bounce back: restart the run next time */
    } else {
        if (!b->candidate) {
            b->candidate = true;
            b->candidate_ms = now_ms;
        }
        if (now_ms - b->candidate_ms >= b->cfg.debounce_ms) {
            b->pressed = pressed;
            b->candidate = false;
            if (pressed) {
                b->press_ms = b->candidate_ms;
                b->long_sent = false;
                return BUTTON_NONE;
            }
            return b->long_sent ? BUTTON_NONE : BUTTON_SHORT;
        }
    }

    /* `pressed` (raw) must still be down: a release being debounced does not count. */
    if (b->pressed && pressed && !b->long_sent && b->cfg.long_press_ms != 0
        && now_ms - b->press_ms >= b->cfg.long_press_ms) {
        b->long_sent = true;
        return BUTTON_LONG;
    }
    return BUTTON_NONE;
}

bool button_is_pressed(const button_t *b)
{
    return (b != NULL) && b->pressed;
}
