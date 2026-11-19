/*
 * GPIO plan: laser gun (alarm game).
 *
 * PROVISIONAL: pins for a prototype on an ESP32-C3-DevKitM-1; nothing is
 * built yet. Avoided: 2/8/9 (strapping), 18/19 (USB), 20/21 (UART0 console).
 */
#ifndef BOARD_PINS_H
#define BOARD_PINS_H

#define PIN_BATTERY_ADC   0   /* battery voltage through a divider (ADC1_CH0) */
#define PIN_TRIGGER       3   /* trigger switch to GND                        */
#define PIN_LASER         4   /* laser module driver, short coded pulses only  */
#define PIN_SIGHT_LED     5   /* glowing iron sights, LEDC PWM                 */
#define PIN_TILT          6   /* tilt switch / accelerometer INT: picked up    */
#define PIN_MAGAZINE      7   /* reserved: magazine inserted (reed switch)     */

#endif /* BOARD_PINS_H */
