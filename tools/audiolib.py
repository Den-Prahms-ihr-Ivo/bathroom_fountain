"""Shared helpers for capture.py and clap_lab.py.

Frame protocol (see tools/recorder/src/frame.h), WAV + sidecar files,
scoring of detections against prompted claps, and synthetic test scenes.
"""
from __future__ import annotations

import binascii
import json
import struct
import wave
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

SAMPLE_RATE = 16000
MAGIC = b"AUD1"
HEADER = 12
MAX_SAMPLES = 1024
FLAG_I2S_OVERFLOW = 0x0001
FULL_SCALE_24 = float(1 << 23)


# ---------------------------------------------------------------------------
# Frame protocol
# ---------------------------------------------------------------------------
def crc16(data: bytes) -> int:
    """CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF), same as frame_crc16()."""
    return binascii.crc_hqx(data, 0xFFFF)


@dataclass
class Frame:
    seq: int
    flags: int
    payload: bytes  # n * 3 bytes, signed 24-bit little-endian

    @property
    def n(self) -> int:
        return len(self.payload) // 3


class FrameDecoder:
    """Turns a raw serial byte stream into frames; resyncs on garbage."""

    def __init__(self) -> None:
        self.buf = bytearray()
        self.crc_errors = 0
        self.bytes_skipped = 0

    def feed(self, data: bytes) -> list[Frame]:
        self.buf += data
        out: list[Frame] = []
        buf = self.buf
        while True:
            i = buf.find(MAGIC)
            if i < 0:
                keep = min(len(buf), len(MAGIC) - 1)
                self.bytes_skipped += len(buf) - keep
                del buf[: len(buf) - keep]
                break
            if i > 0:
                self.bytes_skipped += i
                del buf[:i]
            if len(buf) < HEADER:
                break
            seq, n, flags = struct.unpack_from("<IHH", buf, 4)
            if n == 0 or n > MAX_SAMPLES:
                del buf[:1]
                self.bytes_skipped += 1
                continue
            total = HEADER + 3 * n + 2
            if len(buf) < total:
                break
            (crc_rx,) = struct.unpack_from("<H", buf, HEADER + 3 * n)
            if crc16(bytes(buf[4 : HEADER + 3 * n])) != crc_rx:
                self.crc_errors += 1
                del buf[:1]
                self.bytes_skipped += 1
                continue
            out.append(Frame(seq, flags, bytes(buf[HEADER : HEADER + 3 * n])))
            del buf[:total]
        return out


class SequenceTracker:
    """Detects lost frames and board restarts from frame sequence numbers."""

    MAX_GAP = 1000  # frames; anything larger is treated as a restart

    def __init__(self) -> None:
        self.expected: int | None = None
        self.frames_lost = 0
        self.restarts = 0
        self.overflow_frames = 0

    def check(self, f: Frame) -> int:
        """Returns the number of samples lost before this frame (to be zero-filled)."""
        if f.flags & FLAG_I2S_OVERFLOW:
            self.overflow_frames += 1
        lost = 0
        if self.expected is not None and f.seq != self.expected:
            gap = (f.seq - self.expected) & 0xFFFFFFFF
            if gap <= self.MAX_GAP:
                self.frames_lost += gap
                lost = gap * f.n
            else:
                self.restarts += 1
        self.expected = (f.seq + 1) & 0xFFFFFFFF
        return lost


