# Clap tuning tools

Record real bathroom sounds with the DevKitC-1, then replay them through **your** `mic_frontend` and
`clap_detector` to measure and tune detection before the firmware exists.

```
INMP441 ─I2S─▶ DevKitC-1 (tools/recorder) ─USB 921600 baud─▶ capture.py ─▶ recordings/*.wav + *.json
                                                                                  │
                         your lib/mic_frontend + lib/clap_detector ◀── clap_lab.py ┘ (replay, tune, plot)
```

## 0. Setup (once, today)

Python 3.9 or newer, then from the project folder:

```
python -m pip install -r tools/requirements.txt
python tools/selftest.py
```

Step 1 of the selftest checks the capture protocol against the real C encoder of the recorder firmware.
Step 2 is skipped until `mic_frontend` and `clap_detector` are implemented.

## 1. Before the mic arrives

1. Implement `lib/mic_frontend` (`pio test -e native -f test_mic_frontend`).
2. Run `python tools/selftest.py` again: step 2 now runs your detector over synthetic scenes.
3. Play with the lab:

```
python tools/clap_lab.py synth recordings/synth
python tools/clap_lab.py replay recordings/synth -v
python tools/clap_lab.py plot recordings/synth
```

Synthetic scenes use the same signal model as the unit tests, so expect close to 100 % and 0 false
triggers. They prove the pipeline works; only real recordings say anything about your bathroom.

## 2. Flash the recorder (Monday)

1. Wire the INMP441 to the DevKitC-1 as in Figure 3 of the build guide (board unplugged).
2. Plug in the DevKitC-1 by its **UART** port and flash the recorder. It is a separate small PlatformIO
   project inside `tools/recorder`:

```
pio run -d tools/recorder -t upload
```

   This replaces the unit-test firmware on the DevKitC-1; `pio test -e devkit` flashes the tests again later.
   Not verified: this firmware was written against the ESP-IDF 5.3 I2S API but could not be built before
   delivery. If it does not compile, send me the error.

3. Close every serial monitor (PlatformIO's included), then check the signal:

```
python tools/capture.py ports
python tools/capture.py monitor --port COM5        # your port; Linux /dev/ttyUSB0, macOS /dev/cu.usbserial-*
```

| What you see | Meaning |
| --- | --- |
| rms about −75 … −60 dBFS when quiet, peaks −25 … −5 dBFS when you clap | Working |
| "constant value" warning | Wiring, L/R not on GND, or wrong slot: add `-DMIC_SLOT_RIGHT` to `build_flags` in `tools/recorder/platformio.ini` and flash again |
| "No audio frames" | Wrong port, recorder not flashed, or another program holds the port |
| "lost frames" counting up | USB cable or hub; use a short cable directly on the PC |

## 3. Record

Put the DevKitC-1 and mic **where the final module will sit** (on the shelf, mic facing the room). Position
changes everything about levels and echoes.

Prompted sessions (you clap twice at each prompt):

```
python tools/capture.py record --port COM5 --label double_clap --prompts 10 --distance 1
python tools/capture.py record --port COM5 --label double_clap --prompts 10 --distance 3
python tools/capture.py record --port COM5 --label double_clap_shower --prompts 10 --distance 2 --note "shower running"
```

Negative sessions (no clapping; every detection in them is a false trigger). Make the sound naturally and
repeatedly; Ctrl+C ends early and keeps the file:

```
python tools/capture.py record --port COM5 --label door --seconds 120 --note "open, close, one slam"
```

Recording plan, about 45 minutes in total:

| Label | Kind | How | Checks |
| --- | --- | --- | --- |
| `double_clap` | 3 × 10 prompts | at 1 m, 2 m and 3 m, normal claps | CLAP-07 |
| `double_clap_soft` | 1 × 10 prompts | quieter claps, 2 m | headroom |
| `double_clap_shower` | 2 × 10 prompts | shower running, 2 m | CLAP-09 |
| `single_clap`, `triple_clap` | 90 s each, no prompts | clap once (or three times) every few seconds | must **not** trigger |
| `door` | 2 min | open, close, slam, handle | CLAP-08 |
| `toilet_lid` | 2 min | close, drop | CLAP-08 |
| `flush`, `tap`, `shower` | 2–5 min each | normal use | CLAP-06/08 |
| `footsteps` | 2 min | walk around, turn, stop | CLAP-04 |
| `hair_dryer`, `talking`, `objects` | 2 min each | dryer on/off, talking or singing, put down bottles and cups | CLAP-08 |

Keep one session per label in `recordings/holdout/` and do not tune on it.

## 4. Analyse and tune

```
python tools/clap_lab.py replay recordings/
python tools/clap_lab.py replay recordings/20261102-153000_door.wav -v
python tools/clap_lab.py plot recordings/ --out plots
python tools/clap_lab.py tune recordings/ --grid clap_onset_db=9,12,15 --grid mic_hp_hz=20,150,300 --grid clap_min_peak_dbfs=-45,-40,-35
python tools/clap_lab.py replay recordings/holdout/ --set clap_onset_db=12 --set mic_hp_hz=150 --set clap_min_peak_dbfs=-40
```

`tune` ranks by fewest false triggers, then best detection, then fewest changes from the defaults, and prints
`set` lines for the device console. The same keys work in `--set`.

| Symptom | Settings to try |
| --- | --- |
| Misses at 3 m or with soft claps | lower `clap_min_peak_dbfs` (−45), lower `clap_onset_db` |
| Footsteps, doors, lids trigger | raise `mic_hp_hz` (150–300), lower `clap_level_diff_db`, raise `clap_min_peak_dbfs` |
| Misses with the shower on | lower `clap_onset_db` |
| Fast double claps missed | lower `clap_min_gap_ms` |
| Slow double claps missed | raise `clap_max_gap_ms` (and expect more latency) |
| Misses in a very echoey room | lower `clap_drop_db` |

If the detector itself needs a change rather than a setting, change the contract and the unit test first
(add a synthetic scene that reproduces the failure), then the code. The recordings show you which scene
to build.

## Privacy

Audio is recorded only to your PC, into `recordings/`, which `.gitignore` keeps out of git (NFR-01). The
final firmware never stores or transmits audio.
