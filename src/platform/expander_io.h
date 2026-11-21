/*
 * expander_io – the bath's TCA9555 port expander (lib/tca9555) on the
 * amplifier's I2C bus: cold button, hype/zen switch and their LEDs
 * (pin plan in src/app/bath/board_pins.h).
 *
 * Polls every 10 ms and posts to the event queue:
 *   EVT_COLD_PRESS   cold button released before 2 s (lib/button debounce)
 *   EVT_COLD_HOLD    cold button held 2 s (abort)
 *   EVT_COLD_SWITCH  the hype/zen switch changed (detents 1 = hype, 0 = zen);
 *                    also once after start with the position at boot
 *
 * A missing or broken chip is logged once and retried every 5 s; the bath
 * works without it (cold shower from the web app, cold_profile setting).
 * On boards without EXPANDER_I2C_ADDR everything here does nothing.
 */
#ifndef EXPANDER_IO_H
#define EXPANDER_IO_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void expander_io_start(void);

/* LEDs; ignored until the chip answers (re-applied after a re-init). */
void expander_io_cold_led(bool on);

/* Switch position: true = hype. false when no chip answers. */
bool expander_io_hype(void);
bool expander_io_present(void);

#ifdef __cplusplus
}
#endif

#endif /* EXPANDER_IO_H */
