/*
 * GPIO plan: bedside node.
 *
 * PROVISIONAL: these are the pins for the prototype on an ESP32-S3-DevKitC-1.
 * The bedside PCB does not exist yet; when it is designed, give it its own
 * section here (like BOARD_LOUDER_S3_PLUS / BOARD_DEVKITC1 in the bath app).
 * Avoided on the DevKit: 0/3/45/46 (strapping), 19/20 (USB), 35-37 (PSRAM),
 * 38/48 (RGB LED), 43/44 (UART0 console).
 */
#ifndef BOARD_PINS_H
#define BOARD_PINS_H

#if defined(BOARD_DEVKITC1)

/* I2C bus: RTC (PCF8563 or DS3231), later an optional display */
#define PIN_I2C_SDA   8
#define PIN_I2C_SCL   9

/* Light channels, LEDC PWM through MOSFETs (warm white, cold white, 2 spare) */
#define PIN_LIGHT_CH1  4
#define PIN_LIGHT_CH2  5
#define PIN_LIGHT_CH3  6
#define PIN_LIGHT_CH4  7

/* Rotary encoder: volume, and its switch */
#define PIN_ENC_A    1
#define PIN_ENC_B    2
#define PIN_ENC_SW  42

/* Big button: short = snooze, long = alarm off */
#define PIN_BUTTON_ALARM  47
/* Latching switch: alarms armed (closed to GND) / off */
#define PIN_SWITCH_ARMED  21

/* Microphone, INMP441 (optional: claps at the bedside) */
#define PIN_MIC_SCK   41
#define PIN_MIC_WS    40
#define PIN_MIC_SD    39

#else
#error "Build with -DBOARD_DEVKITC1 (prototype); add the bedside PCB here once it exists"
#endif

#endif /* BOARD_PINS_H */
