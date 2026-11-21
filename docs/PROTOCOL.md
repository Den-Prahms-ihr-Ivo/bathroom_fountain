# Protocol

The contract between the firmware and everything it talks to: the server (MQTT, HTTPS) and the other nodes (ESP-NOW). This file exists in the firmware repository and in the server repository; change it in both together. Byte layouts and topic strings are pinned by the native tests of `lib/mqtt_topics`, `lib/settings_sync` and `lib/link_msg`.

## 1. Device ↔ server: MQTT

- Broker: Mosquitto on the server, port 8883, TLS. The device checks the certificate against the ESP-IDF certificate bundle.
- One broker account per device (`mqtt_user` / `mqtt_pass`), ACL: read and write `shower/<device_id>/#` only. The backend has its own account for `shower/#`.
- `device_id`: 1–24 characters of `a-z 0-9 -`. MQTT client id = `device_id`.
- Last will: `shower/<id>/status` = `{"online":false}`, retained, QoS 1.
- All payloads are UTF-8 JSON on one line. Unknown members must be ignored by both sides.
- The device's MQTT buffer is 4096 bytes: no payload may be longer.

| Topic | Dir | QoS | Retained | Payload |
| --- | --- | --- | --- | --- |
| `shower/<id>/status` | D→S | 1 | yes | status document, on connect, on change, at most every 2 s, and every 60 s |
| `shower/<id>/settings/desired` | S→D | 1 | yes | desired settings |
| `shower/<id>/settings/reported` | D→S | 1 | yes | reported settings, after every apply and every local change |
| `shower/<id>/settings/schema/<group>` | D→S | 1 | yes | how to draw that group's form, on every connect (`lib/settings_schema`) |
| `shower/<id>/cmd/<name>` | S→D | 1 | no | command, see below |
| `shower/<id>/event` | D→S | 1 | no | one event |
| `shower/<id>/log` | D→S | 0 | no | one or more log lines |
| `shower/<id>/hue/scenes` | D→S | 1 | yes | scene list of the bridge |
| `shower/<id>/node/<name>` | D→S | 1 | yes | status of a gun or target the bedside node reports for |

### Status

```json
{"online":true,"node":"bath","fw":"2.0.0","built":"2026-11-15T14:02:11Z","boot":17,"reset":"poweron",
 "uptime_s":8123,"wifi_rssi":-58,"ip":"192.168.1.40","heap_min":141232,
 "groups":["settings","net","bath"],"settings_version":12,
 "mode":"ambient","volumes":{"ambient":14,"cold":10},"hue":"ok","sd":"ok","tracks":{"A":29,"B":12,"C":8,"V":4},
 "cold":{"phase":"cold","remaining_s":41,"profile":"hype","switch":"hype"}}
```

`node` is `bath`, `bedroom_speaker`, `bedside` or `crate`. Common members: `online` … `settings_version`. Per node:

