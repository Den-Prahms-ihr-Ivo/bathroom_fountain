# bathroom-ambience

Double-clap controlled bathroom ambience on a Sonocotta Louder-ESP32-S3 Plus: Hue scene, LED candles, music from microSD, fountain. From v2 the same codebase also builds the bedroom nodes (speaker, bedside) and the alarm-game nodes (gun, target).

- `docs/ARCHITECTURE.md`: nodes, layers, modules, how the pieces talk
- `docs/PROTOCOL.md`: MQTT topics and payloads, ESP-NOW frames (shared with the server repository)
- `docs/IMPLEMENTATION.pdf`: what to implement in which order

## Firmware: first start

The bath firmware (`src/app/bath/` + `src/platform/`) is complete for both boards. On the DevKit, LEDs stand in for the outputs, the amplifier driver only logs, and music plays "dry" (decoded and paced, no sound). On the Louder everything is real.

**1. Build and flash.** Delete `sdkconfig.bath_devkit` / `sdkconfig.bath` once, so they are created again from `sdkconfig.defaults` (bigger app partition, long file names on the SD card, Hue HTTPS settings). Then:

```
pio run -e bath_devkit -t upload     # or: -e bath (the Louder)
pio device monitor -e bath_devkit --echo
```

**2. Wi-Fi** (2.4 GHz only). On the console:

```
set wifi_ssid YourNetwork
set wifi_pass YourPassword
```

It reconnects at once; `status` shows the IP.

**3. Hue Bridge.** Give the bridge a fixed address in your router, then:

```
set hue_host 192.168.1.50
```

Press the round link button on the bridge, then within 30 s hold the device button for 3 s (or type `hue pair`). The key is saved as `hue_key`.

**4. Scenes.** Create a "normal" and an "ambient" scene in the Hue app (e.g. lights at 1 % for ambient). Then:

```
hue scenes                           # lists id, name and room
set scene_normal  <id>
set scene_ambient <id>
hue ambient                          # test: recall it now
hue normal
```

**5. Music.** FAT32 microSD card, MP3 files in the root folder. `status` shows how many were found; `rescan` after swapping the card; `next` skips a track.

**6. Use it.** Double clap: Ambient. Button: back to Normal. Turning the knob changes the volume (saved after 2 s at rest). Ambient ends by itself after 30 minutes.

