/*
 * tas5825m – power sequencing, volume and faults for the TAS5825M amplifier
 * on the Louder-ESP32-S3 Plus (pure C; the hardware comes in through
 * callbacks, so the driver runs and is tested on the PC).
 *
 * Requirements: AUD-05, AUD-06, AUD-07, NFR (fault handling)
 * Tests:        test/test_tas5825m/
 *
 * The board code supplies tas5825m_io_t: I2C register read/write on book 0,
 * page 0 (with the board's I2C address), the PDN pin and a millisecond delay.
 * The address is not part of this module; confirm it with an I2C scan on the
 * board (the ADR resistor selects one of 0x4C … 0x4F).
 *
 * Registers used (book 0, page 0; TI datasheet section 9.6, the Linux
 * tas5805m driver and TI E2E answers agree on these):
 *   0x00 PAGE, 0x7F BOOK          select book 0 / page 0
 *   0x03 DEVICE_CTRL2             bits 1:0 state (00 deep sleep, 10 Hi-Z,
 *                                 11 play), bit 3 mute, bit 4 DIS_DSP
 *   0x4C DIG_VOL                  0x30 = 0 dB, 0.5 dB steps, 0xFF = mute
 *   0x70 CHAN_FAULT, 0x71 GLOBAL_FAULT1, 0x72 GLOBAL_FAULT2, 0x73 WARNING
 *   0x78 FAULT_CLEAR              write 0x80 to clear latched faults
 *
 * Precondition for power-up: the I2S clocks (BCLK, LRCLK) are already
 * running. The player keeps I2S0 enabled and sends zeros when idle.
 *
 * Power-up (from off):
 *   1. PDN high, then wait at least TAS5825M_PDN_SETTLE_MS before any I2C.
 *   2. Select page 0 and book 0 (PAGE = 0, then BOOK = 0).
 *   3. DIG_VOL = 0xFF: the amplifier always comes up muted; the caller sets
 *      the real volume afterwards with tas5825m_set_volume().
 *   4. DEVICE_CTRL2 = 0x02 (Hi-Z, DSP enabled), then wait at least
 *      TAS5825M_DSP_BOOT_MS.
 *   5. DEVICE_CTRL2 = 0x03 (play, not muted, DSP enabled).
 *   6. FAULT_CLEAR = 0x80 (drop faults latched while starting, e.g. the
 *      sticky clock fault).
 *   Any I2C error: PDN goes low, the device is off, TAS5825M_ERR_IO.
 *   Already on: TAS5825M_OK with no I/O at all.
 *
 * Power-down (from on):
 *   1. DEVICE_CTRL2 = 0x02 (Hi-Z), then wait at least TAS5825M_STOP_MS.
 *   2. PDN low. The device is off.
 *   PDN goes low and the device counts as off even if the I2C write failed
 *   (then TAS5825M_ERR_IO is returned): a hard stop beats a stuck amplifier.
 *   Already off: TAS5825M_OK, no I2C (PDN may be driven low again).
 *
 * Volume: one write to DIG_VOL. Codes below 0x30 (boost above 0 dB) are
 * written as 0x30: this driver never boosts, whatever the caller passes.
 *
 * Faults: tas5825m_read_faults() reads 0x70 … 0x73. tas5825m_faults_any() is
 * true if CHAN_FAULT, GLOBAL_FAULT1 or GLOBAL_FAULT2 is non-zero; WARNING
 * (over-temperature warning levels) alone does not count as a fault.
 *
 * Everything except create/destroy/is_powered/faults_any returns
 * TAS5825M_ERR_ARG for NULL arguments and TAS5825M_ERR_STATE when the device
 * must be on but is off (set_volume, read_faults, clear_faults), without any
 * I/O in both cases.
 */
#ifndef TAS5825M_H
#define TAS5825M_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TAS5825M_REG_PAGE          0x00u
#define TAS5825M_REG_DEVICE_CTRL2  0x03u
#define TAS5825M_REG_DIG_VOL       0x4Cu
#define TAS5825M_REG_CHAN_FAULT    0x70u
#define TAS5825M_REG_GLOBAL_FAULT1 0x71u
#define TAS5825M_REG_GLOBAL_FAULT2 0x72u
#define TAS5825M_REG_WARNING       0x73u
#define TAS5825M_REG_FAULT_CLEAR   0x78u
#define TAS5825M_REG_BOOK          0x7Fu

#define TAS5825M_CTRL2_DEEP_SLEEP  0x00u
#define TAS5825M_CTRL2_HIZ         0x02u
#define TAS5825M_CTRL2_PLAY        0x03u
#define TAS5825M_CTRL2_MUTE        0x08u
#define TAS5825M_CTRL2_DIS_DSP     0x10u
#define TAS5825M_FAULT_CLEAR_ALL   0x80u

#define TAS5825M_VOL_0DB           0x30u
#define TAS5825M_VOL_MUTE          0xFFu

#define TAS5825M_PDN_SETTLE_MS     5u
#define TAS5825M_DSP_BOOT_MS       5u
#define TAS5825M_STOP_MS           10u

typedef struct {
    void *ctx;
    /* Return 0 on success, anything else on an I2C error. */
    int (*write_reg)(void *ctx, uint8_t reg, uint8_t value);
    int (*read_reg)(void *ctx, uint8_t reg, uint8_t *value);
    void (*set_pdn)(void *ctx, bool high);
    void (*delay_ms)(void *ctx, uint32_t ms);
} tas5825m_io_t;

typedef enum {
    TAS5825M_OK = 0,
    TAS5825M_ERR_ARG,
    TAS5825M_ERR_IO,
    TAS5825M_ERR_STATE,
} tas5825m_err_t;

typedef struct {
    uint8_t chan;     /* 0x70 */
    uint8_t global1;  /* 0x71 */
    uint8_t global2;  /* 0x72 */
    uint8_t warning;  /* 0x73 */
} tas5825m_faults_t;

typedef struct tas5825m tas5825m_t;

/* NULL if io or any of its four callbacks is NULL. The io struct is copied.
 * Drives PDN low (known off state); no I2C. */
tas5825m_t *tas5825m_create(const tas5825m_io_t *io);

/* Accepts NULL. Does no I/O (power down first if needed). */
void tas5825m_destroy(tas5825m_t *dev);

tas5825m_err_t tas5825m_power_up(tas5825m_t *dev);
tas5825m_err_t tas5825m_power_down(tas5825m_t *dev);
tas5825m_err_t tas5825m_set_volume(tas5825m_t *dev, uint8_t code);
tas5825m_err_t tas5825m_read_faults(tas5825m_t *dev, tas5825m_faults_t *out);
tas5825m_err_t tas5825m_clear_faults(tas5825m_t *dev);

/* false for NULL. */
bool tas5825m_is_powered(const tas5825m_t *dev);

/* false for NULL. */
bool tas5825m_faults_any(const tas5825m_faults_t *faults);

#ifdef __cplusplus
}
#endif

#endif /* TAS5825M_H */
