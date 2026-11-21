# Holy crate

A third use of the same codebase: the couch-table ammunition crate that glows and plays a choir when the lid opens. Louder-ESP32-S3 Plus on a 12.8 V LiFePO4 battery. Online over Wi-Fi whenever it is awake, so music can be uploaded from the web app; asleep (and offline) whenever the lid is closed. Build plan (wiring, perfboard, placement): the "Holy Crate Build Plan" page.

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
| CRATE-12 | The crate pairs with the server like every Wi-Fi node (PAIR-01 … PAIR-08); its pairing button is the encoder's push button held 3 s with the lid open, while the strip pulses. A short press stays "next track". | Must |
| CRATE-13 | The crate is online whenever it is awake (lid open, or the few seconds before sleep). After the lid closes, a running music download or firmware update keeps it awake with light and amplifier off until it is done, for at most 10 min (`busy_max_ms`), then it sleeps; an empty battery always sleeps. Downloads interrupted by sleep continue at the next opening (HTTP Range). | Must |
| CRATE-14 | Status while awake: state, light, playing, volume, battery voltage and state, SD, tracks. Right before deep sleep it publishes a retained "asleep" status with the battery, so the web app shows "asleep · battery 13.0 V" instead of "offline". | Must |
| CRATE-11 | A rotary encoder sets the volume while the lid is open; the volume is kept in NVS for the next opening and capped (`volume` module, as in the bath). Output gain = encoder volume × the fade level from `crate_controller`, so the controller's `volume` setting is 1000 in this app. | Must |

## Modules

| Module | State | Job |
| --- | --- | --- |
| `crate_controller` | implemented (15 tests); 7 tests for CRATE-13 (busy) wait for code | all decisions above |
| `enroll`, `enroll_msg`, `status_json` | stubs (shared with the other Wi-Fi nodes) | pairing; status and "asleep" documents |
| `light_ramp` | stub, 9 tests (shared with the bedside node) | strip fades with gamma, 14-bit |
| `button`, `volume`, `fade_curve`, `playlist`, `tas5825m` | v1 | reused; `volume` turns encoder detents into the user gain |

Platform sources the app lists in `src/app/crate/app.cmake`: `player_task.c`, `mp3_decoder.c`, `amp_io.c`, `events.c`, `input_task.c`, `app_status.c`; once written, also the Wi-Fi ones: `wifi.c`, `mqtt_link.c`, `enroll_task.c`, `music_sync.c`, `ota.c`.

## Wi-Fi on a battery

- **When.** Wi-Fi starts at every wake-up, in parallel with light and music (the opening must not wait for it, CRATE-06). It stops when the crate sleeps.
- **Music arriving while it sleeps.** The server keeps the upload and the pending `cmd/music/sync`; it sends it when the crate's status comes online. The web app shows "waits until the crate is opened".
- **Glue** (`crate_task.c`, to add with the platform code): `crate_controller_busy(cc, music_sync_running() || ota_running(), now)` every tick; before `esp_deep_sleep_start()` publish `status_json_crate_asleep()` retained and disconnect cleanly; the encoder's 3 s hold goes to `enroll_button_held()` (the long press in `input_task` posts it instead of a second "next").
- **Cost.** Wi-Fi draws about 0.3–0.5 W while awake, small next to the strip and the amplifier; asleep it draws nothing extra. A download after closing lasts as long as the files need, at most 10 min.
- **Music on the card.** The crate plays every MP3 in the card's root; the web app manages that folder. The card can still be swapped by hand.
