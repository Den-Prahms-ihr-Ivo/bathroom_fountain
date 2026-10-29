/*
 * button – debounced push button with short and long press (pure C).
 *
 * Requirements: BEH-03, BEH-04 (see the requirements doc). The long press is
 * available for a later feature (for example re-pairing with the Hue bridge);
 * no requirement uses it yet.
 * Tests:        test/test_button/
 *
 * The input task calls button_update() with the raw pin level every few
 * milliseconds (e.g. 5 ms).
 *
 * Debounce
 *   The debounced state starts "released". A raw level that differs from the
 *   debounced state becomes the new debounced state once it has been reported
 *   unchanged for at least cfg.debounce_ms, measured from the first update
 *   that showed it. Any update with the other level restarts that run. That
 *   first update's time is the transition time used below.
 *
 * Events (at most one per update)
 *   BUTTON_LONG   once per press, at the first update where the debounced
 *                 state is pressed, the raw level is still pressed (a release
 *                 being debounced does not count) and at least
 *                 cfg.long_press_ms have passed since the press transition
 *                 time. Never with long_press_ms 0.
 *   BUTTON_SHORT  at the debounced release, if no LONG was emitted for this
 *                 press.
 *
 * Time is a wrapping uint32 millisecond counter.
 */
#ifndef BUTTON_H
#define BUTTON_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t debounce_ms;   /* default 30; 0 = accept every change at once  */
    uint16_t long_press_ms; /* default 3000; 0 = no long press; otherwise
                               must be greater than debounce_ms              */
} button_config_t;

typedef enum {
    BUTTON_NONE = 0,
    BUTTON_SHORT,
    BUTTON_LONG,
} button_event_t;

typedef struct button button_t;

void button_config_default(button_config_t *cfg);

/* Returns NULL if cfg is NULL or invalid. */
button_t *button_create(const button_config_t *cfg, uint32_t now_ms);

/* Accepts NULL. */
void button_destroy(button_t *b);

button_event_t button_update(button_t *b, bool pressed, uint32_t now_ms);

/* Debounced state. */
bool button_is_pressed(const button_t *b);

#ifdef __cplusplus
}
#endif

#endif /* BUTTON_H */
