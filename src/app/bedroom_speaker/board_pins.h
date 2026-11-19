/*
 * GPIO plan: bedroom speaker node on the Louder-ESP32-S3 Plus.
 *
 * No carrier board: the Louder on its own 12 V adapter, a microSD module on
 * the LAN headers (as in v1) and one emergency-off button.
 *
 *   LAN headers P1/P2:  microSD (10/11/12/13 + 3V3/GND)
 *   UART hole "TX" (43): emergency-off button to GND through 1 kOhm
 *                        (GPIO43 drives high while the ROM boots)
 */
#ifndef BOARD_PINS_H
#define BOARD_PINS_H

#if !defined(BOARD_LOUDER_S3_PLUS)
#error "bedroom_speaker runs on the Louder: build with -DBOARD_LOUDER_S3_PLUS"
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
#define PIN_AMP_PWDN      17
#define PIN_AMP_FAULT     18

/* Emergency off (3 s long press) and standby */
#define PIN_BUTTON   43

/* 43 is used as a GPIO: no UART console input on this board (USB only). */
#define UART0_PINS_ARE_GPIO 1

#endif /* BOARD_PINS_H */
