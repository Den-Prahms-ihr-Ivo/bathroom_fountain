# Architecture

One codebase, one firmware per node, one server. This document describes v2: the bathroom node, the two bedroom nodes, the alarm-game nodes and the interface to the web app. The wire formats are in `docs/PROTOCOL.md`; the order of work is in `docs/IMPLEMENTATION.pdf`.

Requirement IDs (CLAP-02, COLD-05, BED-12, …) refer to the "Bathroom Clap Ambience – System Requirements" doc.

## The system

```
                         Internet
   browser ──HTTPS──▶  server (own VPS)                       separate repository
                       Caddy · Node/TS API · Preact app · SQLite · Mosquitto
                              ▲ MQTT over TLS, port 8883, devices dial out
        ┌─────────────────────┼─────────────────────┐
        │                     │                     │                   home Wi-Fi
   bath node            bedside node  ◀─ESP-NOW─▶  speaker node
   Louder + carrier     ESP32-S3, RTC             Louder, SD card
   Hue · music · cold   alarms · light · game     alarm sound, sleep sounds
        │                     ▲ ESP-NOW
        ▼                     ├──────────────┐
   Hue bridge (LAN)         gun (C3)     targets (C3)          battery, Wi-Fi while awake
```

| Node | Build (`pio run -e …`) | Hardware | Role |
| --- | --- | --- | --- |
| bath | `bath`, `bath_devkit` | Louder-ESP32-S3 Plus, later carrier Rev A | v1 behaviour, cold-shower protocol, Hue, music |
| bedroom speaker | `bedroom_speaker` | Louder-ESP32-S3 Plus, SD module, one button | plays sleep sounds and the alarm sound; holds the armed alarm copy |
| bedside | `bedside` | ESP32-S3 (DevKit prototype, own PCB later), RTC | owns time and alarm schedule, sunrise light, buttons, game master |
| gun | `gun` | ESP32-C3, laser, trigger | coded laser shots |
| target | `target` | ESP32-C3, photodiode, LED ring | lights up, detects hits |
| crate | `crate` | Louder-ESP32-S3 Plus on a 12.8 V LiFePO4 battery | lid open = light and choir; Wi-Fi while awake for music upload (docs/CRATE.md) |

The rule that shapes everything: **the server is for control and management, never for function.** Claps, the cold shower and the alarm work with the server, the internet or the other node gone.

Three links, three jobs:

| Link | Used for | Why this one |
| --- | --- | --- |
| MQTT over TLS, device → server | status, settings, commands, logs, OTA and music announcements | devices dial out, so nothing at home is exposed; one connection carries everything |
| HTTPS, device → server | firmware images and music files (download only) | large files do not belong in MQTT |
| ESP-NOW, node ↔ node | alarm copy, ring/snooze/stop, game commands and hits | works without router or internet, millisecond latency, battery nodes need no Wi-Fi |