def payload_to_int24(payload: bytes) -> np.ndarray:
    b = np.frombuffer(payload, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
    v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
    return np.where(v & 0x800000, v - (1 << 24), v).astype(np.int32)


def int24_to_payload(samples: np.ndarray) -> bytes:
    v = np.asarray(samples, dtype=np.int64) & 0xFFFFFF
    b = np.stack([v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF], axis=1).astype(np.uint8)
    return b.tobytes()


def level_dbfs(samples24: np.ndarray) -> tuple[float, float]:
    """(RMS, peak) in dBFS of a block of 24-bit samples, DC removed."""
    if samples24.size == 0:
        return -150.0, -150.0
    x = samples24.astype(np.float64)
    x -= x.mean()
    rms = np.sqrt(np.mean(x * x)) / FULL_SCALE_24
    peak = np.max(np.abs(x)) / FULL_SCALE_24
    return 20 * np.log10(max(rms, 1e-8)), 20 * np.log10(max(peak, 1e-8))


# ---------------------------------------------------------------------------
# Recordings: WAV + JSON sidecar
# ---------------------------------------------------------------------------
@dataclass
class Recording:
    wav: Path
    samples24: np.ndarray
    rate: int
    label: str = "unlabelled"
    prompts_s: list[float] = field(default_factory=list)
    window_s: float = 3.0
    meta: dict = field(default_factory=dict)

    @property
    def seconds(self) -> float:
        return len(self.samples24) / self.rate

    @property
    def prompted(self) -> bool:
        return len(self.prompts_s) > 0


def sidecar_path(wav: Path) -> Path:
    return wav.with_suffix(".json")


def read_wav24(path: Path) -> tuple[np.ndarray, int]:
    with wave.open(str(path), "rb") as w:
        rate, width, channels, n = w.getframerate(), w.getsampwidth(), w.getnchannels(), w.getnframes()
        raw = w.readframes(n)
    if width == 3:
        data = payload_to_int24(raw)
    elif width == 2:
        data = np.frombuffer(raw, dtype="<i2").astype(np.int32) << 8
    elif width == 4:
        data = np.frombuffer(raw, dtype="<i4").astype(np.int32) >> 8
    else:
        raise ValueError(f"{path}: unsupported sample width {width * 8} bit")
    if channels > 1:
        data = data.reshape(-1, channels)[:, 0]
    return data.astype(np.int32), rate


def load_recording(wav: Path) -> Recording:
    samples, rate = read_wav24(wav)
    rec = Recording(wav=wav, samples24=samples, rate=rate)
    sc = sidecar_path(wav)
    if sc.exists():
        meta = json.loads(sc.read_text(encoding="utf-8"))
        rec.meta = meta
        rec.label = meta.get("label", rec.label)
        rec.prompts_s = [float(p) for p in meta.get("prompts_s", [])]
        rec.window_s = float(meta.get("window_s", rec.window_s))
    return rec


def open_wav_writer(path: Path, rate: int = SAMPLE_RATE) -> wave.Wave_write:
    w = wave.open(str(path), "wb")
    w.setnchannels(1)
    w.setsampwidth(3)
    w.setframerate(rate)
    return w


def write_recording(path: Path, samples24: np.ndarray, meta: dict, rate: int = SAMPLE_RATE) -> None:
    with open_wav_writer(path, rate) as w:
        w.writeframes(int24_to_payload(samples24))
    sidecar_path(path).write_text(json.dumps(meta, indent=2), encoding="utf-8")


def find_wavs(paths: list[str]) -> list[Path]:
    out: list[Path] = []
    for p in paths:
        pp = Path(p)
        if pp.is_dir():
            out += sorted(pp.glob("*.wav"))
        elif pp.suffix.lower() == ".wav":
            out.append(pp)
    return out


# ---------------------------------------------------------------------------
# Scoring
# ---------------------------------------------------------------------------
@dataclass
class Score:
    prompts: int = 0
    hits: int = 0
    false: int = 0
    seconds: float = 0.0
    false_times_s: list[float] = field(default_factory=list)
    hit_times_s: list[float] = field(default_factory=list)

    @property
    def misses(self) -> int:
        return self.prompts - self.hits

    @property
    def rate(self) -> float | None:
        return self.hits / self.prompts if self.prompts else None

    def add(self, other: "Score") -> None:
        self.prompts += other.prompts
        self.hits += other.hits
        self.false += other.false
        self.seconds += other.seconds


def score_events(rec: Recording, first_onsets: list[int]) -> Score:
    """A prompt counts as hit if a detection's first clap lies in
    [prompt, prompt + window]. Every other detection is a false trigger."""
    s = Score(prompts=len(rec.prompts_s), seconds=rec.seconds)
    used = [False] * len(rec.prompts_s)
    for first in first_onsets:
        t = first / rec.rate
        hit = False
        for k, p in enumerate(rec.prompts_s):
            if not used[k] and p <= t <= p + rec.window_s:
                used[k] = True
                hit = True
                break
        if hit:
            s.hits += 1
            s.hit_times_s.append(t)
        else:
            s.false += 1
            s.false_times_s.append(t)
    return s


# ---------------------------------------------------------------------------
# Synthetic scenes (same signal model as the unit tests)
# ---------------------------------------------------------------------------
class SceneBuilder:
    def __init__(self, seconds: float, seed: int, rate: int = SAMPLE_RATE) -> None:
        self.rate = rate
        self.x = np.zeros(int(seconds * rate))
        self.rng = np.random.default_rng(seed)

    def _amp(self, dbfs: float) -> float:
        return FULL_SCALE_24 * 10 ** (dbfs / 20)

    def noise(self, dbfs: float, start: float = 0.0, end: float | None = None, lowpass: int = 1) -> None:
        a, b = int(start * self.rate), int((end if end is not None else len(self.x) / self.rate) * self.rate)
        n = self.rng.standard_normal(b - a + lowpass)
        if lowpass > 1:
            n = np.convolve(n, np.ones(lowpass) / np.sqrt(lowpass), mode="valid")[: b - a]
        n = n[: b - a]
        self.x[a:b] += self._amp(dbfs) * n / max(np.std(n), 1e-12)

    def impact(self, t: float, peak_dbfs: float, tau_ms: float = 15.0, length_ms: float = 200.0,
               lowpass: int = 1) -> None:
        a = int(t * self.rate)
        n = int(length_ms * self.rate / 1000)
        if a >= len(self.x):
            return
        n = min(n, len(self.x) - a)
        attack = int(self.rate / 1000)
        k = np.arange(n)
        env = np.where(k < attack, (k + 1) / attack, np.exp(-(k - attack) / (tau_ms * self.rate / 1000)))
        src = self.rng.uniform(-1, 1, n + lowpass)
        if lowpass > 1:
            src = np.convolve(src, np.ones(lowpass) / lowpass, mode="valid")[:n]
            src /= max(np.max(np.abs(src)), 1e-12)
        self.x[a : a + n] += self._amp(peak_dbfs) * env * src[:n]

    def tail(self, t: float, start_dbfs: float, tau_ms: float, length_ms: float) -> None:
        a = int(t * self.rate)
        n = min(int(length_ms * self.rate / 1000), len(self.x) - a)
        if n <= 0:
            return
        k = np.arange(n)
        self.x[a : a + n] += self._amp(start_dbfs) * np.exp(-k / (tau_ms * self.rate / 1000)) * self.rng.uniform(-1, 1, n)

    def samples24(self) -> np.ndarray:
        return np.clip(np.round(self.x), -(1 << 23), (1 << 23) - 1).astype(np.int32)


def synth_set(out_dir: Path, seed: int = 1) -> list[Path]:
    """Writes a small labelled set of synthetic recordings. Returns the paths."""
    out_dir.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(seed)
    written: list[Path] = []

    def save(name: str, sb: SceneBuilder, label: str, prompts: list[float], note: str) -> None:
        path = out_dir / f"{name}.wav"
        meta = {"format": 1, "label": label, "sample_rate": sb.rate, "prompts_s": prompts, "window_s": 3.0,
                "synthetic": True, "note": note}
        write_recording(path, sb.samples24(), meta, sb.rate)
        written.append(path)

    for name, bg, clap_level in (("synth_claps_quiet", -70.0, -10.0), ("synth_claps_shower", -40.0, -14.0)):
        sb = SceneBuilder(64.0, int(rng.integers(1 << 30)))
        sb.noise(bg)
        prompts = [3.0 + 6.0 * k for k in range(10)]
        for p in prompts:
            t1 = p + float(rng.uniform(0.4, 1.2))
            gap = float(rng.uniform(0.20, 0.45))
            l1 = clap_level + float(rng.uniform(-2, 2))
            l2 = l1 + float(rng.uniform(-2, 2))
            for t, lvl in ((t1, l1), (t1 + gap, l2)):
                sb.impact(t, lvl)
                sb.tail(t, lvl - 16, 150, 1200)
        save(name, sb, "double_clap" if bg < -60 else "double_clap_shower", prompts,
             f"10 double claps, background {bg:.0f} dBFS")

    sb = SceneBuilder(40.0, int(rng.integers(1 << 30)))
    sb.noise(-70)
    t = 2.0
    while t < 38.0:
        sb.impact(t, -14 + float(rng.uniform(-2, 2)), tau_ms=25, lowpass=12)
        t += 0.5 + float(rng.uniform(-0.03, 0.03))
    save("synth_footsteps", sb, "footsteps", [], "walking, one step every 0.5 s")

    sb = SceneBuilder(40.0, int(rng.integers(1 << 30)))
    sb.noise(-70)
    for k in range(8):
        t = 2.0 + 4.5 * k
        sb.impact(t, -8, lowpass=6)
        sb.impact(t + 0.2, -19, lowpass=6)
    save("synth_lid_bounce", sb, "toilet_lid", [], "lid drops and bounces, second impact 11 dB weaker")

    sb = SceneBuilder(48.0, int(rng.integers(1 << 30)))
    sb.noise(-70)
    for k in range(6):
        t = 2.0 + 7.5 * k
        sb.impact(t, -6, tau_ms=40, lowpass=4)
        sb.noise(-26, t + 0.03, t + 0.6)
        sb.impact(t + 0.25, -17, tau_ms=10)
    save("synth_door", sb, "door", [], "door slam, rattle, latch click 11 dB weaker")

    sb = SceneBuilder(40.0, int(rng.integers(1 << 30)))
    sb.noise(-70)
    t = 2.0
    while t < 38.0:
        sb.impact(t, -12 + float(rng.uniform(-6, 6)))
        t += float(rng.uniform(1.8, 4.0))
    save("synth_knocks", sb, "single_knocks", [], "single knocks, at least 1.8 s apart")
    return written
