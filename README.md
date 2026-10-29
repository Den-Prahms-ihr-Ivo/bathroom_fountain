# bathroom-ambience

Double-clap controlled bathroom ambience on a Sonocotta Louder-ESP32-S3 Plus: Hue scene, LED candles, music from microSD, fountain.

See `docs/ARCHITECTURE.md` for the task layout and module plan.

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
| `test_mode_controller` | 17 | BEH-01 … BEH-09, AUD-06, AUD-07 | 1 (done) |
| `test_volume` | 21 | AUD-05 | 1 (done) |
| `test_hue_api` | 20 | LIGHT-01, LIGHT-02, NFR-09, NFR-10 | 1 (done) |
| `test_mic_frontend` | 13 | CLAP-01, CLAP-14 | 2 |
| `test_playlist` | 23 | AUD-02, AUD-03, AUD-09, AUD-12 | 2 |
| `test_hue_sender` | 16 | LIGHT-02, NFR-09 | 2 |
| `test_button` | 12 | BEH-03, BEH-04 | 2 |
| `test_settings` | 17 | CLAP-12, NFR-10, NFR-15 | 2 |
| `test_pcm_mix` | 10 | AUD-01 (mono downmix to the left channel) | 3 |
| `test_amp_volume` | 10 | AUD-05, AUD-06, AUD-07 (TAS5825M volume register) | 3 |

Suggested order for round 2: `mic_frontend` first (small, and the clap lab needs it), then `button`, `hue_sender`, `settings`, `playlist`.

Round 3 (`pcm_mix`, `amp_volume`) is small and independent; either order works.

The clap tests generate synthetic scenes (claps, echoes, reverb, rattles, footsteps, shower noise) with deterministic noise, so they run without a microphone. Real recordings from the bathroom tune the defaults: see `tools/README.md`.

## Tools

| Tool | What it does |
| --- | --- |
| `tools/recorder/` | Separate PlatformIO project: DevKitC-1 firmware that streams the INMP441 to the PC |
| `tools/capture.py` | Live level meter, and labelled recordings with clap prompts (WAV + JSON) |
| `tools/clap_lab.py` | Replays recordings through *your* `mic_frontend` + `clap_detector`, scores them, tunes settings, plots |
| `tools/selftest.py` | Checks the capture protocol and the lab pipeline |

Step-by-step for Monday: `tools/README.md`.