| Node | Members |
| --- | --- |
| bath | `mode` (`normal`/`ambient`/`cold`), `volumes` (`{"ambient":14,"cold":10}`, steps 0–32), `hue`, `sd`, `tracks`, `cold` |
| bedroom_speaker | `playing` (`none`/`alarm`/`music`), `folder` (the folder playing, or ""), `kept` (`true` while the session is a KEEP one), `volume`, `sd`, `folders` (names of the root folders on the card, as sent in `LINK_FOLDER`), `armed` (`{"state":"waiting","version":41,"alarm_utc":1795030000}`), `link` (`up`/`down`) |
| crate | `state` (`closed`/`open`/`dim`/`closing`), `light` (0–1000), `playing`, `volume`, `battery_mv`, `battery` (`ok`/`low`/`empty`), `sd`, `tracks`. Before deep sleep: retained `{"online":false,"node":"crate","asleep":true,"battery_mv":12950,"battery":"ok"}` |
| bedside | `time` (local ISO), `time_source` (`sntp`/`rtc`/`none`), `armed_switch`, `next_alarm` (`{"slot":1,"local":"2026-11-16T06:45:00"}` or null), `alarm` (`idle`/`armed`/`sunrise`/`ringing`/`snoozed`), `light` (0–1000), `mode` (`none`/`meditation`/`mood`/`sleep`), `fading`, `candles`, `volumes` (`{"meditation":10,"mood":14,"sleep":8}`), `remaining_s` (of the running mode's timer, or null), `speaker` (`up`/`down`), `game` (`{"state":"idle","hits":0,"needed":3}`) |

The web app shows `mood_folder` and `sleep_folder` (`bed` settings) as dropdowns filled from the speaker's `folders` that start with `mood_` / `sleep_`, and `med_folder` as a dropdown of all folders. The mood button, and the sleep button while sleep plays, move to the next folder of the family on the device, which reports the new value like any local change.

### Settings

Desired (server → device):

```json
{"version":12,"values":{"transition_ms":"4000","hype_voice":"off","alarm1":"06:45,mtwtf--,on"}}
```

Reported (device → server):

```json
{"version":12,"values":{"wifi_ssid":"home","wifi_pass":"********","transition_ms":"4000"},"rejected":{"volume_max":"range"}}
```

Rules (tested in `test_settings_sync`):

- `version` is an integer 1 … 2147483647 that the server raises with every change. The device ignores a document whose version is not greater than the one it applied last (kept in NVS).
- Every value is a JSON string in the console's text form: uint as digits, bool `on`/`off`, choice by name, scene ids as UUID or `""`.
- Missing keys keep their value. Members are applied in document order (matters for dependent keys such as `clap_min_gap_ms` / `clap_max_gap_ms`).
- `rejected` reasons: `unknown`, `invalid`, `range`, `conflict`. The version counts as applied even if values were rejected.
- Secrets are reported as `********` (or `""` if unset). A desired value of `********` for a secret means "leave it".
- `reported.values` lists every key the node has, in a stable order. The web app builds its forms from this list; `status.groups` says which groups they belong to.
- After a local change (console, fallback page) the device publishes `reported` with the unchanged version. The server adopts the differing values into its desired set and publishes a new version.

Key lists with ranges and defaults: `lib/settings/settings.h` (v1 keys) and `lib/node_config/node_config.h` (groups `net`, `bath`, `bed`, `game`).

### Commands

Payload `{}` unless listed. A device ignores commands it does not know and reports `{"type":"cmd","name":"…","ok":false,"why":"unknown"}` as an event.

| `cmd/<name>` | Nodes | Payload | Effect |
| --- | --- | --- | --- |
| `mode` | bath | `{"mode":"ambient"}` or `"normal"` | like the main button, if the mode differs |
| `cold/start`, `cold/next`, `cold/abort` | bath | – | like the cold button |
| `scene/test` | bath, bedside | `{"scene":"<uuid>"}` | recall that scene now |
| `hue/scenes` | bath, bedside | – | fetch the scene list from the bridge and publish it |
| `hue/pair` | bath, bedside | – | try pairing with the bridge for 30 s: press its link button first; the new key is stored as `hue_key` and the scene list is published |
| `voice/test` | bath | `{"cue":"go"}` (`go`, `warning`, `to_cold`, `done`) | play that clip |
| `music/sync` | bath, speaker, crate | `{"base":"https://…/files/","files":[{"path":"B/track01.mp3","size":4812345,"sha256":"…"}]}` | pull missing or changed files, delete files not listed, then publish an event `music`. Rules in `lib/sync_manifest`: paths are `name` or `folder/name` (folder 1–23 of `A-Z a-z 0-9 _ -`; name up to 95 printable characters ending in `.mp3` / `.wav`); at most 256 files, payload up to 64 KB. Only `.mp3`, `.wav` and `.part` files in the root and its folders are ever deleted, so the bath's voice clips (`V/*.wav`) must be in the list. A sync while one runs: event `cmd` with `why` `busy` |
| `ota` | all | `{"version":"2.0.1","url":"https://…/fw/bath-2.0.1.bin","sha256":"…"}` | download, verify, reboot; rollback if the new image does not confirm |
| `log/verbose` | all | `{"minutes":10}` | send DEBUG lines for that long |
| `restart` | all | – | reboot |
| `alarm/stop`, `alarm/snooze` | bedside | – | like the big button (refused while the game is required) |
| `button` | bedside | `{"name":"sleep"}` (`meditation`, `mood`, `sleep`, `next`, `stop`) | like pressing that button (lib/bed_modes) |
| `game/demo`, `game/selftest`, `game/stop` | bedside | – | demo game, self-test |
| `game/calibrate`, `game/identify` | bedside | `{"node":"target-2"}` | forwarded to that target |
| `link/pair` | bedside | `{"seconds":60}` | accept HELLO from new nodes for that long |
| `identify` | all | `{"seconds":10}` | blink / beep so you can find the device |
| `forget` | all | – | the device was removed in the web app: erase the broker account from NVS and go back to pairing (`enroll_forget`) |
| `ack` | all | `{"type":"cold_session","id":3735928559}` | the backend has stored that record: the device drops it from its outbox (see Records) |

### Events

```json
{"type":"clap","accepted":true,"peak_db":[-18.2,-19.0],"gap_ms":312,"rise_ms":[0.4,0.6],"band_db":[14.1,12.7]}
{"type":"mode","mode":"ambient","by":"clap"}
{"type":"mode","mode":"sleep","by":"button","continued":true}
{"type":"cold","phase":"cold","profile":"hype"}
{"type":"cold","end":"finished"}
{"type":"alarm","event":"ring","slot":1,"game":true}
{"type":"alarm","event":"stop","reason":"game"}
{"type":"game","event":"hit","target":"target-2","hits":2,"needed":3}
{"type":"ota","state":"done","version":"2.0.1"}
{"type":"music","have":41,"missing":0,"free_mb":14200}
{"type":"error","where":"hue","text":"bridge unreachable"}
```

```json
{"type":"cold_session","id":3735928559,"start":1795000000,"profile":"hype","planned":{"warm":120,"cold":90,"after":300},"actual":{"warm":118,"cold":90,"after":12},"end":"aborted","skipped":true}
```

Every event may carry `"t"` (UTC seconds) when the device knows the time. Bedside `mode` events: `mode` is `meditation`, `mood`, `sleep` or `none`; `by` `button`, `server`, `timer` (a timer ran out) or `alarm`; `continued` only on sleep (true = a continuation of the night, no scene, no candles); `folder` on mood when the button moved to the next folder. `by` is `clap`, `button`, `server` or `timer`. Alarm stop reasons: `button`, `game`, `remote`, `timeout`, `disarmed`.

### Records

Some events are records the backend must store: today `cold_session` (one per cold-shower run that reached the cold phase), later others. They are ordinary events on `shower/<id>/event` with two extra rules (`lib/record_outbox`, `lib/cold_session`):

- **The device keeps each record until it is acknowledged,** also across a reboot (NVS), and sends it again on every reconnect and every 60 s while unacknowledged. It keeps at most 8; when full, the oldest is dropped and logged.
- **The backend stores, then acknowledges:** after the row is written it publishes `shower/<id>/cmd/ack` with `{"type":"cold_session","id":…}`. A record it already has is acknowledged again without a second row. So the same record may arrive more than once and must never be counted twice.
- `id` is a random non-zero 32-bit number (hardware RNG on the device), unique per device. The database key is (`device_id`, `id`).

`cold_session` members:

| Member | Meaning |
| --- | --- |
| `start` | UTC seconds when the button was pressed; `null` if the device had no time yet. The backend then uses its receive time minus the run's length and marks the row |
| `profile` | `hype` or `zen` |
| `planned.warm/cold/after` | the profile's phase lengths in seconds at the start |
| `actual.warm/cold/after` | seconds really spent in each phase (rounded); `actual.cold` is the cold exposure |
| `end` | `finished` (ran out) or `aborted` (held button, main button) |
| `skipped` | `next` was pressed at least once |

Suggested table (SQLite, backend repository):

```sql
CREATE TABLE cold_session (
  device_id     TEXT    NOT NULL,
  id            INTEGER NOT NULL,          -- from the device, unique per device
  start_utc     INTEGER NOT NULL,          -- seconds; estimated if the device sent null
  start_estimated INTEGER NOT NULL DEFAULT 0,
  received_utc  INTEGER NOT NULL,
  profile       TEXT    NOT NULL CHECK (profile IN ('hype','zen')),
  planned_warm_s INTEGER NOT NULL, planned_cold_s INTEGER NOT NULL, planned_after_s INTEGER NOT NULL,
  warm_s        INTEGER NOT NULL, cold_s  INTEGER NOT NULL, after_s INTEGER NOT NULL,
  ended         TEXT    NOT NULL CHECK (ended IN ('finished','aborted')),
  skipped       INTEGER NOT NULL,
  PRIMARY KEY (device_id, id)
);
-- store: INSERT OR IGNORE ...; then publish cmd/ack in either case
```

### Logs

```json
{"seq":4711,"lines":["I (8123) hue: scene ambient 204","W (8125) wifi: rssi -81"]}
```

`seq` is the sequence number of the first line (`lib/log_ring`); a gap between messages means lines were lost. Lines are ESP-IDF log lines without colour codes, at most 200 bytes each. Default level `info` (`log_level` setting). At boot the device sends `{"seq":1,"lines":["I (0) boot: reset poweron, fw 2.0.0"]}` first.

### Hue scenes

```json
{"t":1795000000,"scenes":[{"id":"3f2a9c1e-5b7d-4e8a-9c21-7d4e5f6a8b90","name":"Kerzenlicht","room":"Bad"}]}
```

Published after boot, after pairing and on `cmd/hue/scenes`. The web app assigns scenes by storing the `id` in a scene key (`scene_normal`, `hype_scene_cold`, …); positions and names are never stored. A stored id that is missing from the list is shown as "missing"; the device falls back as the key's rule says.

### Nodes without Wi-Fi

`shower/<bedside>/node/<name>`:

```json
{"type":"target","alive":true,"battery_mv":3987,"rssi":-61,"fw":"2.0.0","caps":["hit_sensor","light","battery"],"last_seen":1795000000}
```

## 2. Device → server: HTTPS

Downloads with the URLs from `cmd/ota` and `cmd/music/sync`, and pairing. Authentication for downloads: HTTP basic with the device's broker account. Music downloads use `Range` requests so an interrupted file continues where it stopped. The server never calls the device.

### Pairing

A device without a broker account (no `device_id` / `mqtt_user` / `mqtt_pass` in NVS) is *unclaimed*. It talks to `https://<server_host>/api/enroll` (`server_host` in the `net` group, written from the firmware's build default at the first boot; the setup page can change it). Messages are built and parsed by `lib/enroll_msg`; the device side is `lib/enroll`.

```
device                                    server                                   browser
  POST hello  {"chip","node","fw","secret"}   ─▶ remembers chip, node, fw, public IP, last seen;
  ◀─ {"state":"wait"}                            binds chip → secret on first sight
  … every 10 s …                                                                    GET /api/devices/nearby
                                                 lists unclaimed devices seen in the  ◀─ (same public IP as the
                                                 last 60 s from the browser's IP          browser, see below)
                                                                                    POST /api/devices/<chip>/claim
  POST hello  ─▶                                 ◀─ marks a 60 s confirm window
  ◀─ {"state":"confirm","window_s":60}           (blinks)                           "Hold the button for 3 s"
  … polls every 2 s, blinks …
  button held 3 s
  POST confirm {"chip","secret","confirm":true} ─▶ window open and secret matches:
                                                 creates broker account + ACL, device row
  ◀─ {"state":"claimed","device_id","mqtt_host","mqtt_port","mqtt_user","mqtt_pass"}
  saves into the net group, connects MQTT  ─▶   status arrives: device page opens  ──▶ "Bathroom added"
```

Rules:

- **Who sees what.** `nearby` lists unclaimed devices whose last hello came from the same public IP as the browser's request, seen in the last 60 s, plus devices already in the account. Behind CGNAT or with IPv6 privacy addresses the match can fail; the Add page then offers "enter the chip id" (printed in the device's boot log and on its setup page) as a fallback, still with the 3 s hold.
- **The hold is the proof.** Credentials go out only as the answer to a confirm that arrives inside the window with the secret the server bound to that chip. A forged hello for the same chip id from elsewhere has a different secret and is answered `{"state":"rejected","reason":"conflict"}`; the server shows the conflict in the web app. "Forget device" in the web app clears the binding.
- **Answers.** `wait` (nothing to do), `confirm` (`window_s` 10–300), `claimed` (only to a confirm), `rejected` (`reason`). HTTP status 200 for all of them; anything else counts as no answer and the device just tries again.
- **Rate.** Unclaimed: one hello every 10 s. Inside a window: every 2 s. The server answers at most 1 request/s per chip and per IP with 429.
- **Which button.** Bath: the main button (3 s; without a window it starts Hue pairing as before). Bedside: the stop button. Bedroom speaker: its button, unless the alarm rings (then the 3 s press stops the alarm, as always). Crate: the encoder's push button, lid open (the strip pulses). Gun and targets have no Wi-Fi and never pair with the server; they pair with the bedside over ESP-NOW (`cmd/link/pair`).
- **Devices that sleep (crate).** The server keeps commands that wait for an answer (`music/sync`, `ota`) per device and sends them when the device's status next says `online:true`; `asleep:true` in the retained status means "fine, sleeping", not "lost".
- **Losing the account.** Three "not authorised" answers from the broker in a row (account deleted on the server) or `cmd/forget`: the device erases the five net keys and is unclaimed again. Network errors never count.
- **New secret after a flash erase.** The secret lives in NVS; erasing the whole flash (`pio run -t erase`, `esptool erase_flash`) makes the device generate a new one, and its next hello is answered `rejected`/`conflict`. The web app shows "this device has a new secret" with **Allow re-pair**, which drops the old binding; then Add and the 3 s hold as usual. Normal firmware uploads keep NVS and change nothing.
- **No sticker needed.** The Add page finds devices by public IP, and lists chip id, node type and firmware for each. The chip id is also printed in the boot log and on the setup page, for the fallback. The secret is never printed or shown anywhere.
- **Blink.** While a window is open the device shows it: bath candles pulse, bedside LEDs pulse, speaker status LED blinks.

## 3. Browser ↔ device: local fallback page

`http://<device_id>.local/` on the home network, plain HTTP, PIN from `web_pin` (page disabled while the PIN is empty). `GET /api/status`, `GET /api/log?after=<seq>`, `GET /api/settings`, `POST /api/settings` (`{"key":"value"}`, answers like the reported document), `POST /api/firmware` (image upload), `POST /api/restart`. Without Wi-Fi for 2 minutes the device opens the access point `bas-setup-<last 4 of MAC>` with the same page at `192.168.4.1`.

## 4. Node ↔ node: ESP-NOW

Frames are built and parsed by `lib/link_msg`; the layouts are pinned in `test_link_msg`.

```
byte 0    0xBA            magic
byte 1    1               protocol version
byte 2    type
byte 3    flags           bit 0: please acknowledge
byte 4-5  seq             little-endian; 0 = not acknowledged
byte 6…   payload         fields in struct order, little-endian, no padding
```

| Type | # | From → to | Ack | Payload (bytes) |
| --- | --- | --- | --- | --- |
| HELLO | 1 | any → any | yes | node u8, caps u16, fw major/minor/patch u8×3, name char[12] (18) |
| ACK | 2 | receiver → sender | – | acked_seq u16 (2) |
| HEARTBEAT | 3 | every node, every 5 s | no | uptime_s u32, battery_mv u16, rssi i8, state u8 (8) |
| TIME_ALARM | 4 | bedside → speaker, gun, targets | yes | version u32, utc_now u32, alarm_utc u32, sound u8, volume u8, fade_s u16, ring_max_min u8, game u8 (18) |
| RING | 5 | bedside → speaker | yes | action u8 (1 start, 2 snooze, 3 stop, 4 escalate), version u32, arg u16 (7) |
| RING_STATE | 6 | speaker → bedside | yes | state u8, reason u8, spare u8, ring_s u16 (5) |
| AUDIO | 7 | bedside → speaker | yes | cmd u8 (1 stop, 2 play, 3 volume, 4 next), flags u8 (bit 0 resume, bit 1 keep), volume u8 (step 0–32), fade_ds u16 (tenths of a second), folder char[23] (28) |
| GAME_CFG | 8 | bedside → gun, targets | yes | sights u8, zone u8, ammo u8, gun_id u8 (4) |
| TARGET_CMD | 9 | bedside → target | yes | cmd u8 (1 light, 2 off, 3 identify, 4 calibrate), round u8, arg_ms u16 (4) |
| TARGET_EVT | 10 | target → bedside | yes | evt u8 (1 hit, 2 timeout, 3 calibrated), round u8, gun_id u8, value u16 (5) |
| GUN_EVT | 11 | gun → bedside | yes | evt u8 (1 shot, 2 dry, 3 reload, 4 demo request), shot u8, ammo_left u8 (3) |
| FOLDER | 12 | speaker → bedside | yes | index u8, count u8, name char[23] (25) |

Rules:

- A receiver drops frames with another magic or version and ignores bytes after the payload it knows, so fields can be appended later.
- Delivery (`lib/link_peer`): a frame that asks for an ack is sent again every 60 ms, at most 5 times, one frame at a time per peer. The receiver acks every copy and acts on the first. HELLO resets the receiver's duplicate filter (the sender restarted).
- Liveness: any frame counts. 15 s of silence = peer down.
- HEARTBEAT `state`: speaker = `armed_state_t` (0 idle, 1 waiting, 2 ringing, 3 snoozed); target = LED state; gun = 0 awake, 1 about to sleep.
- TIME_ALARM is sent on every change of time or schedule and with every heartbeat of the bedside. A copy with a lower version than the one held is discarded. `alarm_utc` 0 = no alarm.
- RING stop is obeyed whatever its version. RING start while ringing is ignored. RING start stops any music at once (a KEEP session saves its resume point first).
- AUDIO: the bedside owns all timers (sleep, meditation); the speaker only plays, fades and stops. PLAY replaces what plays. A PLAY with RESUME continues the last KEEP session of that folder at its exact track and byte position (`lib/resume_point`; one point per folder in NVS under `resume_key(folder)`), or starts a fresh shuffle if there is none. A RESUME PLAY of the folder already playing, also during its fade-out, cancels the fade and plays on. VOLUME applies under any fade. Unused fields are 0; text fields are NUL-padded, without a NUL when full.
- FOLDER: the speaker sends its folder list after every card scan and after a HELLO from the bedside, one acked frame per folder (`index` 0 … `count`-1); `count` 0 = no folders. Only names that pass `cfg_folder_valid()` are listed (1–23 of `A-Z a-z 0-9 _ -`).
- Capabilities (`caps`): 0x01 audio, 0x02 light, 0x04 laser, 0x08 hit sensor, 0x10 moving, 0x20 magazine, 0x40 battery.
- Pairing: the bedside stores the MAC addresses of its speaker, gun and targets; frames from other addresses are dropped. ESP-NOW encryption with a key set at pairing.
- Channel: bedside and speaker use the router's channel. Gun and targets send HELLO on channels 1–13 in turn until the bedside acks, and remember the channel.

## 5. Laser burst

`lib/pulse_code`, T = 500 µs: start mark 3 T on, 1 T off; then 12 bits, most significant first, each 2 T on (1) or 1 T on (0) followed by 1 T off. Bits: gun id (4), shot (4), check (4) with check = (id XOR shot XOR 0xA) AND 0xF. A burst lasts at most 21 ms. Decoder windows: 0 = 250–750 µs, 1 = 751–1249 µs, start = 1250–1900 µs, gap = 250–900 µs.