Console commands: `help`, `status`, `show`, `get <key>`, `set <key> <value>`, `hue pair|scenes|normal|ambient`, `next`, `rescan`, `reboot`. Typing works on the native USB port (on the DevKit also on the UART port; on the Louder, UART0's pins 43/44 carry the encoder). Where the free Louder pins are: `src/app/bath/board_pins.h` and sheet 2 of the wiring PDF.

## Environments

| Env | Board | Use |
| --- | --- | --- |
| `native` | your PC | unit tests for `lib/` |
| `bath` | Louder-ESP32-S3 Plus | bathroom firmware (was `louder`) |
| `bath_devkit` | ESP32-S3-DevKitC-1 | bathroom firmware on the bench (was `devkit`) |
| `bedroom_speaker` | Louder-ESP32-S3 Plus | bedroom speaker node (skeleton) |
| `bedside` | ESP32-S3-DevKitC-1 | bedside node prototype (skeleton) |
| `gun`, `target` | ESP32-C3-DevKitM-1 | alarm-game nodes (skeletons) |

Each environment passes `-DAPP=<node>` to CMake; `src/CMakeLists.txt` then builds `src/app/<node>/` plus the platform files listed in that folder's `app.cmake`. The configure output prints `bathroom-ambience app: <node>`.

## Test-first workflow

```
pio test -e native                               # all suites
pio test -e native -f test_clap_detector         # one suite
```

A new module starts as a stub that only compiles, so its tests fail until it is implemented. All suites below are green.

1. Pick a module, read its header (the contract) and its test file.
2. Implement `lib/<module>/<module>.c` until its suite is green.
3. If a test looks wrong or too strict, change the contract deliberately: header and test together.

| Suite | Tests | Covers | Round |
| --- | --- | --- | --- |
| `test_clap_detector` | 26 | CLAP-02 … CLAP-06, CLAP-13 | 1 (done) |
| `test_mode_controller` | 31 | BEH-01 … BEH-10, AUD-06, AUD-07 (clap only enters Ambient; auto exit after 30 min) | 1, 5, 7 |
| `test_volume` | 21 | AUD-05 | 1 (done) |
| `test_hue_api` | 20 | LIGHT-01, LIGHT-02, NFR-09, NFR-10 | 1 (done) |
| `test_mic_frontend` | 13 | CLAP-01, CLAP-14 | 2 |
| `test_playlist` | 23 | AUD-02, AUD-03, AUD-09, AUD-12 | 2 |
| `test_hue_sender` | 16 | LIGHT-02, NFR-09 | 2 |
| `test_button` | 12 | BEH-03, BEH-04 | 2 |
| `test_settings` | 17 | CLAP-12, NFR-10, NFR-15 | 2 |
| `test_pcm_mix` | 10 | AUD-01 (mono downmix to the left channel) | 3 |
| `test_amp_volume` | 10 | AUD-05, AUD-06, AUD-07 (TAS5825M volume register) | 3 |
| `test_fade_curve` | 11 | AUD-06 (fades even in dB) | 4 |
| `test_tas5825m` | 19 | AUD-05/06/07, amplifier power sequence and faults (fake I2C) | 4 |
| `test_outputs` | 23 | BEH-05/06/07, AUD-05/06/07 (what to switch, order, retries) | 4 |

v2 suites (implemented, green). Order and reasons: `docs/IMPLEMENTATION.pdf`.

| Suite | Tests | Covers | Phase |
| --- | --- | --- | --- |
| `test_kv_table` | 12 | CFG-01 … CFG-04 (engine for the setting groups) | P0 |
| `test_alarm_schedule` | 9 | BED-01 … BED-04 (needed by `node_config`) | P0 / P6 |
| `test_node_config` | 7 | CFG-05 … CFG-08, COLD-14, SRV-02, BED-05, GAME-10 | P0 |
| `test_wav_clip` | 8 | COLD-09, COLD-12 | P1 |
| `test_voice_mix` | 14 | COLD-10, COLD-11 | P1 |
| `test_cold_protocol` | 14 | COLD-01 … COLD-08, COLD-13 | P1 |
| `test_cold_session` | 10 | COLD-15 … COLD-17 | P1 |
| `test_log_ring` | 9 | LOG-01 … LOG-04 | P2 |
| `test_record_outbox` | 11 | COLD-18, COLD-19 | P2 |
| `test_mqtt_topics` | 8 | SRV-03, SRV-04 | P2 |
| `test_settings_sync` | 12 | SRV-05 … SRV-09 | P2 |
| `test_clap_features` | 9 | CLAP-20 … CLAP-23 | P4 |
| `test_tca9555` | 12 | HW-20, HW-21 | P5 |
| `test_link_msg` | 12 | LINK-01 … LINK-04 | P6 |
| `test_link_peer` | 14 | LINK-05 … LINK-09 | P6 |
| `test_armed_alarm` | 14 | BED-10 … BED-16 | P6 |
| `test_alarm_ring` | 15 | BED-20 … BED-28, GAME-01, GAME-08 | P6 |
| `test_light_ramp` | 9 | BED-30 … BED-33 | P6 |
| `test_pulse_code` | 10 | GAME-20 … GAME-23 | P7 |
| `test_gun_logic` | 10 | GAME-30 … GAME-36 | P7 |
| `test_target_logic` | 11 | GAME-40 … GAME-46 | P7 |
| `test_game` | 18 | GAME-01 … GAME-09, GAME-11 … GAME-15 | P7 |
| `test_crate_controller` | 15 | CRATE-01 … CRATE-10 (docs/CRATE.md) | crate |

The clap tests generate synthetic scenes (claps, echoes, reverb, rattles, footsteps, shower noise) with deterministic noise, so they run without a microphone. Real recordings from the bathroom tune the defaults: see `tools/README.md`.

## Tools

| Tool | What it does |
| --- | --- |
| `tools/recorder/` | Separate PlatformIO project: DevKitC-1 firmware that streams the INMP441 to the PC |
| `tools/capture.py` | Live level meter, and labelled recordings with clap prompts (WAV + JSON) |
| `tools/clap_lab.py` | Replays recordings through *your* `mic_frontend` + `clap_detector`, scores them, tunes settings, plots |
| `tools/selftest.py` | Checks the capture protocol and the lab pipeline |
| `tools/prompt_report.py` | Per prompt window: both clap peaks, level difference, gap, drop, and which default rule would reject the pair |

Step-by-step for Monday: `tools/README.md`.
