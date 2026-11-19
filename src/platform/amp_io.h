/*
 * amp_io – the tas5825m driver's hardware callbacks (I2C, PDN, delay).
 *
 * amp_io_get() picks by board:
 *   Louder (PIN_AMP_I2C_SDA defined)  real I2C on SDA/SCL at 100 kHz, PDN on
 *       PIN_AMP_PWDN. The amplifier's address is found on the first transfer
 *       after PDN goes high (it only answers then): the TAS5825M's addresses
 *       0x4C … 0x4F are tried, then 0x2C … 0x2F (TAS5805M on the non-Plus
 *       board, whose registers used here are the same).
 *   DevKit  callbacks that only log, so the real driver and its power-up
 *       sequence run on the bench; PIN_LED_AMP follows PDN.
 */
#ifndef AMP_IO_H
#define AMP_IO_H

#include <stdint.h>

#include "tas5825m.h"

/* Configures the pins and returns the callbacks for this board. */
const tas5825m_io_t *amp_io_get(void);

/* The detected I2C address, or 0 (not found yet, or log-only). */
uint8_t amp_io_address(void);

/* Raw register access for the console's "amp" command (diagnostics only).
 * Return 0 on success. Only valid after amp_io_get(). */
int amp_io_read(uint8_t reg, uint8_t *value);
int amp_io_write(uint8_t reg, uint8_t value);

#endif /* AMP_IO_H */
