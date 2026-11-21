/*
 * GPIO plan for both boards.
 *
 * Louder-ESP32-S3 Plus (Rev A/A1): the board has no general GPIO header. The
 * free pins are reached through the option footprints that this build does
 * not use (checked against Sonocotta's schematic and PCB, esp32-audio-dock
 * repo, hardware/7-louder-esp32-plus):
 *
 *   LAN headers P1/P2 (2x female 1x6, for the W5500 module, not fitted;
 *   P1 = row at the board edge, P2 = inner row, pin 1 = square pad)
 *       P1: GND GND 11 12 10 6        P2: GND 3V3 3V3 NC 5 13
 *       -> microSD (10/11/12/13 + 3V3/GND), candles 5, fountain 6
 *   Mic footprint "L" (2x3 holes): GND VDD SD=39 LR(=GND) WS=40 CLK=41
 *       -> INMP441
 *   UART holes (3): GND 43(TX0) 44(RX0)
 *       -> encoder push button 43 (via 1 kOhm), encoder A 44
 *   RGB LED header H2 (3): 3V3 (pin 1, square pad) GND 21
 *       -> encoder B 21, encoder supply
 *
 * Not usable: 47/48/38 only exist on the 30-pin 0.5 mm OLED FPC; 7 carries
 * the fitted IR receiver (desolder it to get 7 back); 35-37 belong to PSRAM.
 * Because 43/44 become GPIOs, the serial console on this board is the USB-C
 * port only (console_task skips the UART reader, see UART0_PINS_ARE_GPIO).
 *
 * Code uses #ifdef on the optional pins: a board without PIN_AMP_I2S_BCLK
 * plays "dry" (decodes and paces, no audio out), a board without
 * PIN_AMP_I2C_SDA drives the amplifier driver with logging callbacks only.
 */
#ifndef BOARD_PINS_H
#define BOARD_PINS_H

/* Microphone, INMP441 (same pins on both boards so code transfers unchanged) */
#define PIN_MIC_SCK   41
#define PIN_MIC_WS    40
#define PIN_MIC_SD    39

/* microSD over SPI2 (native SPI2 pins on the ESP32-S3, same on both boards) */
#define PIN_SD_SCK   12
#define PIN_SD_MOSI  11
#define PIN_SD_MISO  13
#define PIN_SD_CS    10

#if defined(BOARD_LOUDER_S3_PLUS)

/* Fixed by the board: amplifier */
#define PIN_AMP_I2S_BCLK  14
#define PIN_AMP_I2S_DOUT  16
#define PIN_AMP_I2S_WS    15
#define PIN_AMP_I2C_SDA    8
#define PIN_AMP_I2C_SCL    9
#define PIN_AMP_PWDN      17
#define PIN_AMP_FAULT     18

/* Rotary encoder: UART holes + RGB header. GPIO43 is UART0 TX and drives
 * high while the ROM boots, so the push button goes through ~1 kOhm to GND. */
#define PIN_ENC_A    44  /* UART hole "RX"           */
#define PIN_ENC_B    21  /* RGB LED header, data pin */
#define PIN_ENC_SW   43  /* UART hole "TX", 1 kOhm   */

/* LR7843 module trigger inputs (LAN headers) */
#define PIN_SW_CANDLES   5  /* P2, LAN_RST */
#define PIN_SW_FOUNTAIN  6  /* P1, LAN_INT */
/* Spot: no free pin left. GPIO7 works once the IR receiver is desoldered:
 * #define PIN_SW_SPOT 7 */

/* Push button: the encoder's switch (and any extra key in parallel), to GND */
#define PIN_BUTTON   PIN_ENC_SW

/* 43/44 are used as GPIOs: no UART console input on this board (USB only). */
#define UART0_PINS_ARE_GPIO 1

/* TCA9555 port expander (16 I/O) on the amplifier's I2C bus (GPIO 8/9, shared
 * with the TAS5825M at 0x4C...0x4F), address 0x20 (A0 = A1 = A2 = GND).
 * Breakout: VCC 3.3 V, GND, SDA -> the amp header's SDA, SCL -> SCL; the
 * Louder's bus pull-ups are enough at 100 kHz. INT is not used (polled every
 * 10 ms). Carrier Rev A moves the chip to GPIO 1/2; only these lines change. */
#define EXPANDER_ON_AMP_BUS 1
#define EXPANDER_I2C_ADDR   0x20

#elif defined(BOARD_DEVKITC1)

/* Bench prototype: LEDs (each with ~330 Ω to GND) stand in for the outputs.
 * (Pin numbers differ from the Louder; the code only uses the names.) */
#define PIN_SW_CANDLES   7
#define PIN_SW_FOUNTAIN  6
#define PIN_LED_AMP      5  /* follows the amplifier's PDN (tas5825m stand-in) */
#define PIN_LED_MUSIC    4  /* on while music plays */

/* Rotary encoder: 38 and 48 are taken by the RGB LED on one revision or the
 * other, so the bench uses 1 and 2 (not strapping pins on the S3). */
#define PIN_ENC_A    1
#define PIN_ENC_B    2

/* Push button (Cherry key and the encoder's switch in parallel) to GND,
 * internal pull-up. */
#define PIN_BUTTON  47

#else
#error "Build with -DBOARD_LOUDER_S3_PLUS or -DBOARD_DEVKITC1 (see platformio.ini)"
#endif

/* Expander pin plan (docs/ARCHITECTURE.md, Carrier Rev A: button port n has
 * its switch on pin 8-2n and its LED on pin 9-2n; pins 0 ... 15 = P00 ... P17).
 * Switches go to GND (inverted inputs: pressed / closed reads 1), LEDs from
 * the pin through ~1 kOhm to GND (high = on). Only boards that define
 * EXPANDER_I2C_ADDR use them. */
#define XP_COLD_BUTTON   6  /* port 1: cold button (P06)                    */
#define XP_COLD_LED      7  /* port 1: LED in / next to it, on while a run  */
#define XP_HYPE_SWITCH   4  /* port 2: hype/zen switch (P04), closed = hype */
#define XP_HYPE_LED      5  /* port 2: LED, on = hype selected              */
#define XP_SD_DETECT     8  /* reserved: card detect (P10)                  */
#define XP_STATUS_LED    9  /* reserved: status LED (P11)                   */
#define XP_OUTPUT_MASK   0x02AAu /* LEDs on 1, 3, 5, 7 and P11             */
#define XP_INVERTED_MASK 0x0155u /* switches on 0, 2, 4, 6 and P10         */

#endif /* BOARD_PINS_H */
