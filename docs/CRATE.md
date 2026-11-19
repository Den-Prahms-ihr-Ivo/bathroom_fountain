# Holy crate

A third use of the same codebase: the couch-table ammunition crate that glows and plays a choir when the lid opens. Louder-ESP32-S3 Plus on a 12.8 V LiFePO4 battery, no Wi-Fi, no server. Build plan (wiring, perfboard, placement): the "Holy Crate Build Plan" page.

Build: `pio run -e crate` (app folder `src/app/crate/`, pins in its `board_pins.h`).

## Requirements

| ID | Requirement | Priority |
| --- | --- | --- |
| CRATE-01 | Opening the lid fades the light in (default 60 % over 1.5 s) and starts the music from the first track; closing fades light and music out (3 s) and then turns the amplifier off. | Must |
| CRATE-02 | The lid is a level: the crate follows it. Reopening during the fade-out turns the fades around without restarting the music. | Must |
| CRATE-03 | The door contact is debounced (50 ms, *tunable*). | Must |
| CRATE-04 | A lid left open dims the light (15 %) and stops the music after 15 min (*tunable*, 0 = never). | Should |
| CRATE-05 | Closed crate: deep sleep, woken by the lid (ext0 on GPIO 1); the amplifier's PWDN is held low and the LED gate pulled low through sleep. | Must |
| CRATE-06 | From lid open to light within about 0.5 s and to sound within about 1 s, from deep sleep. | Should |
| CRATE-07 | Battery voltage is measured on GPIO 2 (1 MΩ / 220 kΩ divider). Below 12.8 V for 10 s: battery low. Below 12.4 V for 10 s: battery empty. 300 mV hysteresis on the way back up. | Must |
| CRATE-08 | Battery empty: the crate does not play; opening the lid only blinks the strip. If it runs empty while open, it fades out as if the lid had closed. | Must |
| CRATE-09 | A main switch disconnects everything but the charging jack; charging works with the switch off. | Must |
| CRATE-10 | Light and volume levels, fade times, max-open time and battery thresholds are settings with the ranges in `lib/crate_controller/crate_controller.h`. | Should |
| CRATE-11 | A rotary encoder sets the volume while the lid is open; the volume is kept in NVS for the next opening and capped (`volume` module, as in the bath). Output gain = encoder volume × the fade level from `crate_controller`, so the controller's `volume` setting is 1000 in this app. | Must |

## Modules

| Module | State | Job |
| --- | --- | --- |
| `crate_controller` | stub, 15 tests | all decisions above |
| `light_ramp` | stub, 9 tests (shared with the bedside node) | strip fades with gamma, 14-bit |
| `button`, `volume`, `fade_curve`, `playlist`, `tas5825m` | v1 | reused; `volume` turns encoder detents into the user gain |

Platform sources the app will list in `src/app/crate/app.cmake`: `player_task.c`, `mp3_decoder.c`, `amp_io.c`, `events.c`, `input_task.c` (encoder over PCNT and its push button, unchanged from the bath).

The encoder's push button has no job yet.
