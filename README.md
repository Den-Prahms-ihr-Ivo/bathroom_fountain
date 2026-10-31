# bathroom-ambience

Double-clap controlled bathroom ambience on a Sonocotta Louder-ESP32-S3 Plus: Hue scene, LED candles, music from microSD, fountain.

See `docs/ARCHITECTURE.md` for the task layout and module plan.

## Firmware: first start

The firmware in `src/` is complete for both boards. On the DevKit, LEDs stand in for the outputs, the amplifier driver only logs, and music plays "dry" (decoded and paced, no sound). On the Louder everything is real.

**1. Build and flash.** Delete `sdkconfig.devkit` / `sdkconfig.louder` once, so they are created again from `sdkconfig.defaults` (bigger app partition, long file names on the SD card, Hue HTTPS settings). Then:

```
pio run -e devkit -t upload          # or: -e louder
pio device monitor -e devkit --echo
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

Console commands: `help`, `status`, `show`, `get <key>`, `set <key> <value>`, `hue pair|scenes|normal|ambient`, `next`, `rescan`, `reboot`. Typing works on the UART port and on the native USB port.

## Environments

| Env | Board | Use |
| --- | --- | --- |
| `native` | your PC | unit tests for `lib/` |
| `louder` | Louder-ESP32-S3 Plus | target firmware |
| `devkit` | ESP32-S3-DevKitC-1 | bench prototyping (mic recorder, clap tuning) |

## Test-first workflow

```
pio test -e native                               # all suites
pio test -e native -f test_clap_detector         # one suite
```

New modules start as stubs that only compile, so their tests fail until you implement them.

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

Suggested order for round 2: `mic_frontend` first (small, and the clap lab needs it), then `button`, `hue_sender`, `settings`, `playlist`.

Round 3 (`pcm_mix`, `amp_volume`) is small and independent; either order works.

Round 4: `fade_curve` first, because `test_outputs` also uses it (and `amp_volume`); then `outputs` and `tas5825m` in either order. `test_tas5825m` brings its own fake board, so the driver runs on the PC.

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
