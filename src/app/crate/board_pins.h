/*
 * GPIO plan: holy crate on the Louder-ESP32-S3 Plus (battery powered).
 *
 * The Louder, a microSD module on the LAN headers (as in the bath) and the
 * crate perfboard (docs/CRATE.md): lid contact, battery divider, LED MOSFET.
 * GPIO 1, 2 and 42 are test pads on the Louder's underside; solder thin
 * wires to them. The volume encoder is wired exactly as in the bath:
 *   UART holes (3): GND 43(TX0) 44(RX0)  -> encoder push button 43 (via
 *                                           1 kOhm), encoder A 44
 *   RGB LED header H2 (3): 3V3 GND 21     -> encoder B 21, encoder supply
 */
#ifndef BOARD_PINS_H
#define BOARD_PINS_H

#if !defined(BOARD_LOUDER_S3_PLUS)
#error "crate runs on the Louder: build with -DBOARD_LOUDER_S3_PLUS"
#endif

/* microSD over SPI2 */
#define PIN_SD_SCK   12
#define PIN_SD_MOSI  11
#define PIN_SD_MISO  13
#define PIN_SD_CS    10

/* Fixed by the board: amplifier */
#define PIN_AMP_I2S_BCLK  14
#define PIN_AMP_I2S_DOUT  16
#define PIN_AMP_I2S_WS    15
#define PIN_AMP_I2C_SDA    8
#define PIN_AMP_I2C_SCL    9
#define PIN_AMP_PWDN      17 /* RTC GPIO: hold it low through deep sleep */
#define PIN_AMP_FAULT     18

/* Crate perfboard */
#define PIN_LID       1  /* high = lid open (RTC pull-up, contact to GND when
                            closed); RTC GPIO, ext0 wake source           */
#define PIN_VBAT_ADC  2  /* ADC1 channel 1; 1 MOhm / 220 kOhm divider:
                            V_bat = V_pin * 1220 / 220                     */
#define PIN_LED_PWM  42  /* LEDC, 14-bit at 1 kHz; floats in deep sleep,
                            the 10 kOhm on the board keeps the gate low   */

/* Volume encoder (EC11). GPIO43 is UART0 TX and drives high while the ROM
 * boots, so the push button goes through ~1 kOhm to GND. */
#define PIN_ENC_A    44  /* UART hole "RX"           */
#define PIN_ENC_B    21  /* RGB LED header, data pin */
#define PIN_ENC_SW   43  /* UART hole "TX", 1 kOhm   */

/* The encoder's push button is the node's button (input_task). */
#define PIN_BUTTON   PIN_ENC_SW

/* No Hue on the crate: input_task leaves out the long-press pairing. */
#define INPUT_NO_HUE_PAIRING 1

/* 43/44 are used as GPIOs: no UART console input on this board (USB only). */
#define UART0_PINS_ARE_GPIO 1

#endif /* BOARD_PINS_H */
