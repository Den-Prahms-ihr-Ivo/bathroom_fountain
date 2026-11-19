/*
 * tca9555 – 16-bit I2C port expander on the carrier board (pure C; the bus
 * comes in through callbacks, so the driver runs and is tested on the PC).
 *
 * Requirements: HW-20, HW-21 (see the requirements doc).
 * Tests:        test/test_tca9555/
 *
 * Carrier Rev A puts a TCA9555 at address 0x20 on GPIO 1 (SDA) / GPIO 2
 * (SCL). The address is not part of this module: the board code's callbacks
 * know it. Pins are numbered 0 ... 15: P00 ... P07 are 0 ... 7, P10 ... P17
 * are 8 ... 15. The carrier's pin plan (which button port sits on which pin)
 * belongs in src/app/bath/board_pins.h, not here.
 *
 * Registers (each is a pair, port 0 then port 1; the device auto-increments
 * within a pair, so one two-byte transfer covers both ports):
 *   0x00 INPUT       pin levels (read only)
 *   0x02 OUTPUT      output latches, power-on value 0xFFFF
 *   0x04 POLARITY    1 = input reads inverted, power-on value 0x0000
 *   0x06 CONFIG      1 = input, 0 = output, power-on value 0xFFFF
 * Words below are little-endian in that sense: bit n is pin n.
 *
 * tca9555_init()
 *   Brings the chip into a known state whatever it was before (the ESP32 can
 *   reset without the expander losing power). In this order, each as one
 *   two-byte write:
 *     1. OUTPUT   = cfg.initial_levels  (latches first, so no pin glitches
 *                                        to a wrong level when it becomes an
 *                                        output)
 *     2. POLARITY = cfg.inverted_inputs
 *     3. CONFIG   = ~cfg.output_pins
 *   Then CONFIG is read back; anything else than what was written is
 *   TCA9555_ERR_VERIFY (wrong chip, no chip pulling the bus right).
 *
 * Outputs
 *   The driver keeps a shadow of the OUTPUT register, so setting one pin is
 *   a single two-byte write with no read. A write is skipped when the shadow
 *   already has the wanted value. After a failed write the shadow is
 *   unknown: the next write is sent in any case.
 *   Only pins in cfg.output_pins can be written; others are
 *   TCA9555_ERR_ARG.
 *
 * Inputs
 *   tca9555_read() reads both INPUT bytes in one transfer and returns all 16
 *   levels after the chip's polarity inversion. Output pins read back their
 *   pin level.
 *
 * Errors
 *   TCA9555_ERR_NOT_READY  a call before a successful init
 *   TCA9555_ERR_IO         a callback returned non-zero; the call stops
 *   A failed init leaves the driver not ready.
 */
#ifndef TCA9555_H
#define TCA9555_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TCA9555_REG_INPUT 0x00
#define TCA9555_REG_OUTPUT 0x02
#define TCA9555_REG_POLARITY 0x04
#define TCA9555_REG_CONFIG 0x06

typedef struct {
    void *ctx;
    /* Write / read `len` bytes starting at register `reg`. 0 on success. */
    int (*write)(void *ctx, uint8_t reg, const uint8_t *data, size_t len);
    int (*read)(void *ctx, uint8_t reg, uint8_t *data, size_t len);
} tca9555_io_t;

typedef struct {
    uint16_t output_pins;     /* 1 = pin is an output                       */
    uint16_t initial_levels;  /* level of each output after init            */
    uint16_t inverted_inputs; /* 1 = input reads 1 when the pin is low
                                 (switches to GND read "pressed = 1")       */
} tca9555_config_t;

typedef enum {
    TCA9555_OK = 0,
    TCA9555_ERR_ARG,
    TCA9555_ERR_NOT_READY,
    TCA9555_ERR_IO,
    TCA9555_ERR_VERIFY,
} tca9555_err_t;

typedef struct tca9555 tca9555_t;

/* Returns NULL if io or one of its callbacks is NULL. No bus traffic. */
tca9555_t *tca9555_create(const tca9555_io_t *io);

/* Accepts NULL. */
void tca9555_destroy(tca9555_t *dev);

tca9555_err_t tca9555_init(tca9555_t *dev, const tca9555_config_t *cfg);

bool tca9555_is_ready(const tca9555_t *dev);

/* All 16 pin levels. *levels is only written on TCA9555_OK. */
tca9555_err_t tca9555_read(tca9555_t *dev, uint16_t *levels);

/* Set one output pin (0 ... 15). */
tca9555_err_t tca9555_write_pin(tca9555_t *dev, uint8_t pin, bool high);

/* Set several outputs at once: the pins in mask take their bit from levels.
 * mask must only contain output pins. */
tca9555_err_t tca9555_write_masked(tca9555_t *dev, uint16_t mask, uint16_t levels);

#ifdef __cplusplus
}
#endif

#endif /* TCA9555_H */
