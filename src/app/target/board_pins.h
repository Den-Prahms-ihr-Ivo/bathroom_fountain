/*
 * GPIO plan: target (alarm game).
 *
 * PROVISIONAL: pins for a prototype on an ESP32-C3-DevKitM-1; nothing is
 * built yet. Avoided: 2/8/9 (strapping), 18/19 (USB), 20/21 (UART0 console).
 */
#ifndef BOARD_PINS_H
#define BOARD_PINS_H

#define PIN_BATTERY_ADC   0   /* battery voltage through a divider (ADC1_CH0)   */
#define PIN_PHOTO_LEVEL   1   /* photodiode amplifier, analog: room light level  */
#define PIN_PHOTO_PULSE   3   /* photodiode comparator output: edges of a shot   */
#define PIN_LED_RING      4   /* LED ring data (WS2812-type)                      */
#define PIN_SERVO         5   /* reserved: moving targets                         */

#endif /* BOARD_PINS_H */
