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
| `mic_task` | 1 | 10 | Read I2S1 (INMP441, 16 kHz) → `mic_frontend` → `clap_detector`, post `EVT_CLAP`; 1 s + 5 s warm-up | CLAP-01, CLAP-14, NFR-01, BEH-02 |
| `input_task` | 1 | 6 | Every 10 ms: push switch via `button` (short → `EVT_BUTTON`, long → Hue pairing), encoder via PCNT → `EVT_VOLUME` | BEH-04, AUD-05, NFR-10 |
| `control_task` | 1 | 5 | Own `mode_controller`, `volume`, `outputs` and the `tas5825m` driver; tick every 20 ms; carry out the actions `outputs` plans; poll amplifier faults every 2 s | BEH-*, AUD-05/06/07 |
| `player_task` | 0 | 7 | microSD → `playlist` (shuffle) → MP3 (minimp3) → `pcm_mix` (mono, left) → I2S0 → TAS5825M; I2S sends zeros when idle | AUD-01/02/03/09/12 |
| `hue_task` | 0 | 4 | `hue_sender` decides what to send; `hue_api` builds and parses; keep-alive HTTPS to the bridge; pairing; scene list | LIGHT-01/02, NFR-07/09/10 |
| `console_task` | 0 | 2 | `set` / `get` / `show` through `settings` (NVS), `status`, `hue …`, `next`, `rescan`, `reboot`; reads the UART and the native USB port | CLAP-12, NFR-10, NFR-15 |
| Wi-Fi | 0 | (IDF) | `wifi.c`: station from the settings, reconnect 1 s doubling to 30 s | LIGHT-01 |

Wi-Fi runs on core 0, so the real-time input and control work sits on core 1. The player sits on core 0 too: it decodes much faster than real time and ~90 ms of audio in the I2S DMA buffers cover Wi-Fi bursts.

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

Output volume = user volume (encoder, saved in NVS) × `fade_gain` from `mode_controller`, applied through the TAS5825M's digital volume (AUD-05, AUD-06). The fade runs through `fade_curve`, so it changes evenly in dB. `amp_volume` turns the product into the DIG_VOL register code (0.5 dB steps, 0x30 = 0 dB, 0xFF = mute).

Each control tick:

```
want = outputs_want_from_mode(mode outputs, volume gain)      // fade_curve + amp_volume inside
for action in outputs_plan(want):                             // only what changed, fixed order
    candles / fountain  → GPIO (LR7843 trigger)
    music               → player command
    amp power           → tas5825m_power_up() / tas5825m_power_down()
    volume              → tas5825m_set_volume()
    success → outputs_done(action); failure → stop, it is planned again next tick
```

The amplifier register is therefore written only when the code changes, and a failed I2C write is retried on the next tick without extra code.

## Modules

| Module | Status | Contract |
| --- | --- | --- |
| all `lib/` modules | Done, 252 native tests | `lib/<module>/<module>.h` |
| `src/` tasks and drivers | Done; compiled for both boards with ESP-IDF 5.5 | see the task table |
| `src/third_party/minimp3` | Vendored MP3 decoder (CC0) | `src/mp3_decoder.h` wraps it |

## Design decisions to know about

