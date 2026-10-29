# Architecture

Requirement IDs (CLAP-02, BEH-05, …) refer to the "Bathroom Clap Ambience – System Requirements" doc.

## Two layers

| Layer | Where | Depends on | Tested by |
| --- | --- | --- | --- |
| Pure logic: decisions, signal processing, parsing | `lib/<module>/` | C standard library only | `pio test -e native` on your PC |
| Drivers and tasks: I2S, GPIO, Wi-Fi, NVS, SD | `src/` | ESP-IDF | on the board |

Everything that can be decided without hardware lives in `lib/` and is written test-first. The tasks in `src/` stay thin: they move data between peripherals and the pure modules.

Rules for `lib/` modules:

- No ESP-IDF headers. Time comes in as a parameter (`now_ms`), never from a clock.
- Allocate once at create time; no allocation in processing paths.
- Each module has one header that is its contract; the tests are written against that header only.

## Tasks (target firmware)

| Task | Core | Priority | Job | Requirements |
| --- | --- | --- | --- | --- |
| `mic_task` | 1 | high | Read I2S1 (INMP441, 16 kHz), convert to int16, feed `clap_detector`, post `EVT_CLAP` | CLAP-01, NFR-01 |
| `input_task` | 1 | medium | Rotary encoder (PCNT) and push switch; post `EVT_BUTTON`, `EVT_VOLUME` | BEH-03/04, AUD-05 |
| `control_task` | 1 | medium | Own `mode_controller`; consume events; tick every 20 ms; apply outputs | BEH-*, AUD-06/07 |
| `player_task` | 1 | high | microSD → MP3 decode → I2S0 → TAS5825M | AUD-01/02/09 |
| `hue_task` | 0 | low | Keep the HTTPS connection to the bridge; recall scenes; retry in the background | LIGHT-01/02, NFR-07/09 |

Wi-Fi runs on core 0 by default, so the audio and control work sits on core 1.

## Event flow

```
INMP441 ──I2S1──▶ mic_task ── clap_detector ──┐
encoder + switch ──▶ input_task ──────────────┤  event queue
                                              ▼
                                        control_task
                                      (mode_controller)
          ┌───────────────┬───────────────┬───┴───────────────┐
          ▼               ▼               ▼                   ▼
     Hue queue      player commands   MOSFET GPIOs    TAS5825M PWDN + volume
          │               │           (candles, fountain)
      hue_task       player_task ──I2S0──▶ TAS5825M ──▶ speakers
   (Wi-Fi, HTTPS)    (microSD, MP3)
```

Output volume = user volume (encoder, saved in NVS) × `fade_gain` from `mode_controller`, applied through the TAS5825M's digital volume (AUD-05, AUD-06).

## Modules

| Module | Status | Contract |
| --- | --- | --- |
| `clap_detector` | Tests ready, to implement | `lib/clap_detector/clap_detector.h` |
| `mode_controller` | Tests ready, to implement | `lib/mode_controller/mode_controller.h` |
| `volume` | Tests ready, to implement | `lib/volume/volume.h` |
| `hue_api` | Tests ready, to implement | `lib/hue_api/hue_api.h` |
| `playlist` | Later | folder scan, loop order (AUD-02/03) |
| drivers + tasks | Later | `src/` |

## Design decisions to know about

- **Clap quiet windows scale with the gap** (CLAP-04): `max(300 ms, 1.5 × gap)` before, `max(400 ms, 1.5 × gap)` after. Fixed windows would accept two consecutive footsteps at normal walking pace (about 500 ms apart).
- **Confirmation latency**: a double clap is reported only after the quiet-after window, so 400–900 ms after the second clap depending on the gap. NFR-06 allows 1.5 s.
- **Leaving Ambient**: candles, fountain and music stay on during the 3–5 s fade and switch off together at its end; the amplifier powers down after that.
- **Hue commands are "latest wins"**: if the Hue task falls behind, only the newest scene is sent.
- **Hue errors**: transport failures, 429 and 5xx are retried with backoff (0.5 s doubling to 30 s); 401/403 means the key is gone and needs re-pairing; 404 means the scene was deleted in the Hue app. Neither of the last two is retried.
- **Volume is stored only after the knob rests for 2 s**, and only if the value actually changed, to spare the flash.
- **Hardware notes**: the LR7843 switch modules need at least 6 V on their power side, so the candle module switches the 12 V input of the 3 V buck. The microSD breakout runs from a second buck set to 5 V; its level shifter keeps SPI at 3.3 V.
