#!/usr/bin/env python3
"""Record labelled audio from the recorder firmware (tools/recorder).

  python tools/capture.py ports
  python tools/capture.py monitor --port COM5
  python tools/capture.py record --port COM5 --label double_clap --prompts 10 --distance 2
  python tools/capture.py record --port COM5 --label door --seconds 120 --note "bathroom door, normal force"

Prompted sessions ("--prompts N") tell you when to clap; the prompt times go
into the .json sidecar so tools/clap_lab.py can score detections. Sessions
without prompts are negatives: every detection in them is a false trigger.

Privacy: recordings are stored only on your PC, in the folder you choose.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import audiolib as al  # noqa: E402

BAUD = 921600
NO_DATA_TIMEOUT_S = 3.0


def open_port(port: str):
    try:
        import serial  # pyserial
    except ImportError:
        sys.exit("pyserial is missing: pip install -r tools/requirements.txt")
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = BAUD
    ser.timeout = 0.05
    ser.dtr = False  # keep the board's auto-reset circuit idle
    ser.rts = False
    try:
        ser.open()
    except Exception as e:  # noqa: BLE001
        sys.exit(f"Cannot open {port}: {e}\nClose any serial monitor using the port, or check the port name "
                 f"with 'python tools/capture.py ports'.")
    ser.reset_input_buffer()
    return ser


def cmd_ports(_args) -> None:
    try:
        from serial.tools import list_ports
    except ImportError:
        sys.exit("pyserial is missing: pip install -r tools/requirements.txt")
    ports = list(list_ports.comports())
    if not ports:
        print("No serial ports found. Is the board plugged in (DevKitC-1: the port marked UART)?")
    for p in ports:
        print(f"{p.device:20s} {p.description}")


class Stream:
    """Reads frames from the port and keeps protocol statistics."""

    def __init__(self, ser) -> None:
        self.ser = ser
        self.dec = al.FrameDecoder()
        self.seq = al.SequenceTracker()
        self.last_data = time.monotonic()
        self.constant_since: float | None = None
        self.last_value: int | None = None

    def read(self) -> list[tuple[int, bytes]]:
        """Returns (zero-fill sample count, payload) pairs."""
        data = self.ser.read(8192)
        out = []
        for f in self.dec.feed(data):
            self.last_data = time.monotonic()
            out.append((self.seq.check(f), f.payload))
        if time.monotonic() - self.last_data > NO_DATA_TIMEOUT_S:
            raise RuntimeError(
                f"No audio frames for {NO_DATA_TIMEOUT_S:.0f} s. Is the recorder firmware flashed "
                f"(pio run -d tools/recorder -t upload), the right port chosen, and no other program "
                f"using it? Bytes seen but not decodable: {self.dec.bytes_skipped}.")
        return out

    def check_constant(self, samples: np.ndarray) -> str | None:
        """Warns when the mic delivers a constant value for 2 s (wiring or slot problem)."""
        if samples.size == 0:
            return None
        if np.all(samples == samples[0]) and (self.last_value is None or samples[0] == self.last_value):
            self.last_value = int(samples[0])
            if self.constant_since is None:
                self.constant_since = time.monotonic()
            elif time.monotonic() - self.constant_since > 2.0:
                self.constant_since = time.monotonic()
                return (f"The microphone delivers a constant value ({self.last_value}). Check VDD/GND/SCK/WS/SD, "
                        f"that L/R is tied to GND, or rebuild the recorder with -DMIC_SLOT_RIGHT.")
        else:
            self.constant_since = None
            self.last_value = None
        return None


def bar(dbfs: float, width: int = 40) -> str:
    filled = int(max(0.0, min(1.0, (dbfs + 90.0) / 90.0)) * width)
    return "#" * filled + "." * (width - filled)


def cmd_monitor(args) -> None:
    ser = open_port(args.port)
    st = Stream(ser)
    print("Live level (100 ms blocks). Clap to see the peaks. Ctrl+C to stop.\n")
    pending = np.zeros(0, dtype=np.int32)
    block = al.SAMPLE_RATE // 10
    try:
        while True:
            for _gap, payload in st.read():
                pending = np.concatenate([pending, al.payload_to_int24(payload)])
            while pending.size >= block:
                chunk, pending = pending[:block], pending[block:]
                rms, peak = al.level_dbfs(chunk)
                warn = st.check_constant(chunk)
                if warn:
                    print("\n" + warn)
                lost = st.seq.frames_lost
                print(f"\rrms {rms:6.1f} dBFS  peak {peak:6.1f} dBFS  |{bar(peak)}|  lost frames {lost}   ",
                      end="", flush=True)
    except KeyboardInterrupt:
        print()
    except RuntimeError as e:
        print("\n" + str(e))
    finally:
        ser.close()


def cmd_record(args) -> None:
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    safe_label = "".join(c if c.isalnum() or c in "-_" else "_" for c in args.label)
    wav_path = out_dir / f"{stamp}_{safe_label}.wav"
    rate = al.SAMPLE_RATE

    if args.prompts > 0:
        prompt_at = [args.lead_in + k * args.interval for k in range(args.prompts)]
        total_s = prompt_at[-1] + max(args.interval, args.window + 0.5)
        print(f"Prompted session: {args.prompts} double claps, one every {args.interval:g} s. "
              f"Clap twice within {args.window:g} s of each prompt.")
    else:
        prompt_at = []
        total_s = args.seconds
        print(f"Negative session '{args.label}': {total_s:g} s. Make the sound naturally; "
              f"do not clap. Ctrl+C ends early and keeps what was recorded.")
    print(f"Writing {wav_path}\n")

    ser = open_port(args.port)
    st = Stream(ser)
    writer = al.open_wav_writer(wav_path, rate)
    written = 0
    prompts_s: list[float] = []
    next_prompt = 0
    warned_ready = False
    last_status = -1
    end_sample = int(total_s * rate)
    interrupted = False
    try:
        while written < end_sample:
            for gap, payload in st.read():
                if gap:
                    writer.writeframes(b"\x00" * (3 * gap))
                    written += gap
                writer.writeframes(payload)
                samples = al.payload_to_int24(payload)
                written += samples.size
                warn = st.check_constant(samples)
                if warn:
                    print("\n" + warn)
            now_s = written / rate
            if next_prompt < len(prompt_at):
                if not warned_ready and now_s >= prompt_at[next_prompt] - 1.0:
                    print(f"  get ready ({next_prompt + 1}/{len(prompt_at)}) ...", flush=True)
                    warned_ready = True
                if now_s >= prompt_at[next_prompt]:
                    prompts_s.append(round(now_s, 3))
                    print(f"\a  >>> CLAP CLAP now ({next_prompt + 1}/{len(prompt_at)}) <<<", flush=True)
                    next_prompt += 1
                    warned_ready = False
            elif not prompt_at and int(now_s) != last_status:
                last_status = int(now_s)
                print(f"\r  {last_status:4d} / {total_s:g} s", end="", flush=True)
    except KeyboardInterrupt:
        interrupted = True
    except RuntimeError as e:
        print("\n" + str(e))
        interrupted = True
    finally:
        writer.close()
        ser.close()

    seconds = written / rate
    kept = [p for p in prompts_s if p + args.window <= seconds]
    meta = {
        "format": 1,
        "label": args.label,
        "sample_rate": rate,
        "prompts_s": kept,
        "window_s": args.window,
        "distance_m": args.distance,
        "note": args.note,
        "started": stamp,
        "seconds": round(seconds, 3),
        "interrupted": interrupted,
        "frames_lost": st.seq.frames_lost,
        "overflow_frames": st.seq.overflow_frames,
        "crc_errors": st.dec.crc_errors,
        "restarts": st.seq.restarts,
    }
    al.sidecar_path(wav_path).write_text(json.dumps(meta, indent=2), encoding="utf-8")
    print(f"\n\nSaved {seconds:.1f} s to {wav_path.name} (+ .json).")
    if st.seq.frames_lost or st.seq.overflow_frames or st.dec.crc_errors:
        print(f"Warning: {st.seq.frames_lost} frames lost, {st.seq.overflow_frames} with I2S overflow, "
              f"{st.dec.crc_errors} CRC errors. Gaps were filled with silence. Use a shorter or better USB "
              f"cable and close other serial programs.")
    if st.seq.restarts:
        print("Warning: the board restarted during the recording.")
    if len(kept) < len(prompts_s):
        print(f"{len(prompts_s) - len(kept)} prompt(s) at the end were dropped: the recording ended inside "
              f"their window.")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("ports", help="list serial ports")

    m = sub.add_parser("monitor", help="live level meter to check the wiring")
    m.add_argument("--port", required=True)

    r = sub.add_parser("record", help="record a labelled session")
    r.add_argument("--port", required=True)
    r.add_argument("--label", required=True, help="e.g. double_clap, double_clap_shower, door, toilet_lid, "
                                                  "footsteps, flush, hair_dryer, talking, music")
    r.add_argument("--prompts", type=int, default=0, help="number of prompted double claps (0 = negative session)")
    r.add_argument("--interval", type=float, default=6.0, help="seconds between prompts")
    r.add_argument("--lead-in", type=float, default=3.0, help="seconds before the first prompt")
    r.add_argument("--window", type=float, default=3.0, help="seconds after a prompt in which claps count")
    r.add_argument("--seconds", type=float, default=60.0, help="length of a negative session")
    r.add_argument("--distance", type=float, default=None, help="distance to the mic in metres (metadata)")
    r.add_argument("--note", default="", help="free text stored in the sidecar")
    r.add_argument("--out", default="recordings", help="output folder (default: recordings)")

    args = ap.parse_args()
    {"ports": cmd_ports, "monitor": cmd_monitor, "record": cmd_record}[args.cmd](args)


if __name__ == "__main__":
    main()