- **Clap quiet windows scale with the gap** (CLAP-04): `max(300 ms, 1.5 × gap)` before, `max(400 ms, 1.5 × gap)` after. Fixed windows would accept two consecutive footsteps at normal walking pace (about 500 ms apart).
- **Confirmation latency**: a double clap is reported only after the quiet-after window, so 400–900 ms after the second clap depending on the gap. NFR-06 allows 1.5 s.
- **Leaving Ambient**: candles, fountain and music stay on during the 3–5 s fade and switch off together at its end; the amplifier powers down after that.
- **Hue commands are "latest wins"**: if the Hue task falls behind, only the newest scene is sent.
- **Hue errors**: transport failures, 429 and 5xx are retried with backoff (0.5 s doubling to 30 s); 401/403 means the key is gone and needs re-pairing; 404 means the scene was deleted in the Hue app. Neither of the last two is retried.
- **Volume is stored only after the knob rests for 2 s**, and only if the value actually changed, to spare the flash.
- **Mic high-pass is a tuning knob** (CLAP-14): 20 Hz removes the DC offset; 100–300 Hz also weakens footstep and door thumps relative to claps. `tools/clap_lab.py tune` decides.
- **Hue retries never give up**: the bridge must end up in the current mode's scene whenever it is reachable again. A command submitted while another is in flight supersedes it instead of queueing behind its retries.
- **One settings table for console, NVS and the lab**: NVS stores each value as the same text the console accepts, and boot feeds it back through `settings_set()`, so a corrupt value falls back to the default instead of being used.
- **Button**: SHORT fires on release (latency about 100–200 ms, inside NFR-06) and is the only way to leave Ambient besides the 30-minute auto exit; LONG (3 s, physically held) starts Hue pairing.
- **One speaker, mono (AUD-01)**: stereo MP3s are averaged as (L+R)/2 in 32 bit (no clipping, truncation toward zero) and sent to the left output; the right output carries silence. `PCM_RIGHT_COPY` exists for a second speaker later.
- **Mute only at fade 0**: any gain above zero maps to at most 0xFE (-103 dB), so the amplifier is muted exactly when the fade ends, never early. The module never boosts above 0 dB.
- **Fades even in dB**: `fade_gain` from `mode_controller` is treated as progress; `fade_curve` maps it to a straight line in dB from -50 dB to 0 dB. A linear gain would still be at -6 dB halfway through a fade-out and drop off audibly at the end.
- **Amplifier always starts muted**: `tas5825m_power_up()` sets DIG_VOL to mute before play, and `outputs` always sends the volume right after a power-up. The driver also refuses to boost: codes below 0x30 are written as 0x30.
- **Drivers are pure C with injected I/O**: `tas5825m` gets I2C, PDN and delay as callbacks, so the power sequence and error handling are tested on the PC; the ESP-IDF glue in `src/` is a few lines. Precondition: I2S clocks run before power-up, so the player keeps I2S0 enabled and sends zeros when idle.
- **Order of output changes**: music stops before the amplifier powers down, and starts only after power-up and volume. On any I2C error during power-up the driver pulls PDN low, so a half-started amplifier never stays on.
- **Unplayable files** are skipped for the session (AUD-12); an empty or unreadable card never blocks the rest of Ambient (AUD-09).
- **Hardware notes**: the LR7843 switch modules need at least 6 V on their power side, so the candle module switches the 12 V input of the 3 V buck. The microSD module used is a 3.3 V-only module (pull-ups and capacitors on board) and runs from the board's 3V3, so the 5 V buck is not needed for it.
- **Mic warm-up in two phases** (BEH-02): 1 s with the detector off (the mic's startup transient), then 5 s with the detector running but its detections ignored, so its background estimate (rising ~10 dB/s) has caught up with a running shower. The same happens after a `clap_*` / `mic_*` setting changes.
- **Amplifier address is detected**: after PDN goes high, the first I2C transfer probes 0x4C–0x4F (TAS5825M) and 0x2C–0x2F (TAS5805M, same registers). `status` shows what answered. A failed amplifier action is retried every 2 s, not every tick.
- **Amplifier faults** are read every 2 s while it is on; any fault is logged and cleared (the TAS5825M then retries by itself).
- **Hue HTTPS without certificate check**: the bridge's certificate is self-signed; the connection is encrypted but the server is not verified (`CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY`). Acceptable inside the home network.
- **Hue settings are read at every request**, so `set hue_host`, `hue_key` or a scene id takes effect without a restart. A missing scene id drops the command with a log line; a missing key means "needs pairing" until pairing succeeds.
- **Pairing**: long press (3 s) or `hue pair`; tries every second for 30 s until the bridge's link button has been pressed, then stores the key and sends the waiting command.
- **Player on the DevKit is a dry run**: without amplifier pins it decodes and paces in real time but outputs nothing, which checks card, playlist and decoder on the bench.
- **ID3v2 tags are skipped** before decoding, so large embedded cover images cost nothing.
- **Console on both USB ports**: output is mirrored to the native USB port by the IDF; `console_task` also reads input there, so the Louder works whichever port it exposes.
