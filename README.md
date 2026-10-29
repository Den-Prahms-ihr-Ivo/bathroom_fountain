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

Right now every test fails: the modules in `lib/` are stubs that only compile.

1. Pick a module, read its header (the contract) and its test file.
2. Implement `lib/<module>/<module>.c` until its suite is green.
3. If a test looks wrong or too strict, change the contract deliberately: header and test together.

| Suite | Tests | Covers |
| --- | --- | --- |
| `test_clap_detector` | 26 | CLAP-02 … CLAP-06, CLAP-13 |
| `test_mode_controller` | 17 | BEH-01 … BEH-09, AUD-06, AUD-07 |
| `test_volume` | 19 | AUD-05 |
| `test_hue_api` | 20 | LIGHT-01, LIGHT-02, NFR-09, NFR-10 |

One hue_api test (unparsable bodies) already passes against the stub, because the stub happens to return the expected error; it starts to matter once you implement parsing.

The clap tests generate synthetic scenes (claps, echoes, reverb, rattles, footsteps, shower noise) with deterministic noise, so they run without a microphone. Once the INMP441 is here, real recordings from the bathroom will tune the defaults.
