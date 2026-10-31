/*
 * GPIO plan for both boards. PROVISIONAL: confirm every pin against your
 * board's silkscreen and the Sonocotta schematic before soldering.
 *
 * Louder-ESP32-S3 Plus: pins fixed by the board come from the Sonocotta
 * pinout (I2S 14/15/16, I2C 8/9, PDN 17 also appear in Sonocotta's ESPHome
 * configs). The free pins below are taken from the option headers that are
 * not fitted in this build (Ethernet, OLED, IR), so they are physically
 * reachable.
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

/* Rotary encoder on the OLED header (OLED not fitted) */
#define PIN_ENC_A    47
#define PIN_ENC_B    38
#define PIN_ENC_SW   48

/* LR7843 module trigger inputs */
#define PIN_SW_CANDLES   7  /* IR header (IR receiver not fitted)  */
#define PIN_SW_FOUNTAIN  6  /* Ethernet INT (module not fitted)    */
#define PIN_SW_SPOT      5  /* Ethernet RST (module not fitted)    */

/* Push button: the encoder's switch (and any extra key in parallel), to GND */
#define PIN_BUTTON   PIN_ENC_SW

#elif defined(BOARD_DEVKITC1)

/* Bench prototype: LEDs (each with ~330 Ω to GND) stand in for the outputs.
 * Candles and fountain use the Louder's pins, so the code moves over unchanged. */
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

#endif /* BOARD_PINS_H */