**Which nodes have their own server connection.** Bath, bedside, speaker and crate (while awake) each connect to the broker themselves: all three have Wi-Fi anyway (Hue, SNTP, music and OTA downloads), and a node that reports for itself keeps its status, logs and OTA when its partner is off. Only gun and targets have no Wi-Fi; the bedside node reports for them under `shower/<bedside>/node/<name>`. (The roadmap's topic table had the bedside bridged too; this replaces that row.) ESP-NOW and the Wi-Fi station share one radio and therefore one channel: bedside and speaker follow the router's channel, and gun and targets find the bedside node by trying the channels until a HELLO is answered.

## Three layers

| Layer | Where | Depends on | Tested by |
| --- | --- | --- | --- |
| Pure logic: decisions, signal processing, codecs | `lib/<module>/` | C standard library (two modules also cJSON) | `pio test -e native` on your PC |
| Platform: drivers and services every node can use | `src/platform/` | ESP-IDF | on the board |
| App: one node's tasks, pins and wiring of modules | `src/app/<node>/` | ESP-IDF, platform, lib | on the board |

Everything that can be decided without hardware lives in `lib/` and is written test-first. Platform and app code stays thin: it moves data between peripherals and the pure modules.

Rules for `lib/` modules (unchanged from v1):

- No ESP-IDF headers. Time comes in as a parameter (`now_ms`), never from a clock.
- Allocate once at create time; no allocation in processing paths.
- Each module has one header that is its contract; the tests are written against that header only.
- No node-specific behaviour: a module is told what to do through its config, it never asks which node it runs on.

### How one build picks its node

`src/CMakeLists.txt` reads the CMake variable `APP` (PlatformIO passes `-DAPP=bath` through `board_build.cmake_extra_args`; without it the environment variable `BAS_APP`, then `bath`). It compiles every `.c` file in `src/app/<APP>/` plus the platform files that app lists in `src/app/<APP>/app.cmake`:

```cmake
# src/app/bath/app.cmake
set(APP_PLATFORM_SRCS amp_io.c app_settings.c ... wifi.c)
```

So a node only links the platform code it names, a new node is a new folder, and nothing is switched with `#ifdef NODE_…`. The board (Louder or DevKit) is still chosen with `-DBOARD_…`, checked in the app's `board_pins.h`. The configure output prints `bathroom-ambience app: <name>`; that line is the proof the right app was picked.

```
src/
  CMakeLists.txt            picks the app
  platform/                 events, wifi, app_settings (NVS), app_status, console, hue_task,
                            player_task, mp3_decoder, amp_io, mic_task, input_task
                            new in v2: mqtt_link, log_capture, ota, music_sync, fallback_http,
                                       espnow_link, expander_io, rtc, light_pwm
  app/bath/                 main.c, control_task.c, board_pins.h, app.cmake
  app/bedroom_speaker/      skeleton (main.c logs "nothing implemented yet")
  app/bedside/              skeleton
  app/gun/                  skeleton (ESP32-C3)
  app/target/               skeleton (ESP32-C3)
```

## Library modules

"v1" modules came with the first firmware (252 tests); "v2" modules were written test-first against their contracts for the new nodes (263 tests). "v2.1" modules came with the bedside buttons and per-mode volume (61 tests, implemented). "v2.2" modules hold the logic of the bedside, speaker and bath glue and the status documents, so the apps stay thin; "v2.3" modules are pairing with the server and the settings schema. For both, contracts and tests are written and the code is a stub to replace (v2.2: 92 tests, v2.3: 41 tests, red until then).

| Module | Used by | Job | State |
| --- | --- | --- | --- |
| `clap_detector`, `mic_frontend` | bath, bedside | double-clap detection | v1 |
| `mode_controller`, `outputs`, `volume`, `amp_volume`, `fade_curve` | bath | Normal/Ambient, what to switch | v1 |
| `button`, `playlist`, `pcm_mix`, `tas5825m` | bath, speaker | input, music, amplifier | v1 |
| `hue_api`, `hue_sender` | bath, bedside | Hue bridge | v1 |
| `settings` | every Wi-Fi node | the v1 keys (Wi-Fi, Hue, clap, mic) | v1 |
| `kv_table` | every Wi-Fi node | engine for the v2 setting groups | v2 |
| `node_config` | every Wi-Fi node | the groups `net`, `bath`, `bed`, `game` | v2 |
| `settings_sync` | every Wi-Fi node | desired / reported settings as JSON | v2 |
| `mqtt_topics` | every Wi-Fi node | topic strings | v2 |
| `log_ring` | every Wi-Fi node | last log lines in RAM | v2 |
| `wav_clip`, `voice_mix` | bath (speaker later) | voice clip over music, ducking | v2 |
| `cold_protocol` | bath | the cold-shower sequence | v2 |
| `cold_session` | bath | one run as a record: start, real phase lengths, how it ended | v2 |
| `record_outbox` | every Wi-Fi node | records kept until the backend confirms them, also across reboots | v2 |
| `crate_controller` | crate | lid, fades, sleep, battery (docs/CRATE.md) | v2 |
| `clap_features` | bath, bedside | bright and abrupt, or dull and swelling? | v2 |
| `tca9555` | bath | port expander on the carrier | v2 |
| `link_msg`, `link_peer` | bedside, speaker, gun, target | ESP-NOW frames; acks, retries, liveness | v2 |
| `alarm_schedule` | bedside | alarm text form, next ring time | v2 |
| `alarm_ring` | bedside | sunrise → ring → snooze → off | v2 |
| `light_ramp` | bedside | fades and gamma for the LED channels | v2 |
| `armed_alarm` | speaker | rings from its own copy | v2 |
| `game` | bedside | game master, self-test | v2 |
| `bed_modes` | bedside | meditation, mood, sleep, next, stop; nights; candles | v2.1 |
| `mode_volume` | bath, bedside | one encoder volume per music mode | v2.1 |
| `resume_point` | speaker | where the sleep session stopped, as an NVS blob | v2.1 |
| `folder_list` | bedside | the speaker's folders, from `LINK_FOLDER` frames | v2.2 (stub) |
| `bed_audio` | bedside | `bed_modes` commands → `LINK_AUDIO`, scene ids, volume slots and keys, mood folder | v2.2 (stub) |
| `speaker_session` | speaker | `LINK_AUDIO`, alarm and standby → start / save / halt / next, fade level | v2.2 (stub) |
| `resume_restore` | speaker | capture a resume point; find it again in a rebuilt playlist | v2.2 (stub) |
| `bath_volume` | bath | which volume slot the encoder sets, NVS keys | v2.2 (stub) |
| `status_json` | bath, speaker, bedside | status documents and mode / alarm / error / cmd events | v2.2 (stub) |
| `enroll`, `enroll_msg` | every Wi-Fi node | pairing with the server: hello, 3 s hold, credentials | v2.3 (stub) |
| `settings_schema` | every Wi-Fi node | how the web app draws each settings group | v2.3 (stub) |
| `pulse_code` | gun, target | the coded laser burst | v2 |
| `gun_logic` | gun | trigger, magazine, sights, sleep | v2 |
| `target_logic` | target | lit / hit / timeout, sleep plan | v2 |

Dependencies between lib modules: `node_config → kv_table, alarm_schedule`; `mode_volume → volume`; `bed_audio → bed_modes, folder_list, link_msg, node_config, mode_volume`; `folder_list, speaker_session → link_msg, node_config`; `resume_restore → playlist, resume_point`; `bath_volume → mode_controller, mode_volume`; `status_json, settings_schema → cjson`; `enroll → enroll_msg → cjson, node_config`; `settings_schema → kv_table`; `settings_sync → cjson`; `armed_alarm, target_logic, game → link_msg` (for the shared message structs). Everything else stands alone.

## Settings

Three decisions:

1. **Every setting is text key = value**, on every path: console, NVS, fallback page, MQTT. One validation, one format.
2. **`lib/settings` stays as it is.** It holds the v1 keys with hand-written code and 17 green tests. Everything v2 adds is a *group*: a plain struct plus a const table, read and written by the generic engine `kv_table`. Groups are data, not code, so a new key is one table line and one test line. (The roadmap's P0 item "bool type in lib/settings" is covered by this: bools, choices and the new scene slots live in groups.)
3. **Each node picks its groups.** Key names are unique across `lib/settings` and all groups (tested), so the name space stays flat.

| Node | `settings` | `net` | `bath` | `bed` | `game` |
| --- | --- | --- | --- | --- | --- |
| bath | x | x | x | | |
| bedroom speaker | x | x | | | |
| bedside | x | x | | x | x |
| crate | x | x | | | |

`src/platform/app_settings.c` becomes the *store*: it chains `lib/settings` and the node's groups behind four callbacks (`sync_store_t`), persists a changed key to NVS under its name and tells the tasks. Console, fallback page and `settings_sync` all go through the store. Gun and targets have no settings; the bedside sends them what they need (`LINK_GAME_CFG`).

The two cold-shower profiles are two sets of nine keys (`hype_*`, `zen_*`) in the `bath` group; they replace the single `cold_*` set the roadmap listed before the hype/zen switch existed. Alarms are six text keys (`alarm1` … `alarm6`, e.g. `06:45,mtwtf--,on`), so they need no second mechanism.

Server side: the server publishes the full desired set with a version; the device applies what passes validation, stores it and reports what it now holds, including a `rejected` list. Secrets never leave the device (reported as `********`). A local change shows up in the next report and the server adopts it. Details in `docs/PROTOCOL.md`.

## Server link (platform)

| Platform file | Job | Lib modules |
| --- | --- | --- |
| `mqtt_link.c` | esp-mqtt over TLS with the ESP-IDF certificate bundle; subscribes `settings/desired` and `cmd/#`; publishes status (retained, with last will "offline"), reported settings, events, logs; reconnect with backoff | `mqtt_topics`, `settings_sync` |
| `log_capture.c` | `esp_log_set_vprintf` hook → `log_ring` (under a mutex) → publisher follows the ring by sequence number, drops below `log_level`, at most 10 lines/s; sends the reset reason at boot | `log_ring` |
| `enroll_task.c` | pairing while unclaimed: HTTPS hello/confirm with `esp_http_client`, feeds `enroll`, writes or erases the five net keys, starts `mqtt_link` once claimed; the app passes its 3 s button hold in | `enroll`, `enroll_msg`, `node_config` |
| `ota.c` | `cmd/ota` carries URL, version and SHA-256 → `esp_https_ota` into the other slot → reboot → confirm after Wi-Fi + broker are up, else roll back | – |
| `music_sync.c` | `cmd/music/sync` carries a manifest; missing files are pulled over HTTPS in chunks into a temp name, renamed when complete; holds the card mutex per chunk, never while music needs it | – |
| `fallback_http.c` | `http://<device_id>.local` on the LAN, PIN (`web_pin`): status, last log lines, Wi-Fi and server keys, firmware upload, restart. SoftAP `bas-setup-xxxx` when Wi-Fi fails for 2 minutes | `log_ring`, the store |

OTA needs a new partition table (two app slots of 3 MB instead of one factory slot) and therefore one last USB flash; that step is in the implementation guide. Broker credentials (`mqtt_user`, `mqtt_pass`) are typed on the console or the fallback page and live in NVS only.

Commands are handled by the app, not the platform: `mqtt_link` hands `(suffix, payload)` to a callback the app registers, and the app turns it into the same events its buttons produce. That keeps "what a command means" next to the mode logic.

## Bath node

Unchanged from v1 except for the additions marked v2.

### Tasks

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

### Event flow

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


### v2 additions

**Cold shower.** `control_task` gets a third mode beside Normal and Ambient. `cold_protocol` keeps the time; `control_task` is the glue:

```
cold button short, or cmd/cold/start
  Normal:  mode_controller_trigger(BUTTON) must be accepted (not in lockout) → then cold_protocol_start()
  Ambient: cold_protocol_start() directly
each tick while the protocol runs:
  cold_protocol_tick()
  take_scene → hue_task: the profile's scene_warm / scene_cold / scene_after ("" → scene_ambient)
               (a Hue command from mode_controller is dropped while the protocol runs)
  take_cue   → player: voice clip (voice_mix_start) or beep
  outputs.music → player folder: NONE = silence, HYPE = /B, CALM = /C (empty → /A)
  take_end   → FINISHED or ABORTED: mode_controller_trigger(BUTTON) → Normal; ABORTED also voice_mix_stop()
cold button short while running → cold_protocol_next();  held 2 s, or the main button → cold_protocol_abort()
double clap while running → ignored
```

Candles, fountain, amplifier and the volume fade stay with `mode_controller` and `outputs`: for them the protocol is simply "Ambient". The profile is the hype/zen switch if `cold_switch` is on, else the `cold_profile` setting; a flip during a run goes to `cold_protocol_set_profile()` and applies at the next phase change.

**Cold-shower log.** `control_task` runs a `cold_session` beside the protocol: `begin()` with a random id (`esp_random()`) and the SNTP time when the protocol starts, every change of `outputs.phase`, every `next`, and `end()` on `take_end`. A run that reached the cold phase gives a record; its JSON goes into the `record_outbox`, which is saved to NVS (one blob, key `outbox`) after every change. `mqtt_link` feeds the outbox its connection state, publishes what `take_due()` hands out as an event, and passes `cmd/ack` to `record_outbox_ack()`. The backend stores the row and acknowledges (docs/PROTOCOL.md, Records). Wi-Fi or server down only delays the record; nothing in the shower waits for it.

**Voice layer.** At boot and after a music sync the player reads the first 512 bytes of the four clips in `/V` through `wav_clip` and keeps their lengths for `cold_protocol_start()`. `voice_mix_process()` sits between the MP3 decoder and `pcm_mix`; the clip is read from the SD card inside the mixer's callback, in the player task, under the card mutex the player already holds.

**Volume per mode (AUD-13/14).** `control_task` replaces its one `volume_t` with a `mode_volume_t` of `BATH_VOL_COUNT` slots. `bath_volume_mode()` says which slot the encoder sets (cold run, Ambient or its fade-out, or none in Normal) and `bath_volume_key()` its NVS key (`volume` for Ambient, so the saved level carries over; `vol_cold`). The amplifier gain uses `mode_volume_gain()` of the same slot. `mode_volume_take_save()` in a loop until it returns false. Until `bath_volume` is implemented, the bath keeps its single volume.

**Clap gate.** `mic_task` keeps the last 100 ms of samples. When `clap_detector` confirms a double clap it runs `clap_features_measure()` on a window around each onset; both must pass `clap_features_is_clap()`. The features go into the clap event either way, so the thresholds can be tuned from the web app's event list before the gate is switched on.

**Carrier Rev A.** `expander_io.c` owns the `tca9555` driver (I2C on GPIO 1/2, address 0x20) and polls it every 10 ms. Buttons get a *source*: a GPIO now, an expander bit on the carrier, so nothing above `input_task` changes. Pin plan (in `board_pins.h` when the carrier arrives): button port n has its switch on P0(8−2n) and its LED on P0(9−2n); P10 SD detect, P11 status LED; outputs mask `0x02AA`, inverted inputs `0x0155`.

## Bedroom: bedside and speaker

```
bedside (master)                                   speaker (follower)
  rtc + SNTP ─▶ local time                           armed_alarm ◀── LINK_TIME_ALARM (every change, every heartbeat)
  alarm_schedule ─▶ next ring time                        │ rings by itself at the alarm time
  alarm_ring: ARMED ▶ SUNRISE ▶ RINGING ⇄ SNOOZED         ▼
     │ EV_SUNRISE ─▶ light_ramp ─▶ LEDC                player_task: alarm sound, fade-in, volume
     │ EV_RING    ─▶ LINK_RING START ───────────────▶  (already ringing: ignored)
     │ EV_SNOOZE  ─▶ LINK_RING SNOOZE ──────────────▶
     │ EV_STOP    ─▶ LINK_RING STOP (acked, retried) ▶
     ◀── LINK_RING_STATE (emergency button, timeout) ──  button on IO43, 3 s
  game (if game_on) ⇄ gun, targets
```

**One owner, one armed copy.** Only the bedside has an RTC and only there is the schedule edited. The speaker never computes an alarm; it holds the next one as a versioned copy with the bedside's time, counts forward on its own clock and rings when the time comes. A link drop, a dead bedside or a reboot of either node before the alarm therefore still ends in a ringing speaker. The reverse safety nets: maximum ring time on both nodes, the speaker's emergency button, and a STOP that is accepted whatever its version says.

**Time.** The bedside sets the system clock from the RTC at boot and from SNTP when Wi-Fi is up (and writes SNTP time back to the RTC). `alarm_schedule` works in local seconds; the app converts with the `tz` setting (`setenv("TZ")`, `localtime_r`, `mktime`), which also decides what happens inside a daylight-saving jump. After every settings change, every stop and at midnight the app asks `alarm_next()` again, arms `alarm_ring` with the milliseconds left and sends a new `LINK_TIME_ALARM` with a higher version.

**Tasks.** Bedside: `input_task` (big button: short = snooze, long 1.5 s = off; armed switch as a level; the five mode buttons; the encoder), `bed_task` (20 ms tick: alarm_ring, bed_modes, mode_volume, light_ramp, game, link), `hue_task`, `mqtt_link`, Wi-Fi, optional `mic_task`. Speaker: `player_task`, `speaker_task` (100 ms tick: armed_alarm, link, button), `mqtt_link`, Wi-Fi. The speaker's amplifier follows the v1 rule: powered down unless something plays.

**Bedside buttons (BED-40 ... BED-52).** Five buttons on the bedside: meditation, mood, sleep, next, stop; plus the encoder for volume. `bed_modes` decides, `bed_task` carries it out each tick:

```
input_task ── button ──▶ bed_modes_button(btn, night, now)      night = bed_night_id(timegm(localtime)) or UNKNOWN
alarm_ring ── ringing ─▶ bed_modes_alarm()
bed_modes_take_cmd():
  SCENE(which, ms)  → hue_task: scene_med / scene_mood / scene_sleep / scene_normal ("" = leave the lights)
  PLAY(mode, resume, keep, ms) → LINK_AUDIO PLAY: folder from med_folder / mood_folder / sleep_folder,
                                 volume = mode_volume_step(mode), fade_ds = ms / 100
  STOP(ms)          → LINK_AUDIO STOP          NEXT → LINK_AUDIO NEXT
  MOOD_FOLDER_NEXT  → mood_folder = bed_audio_next_folder(speaker folders, mood_folder, "mood_"), stored
                      as a local settings change (reported to the server, so the web app shows it)
  SLEEP_FOLDER_NEXT → the same for sleep_folder and "sleep_"
outputs.candles     → candle GPIO (LR7843 trigger, as in the bath)
outputs.mode        → mode_volume: 0 meditation, 1 mood, 2 sleep, NONE (keys vol_med, vol_mood, vol_sleep)
encoder turn        → mode_volume_turn(mode) → LINK_AUDIO VOLUME with the new step
bed_modes_sleep_night() changed → NVS key sleep_night
```

`bed_modes` is created with `med_min`, `med_fade_s`, `sleep_timer_min`, `sleep_fade_s`, `sleep_candle_min` from the `bed` group; after a settings change it is re-created only while its mode is NONE (a running mode keeps its timings). Mood and sleep folder before a PLAY: `bed_audio_pick_folder()` with the family's prefix keeps the stored folder if it is on the card, else picks the next one of the family and stores it. Sleep folders behave exactly like mood folders: the sleep button while sleep plays moves to the next `sleep_` folder (the timer keeps running; switching is not extending); each folder continues where it stopped. The first sleep press of a night plays the current sleep folder from a random track. The speaker sends its folder list (`LINK_FOLDER`, one frame per folder) after every scan and on HELLO; the bedside keeps up to 32 names. If the speaker is down, PLAY/STOP are dropped with a log line; scenes and candles still work.

Speaker side: `LINK_AUDIO PLAY` with `KEEP` makes the session a kept one: when it stops (STOP, another PLAY, RING start, standby) the speaker writes a `resume_point` blob for that folder (NVS key `resume_key(folder)`, so every sleep folder has its own) with the position at that moment, before any fade. `RESUME` rebuilds the playlist with the stored seed, replays `index` tracks and seeks to `offset` if the name matches. RING start stops any music without a fade. The speaker reports `folders` and `playing` in its status.

**Standby instead of a power switch.** A long press on the speaker's button stops playback and powers the amplifier down; the ESP32 stays up for the alarm. A speaker that is really switched off shows as "offline" on the bedside as soon as `link_peer` reports DOWN.

## ESP-NOW link

`espnow_link.c` (platform) owns the radio: it registers peers, encodes with `link_msg`, runs one `link_peer` per partner and hands decoded messages to the app. Pairing is by MAC address, learned from the first HELLO while the bedside is in pairing mode (button combination or `cmd/link/pair`) and stored in NVS; frames from unknown addresses are dropped. ESP-NOW encryption (PMK/LMK) is switched on once pairing works; the game and the alarm copy carry nothing secret, but a neighbour must not be able to stop or start an alarm.

Reliability is the application's: a frame with `LINK_FLAG_ACK_REQ` is repeated every 60 ms up to five times until the peer's ACK arrives; the receiver acks duplicates but acts once. Heartbeats every 5 s; 15 s of silence is DOWN. All of this is `link_peer`, tested on the PC.

## Alarm game

```
alarm_ring EV_RING (game required)
   └▶ game_start(rules, targets alive, gun alive)
         false → alarm_ring_game_unavailable()   buttons work, display says why
         true  → LINK_TARGET_CMD LIGHT ─▶ target: LED ring on, target_logic LIT
                     gun: trigger ─▶ gun_logic FIRE ─▶ pulse_code burst on the laser
                     target: photodiode edges ─▶ pulse_decoder ─▶ target_logic_shot(strength)
                 ◀── LINK_TARGET_EVT HIT / TIMEOUT ── game_on_hit / game_on_timeout
         GAME_WON → alarm_ring_game_won() → LINK_RING STOP
         a node drops out (link_peer DOWN) → game_on_target_lost / game_on_gun_lost
                                             → GAME_UNAVAILABLE → alarm_ring_game_unavailable()
```

- **Never an alarm you can't stop:** the game can only add a requirement, and four things remove it again: winning, a missing node, the speaker's emergency button, the maximum ring time. Each is a tested path in `alarm_ring` and `game`.
- **Coded light:** one trigger pull is one burst of about 20 ms with gun id, shot number and a check; a continuous beam, a lamp or a flash decodes to nothing. Hit zone = minimum signal strength behind the diffuser.
- **Battery:** gun and targets are in deep sleep almost always. A target wakes on a timer (`target_sleep_seconds()`: until one minute before the alarm, at most 6 h), asks for `LINK_TIME_ALARM`, and sleeps again. The gun wakes on its tilt switch or trigger.
- **Demo, self-test, status:** demo is `game_start()` without an alarm (`cmd/game/demo`, or holding the trigger 3 s); self-test is `game_start_selftest()`; battery and signal of gun and targets come from their heartbeats and are published under `node/<name>`.
- **Later hardware without protocol changes:** a magazine (`LINK_CAP_MAGAZINE`, `gun_logic_magazine()`) and moving targets (`LINK_CAP_MOVING`) announce themselves in HELLO.

## Server and web app (separate repository)

The firmware's contract with the server is `docs/PROTOCOL.md`; copy that file into the server repository and change it in both places together. The stack that was decided:

| Part | Choice | Job |
| --- | --- | --- |
| Reverse proxy | Caddy | HTTPS with automatic certificates, serves the built Preact app, proxies `/api` |
| Broker | Mosquitto | port 8883 with TLS; one account per device, ACL `shower/<id>/#`; one account for the backend |
| Backend | Node.js + TypeScript (Fastify, mqtt.js, better-sqlite3) | login and sessions, device registry, desired settings + versions, log and event store with retention, firmware and music library, WebSocket for live status and logs |
| Frontend | Preact + TypeScript (Vite) | screens per node type |
| Storage | SQLite + a files directory | no separate database server |

The browser talks only to the backend (REST + one WebSocket); the backend is the only MQTT client besides the devices. Screens: devices and status; bath (settings, both cold-shower profiles, scene slots filled from the published Hue scene list with a Test button, clap events with features); bedroom (alarms, sunrise, sleep sounds, game difficulty, node status, demo and self-test); logs (live, filter, download); music library; firmware. The key list each screen edits comes from the reported settings document, so a firmware with a new key shows up without a server change.


## Design decisions of v1 (still valid for the bath node)

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
