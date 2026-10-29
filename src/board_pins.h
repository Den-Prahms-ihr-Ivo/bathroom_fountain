/*
 * GPIO plan for both boards. PROVISIONAL: confirm every pin against your
 * board's silkscreen and the Sonocotta schematic before soldering.
 *
 * Louder-ESP32-S3 Plus: pins fixed by the board come from the Sonocotta
 * pinout. The free pins below are taken from the option headers that are not
 * fitted in this build (Ethernet, OLED, IR), so they are physically reachable.
 */
#ifndef BOARD_PINS_H
#define BOARD_PINS_H

/* Microphone, INMP441 (same pins on both boards so code transfers unchanged) */
#define PIN_MIC_SCK   41
#define PIN_MIC_WS    40
#define PIN_MIC_SD    39

#if defined(BOARD_LOUDER_S3_PLUS)

/* Fixed by the board: amplifier */
#define PIN_AMP_I2S_BCLK  14
#define PIN_AMP_I2S_DOUT  16
#define PIN_AMP_I2S_WS    15
#define PIN_AMP_I2C_SDA    8
#define PIN_AMP_I2C_SCL    9
#define PIN_AMP_PWDN      17
#define PIN_AMP_FAULT     18

/* microSD on the Ethernet header's SPI pins (Ethernet module not fitted) */
#define PIN_SD_SCK   12
#define PIN_SD_MOSI  11
#define PIN_SD_MISO  13
#define PIN_SD_CS    10

/* Rotary encoder on the OLED header (OLED not fitted) */
#define PIN_ENC_A    47
#define PIN_ENC_B    38
#define PIN_ENC_SW   48

/* LR7843 module trigger inputs */
#define PIN_SW_CANDLES   7  /* IR header (IR receiver not fitted)  */
#define PIN_SW_FOUNTAIN  6  /* Ethernet INT (module not fitted)    */
#define PIN_SW_SPOT      5  /* Ethernet RST (module not fitted)    */

#elif defined(BOARD_DEVKITC1)

/* Bench prototype: microphone only for now */

#endif

#endif /* BOARD_PINS_H */
