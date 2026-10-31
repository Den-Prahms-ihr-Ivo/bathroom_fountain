#!/usr/bin/env python3
"""Per-prompt report: what the claps in each prompt window look like.

  python tools/prompt_report.py recordings/20261031-140304_double_clap.wav
  python tools/prompt_report.py recordings/            # every prompted recording

For every prompt window it finds the two strongest transients (peak level in
2 ms blocks, at least 60 ms apart) and prints the numbers the detector's rules
look at, with a flag where a DEFAULT setting would reject the pair:

  peak   level of each clap (dBFS, sample peak)        CLAP-13 clap_min_peak_dbfs
  diff   level difference between the two claps (dB)   clap_level_diff_db
  gap    time between the claps (ms)                   clap_min_gap_ms / clap_max_gap_ms
  drop   how far the level falls between them (dB)     clap_drop_db
  rise   first clap above the background before it     clap_onset_db

This is an approximation of the detector (it does not run your C code), meant
to show which rule a missed double clap ran into. Confirm a change with
clap_lab.py replay / tune.
"""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import audiolib as al  # noqa: E402

BLOCK_MS = 2
MIN_SEP_MS = 60
FULL_SCALE = float(1 << 23)

LIMITS = {
    "peak": -40.0,
    "diff": 6.0,
    "gap_min": 150.0,
    "gap_max": 600.0,
    "drop": 12.0,
    "rise": 12.0,
}


def db(x: np.ndarray | float) -> np.ndarray | float:
    return 20.0 * np.log10(np.maximum(x, 1e-9))


def block_peaks(samples: np.ndarray, rate: int) -> tuple[np.ndarray, int]:
    n = max(1, rate * BLOCK_MS // 1000)
    x = samples.astype(np.float64)
    x -= np.convolve(
        x, np.ones(rate // 50) / (rate // 50), mode="same"
    )  # crude DC / rumble removal
    usable = len(x) // n * n
    blocks = np.abs(x[:usable]).reshape(-1, n).max(axis=1) / FULL_SCALE
    return db(blocks), n


def strongest(env: np.ndarray, lo: int, hi: int, sep: int, count: int) -> list[int]:
    seg = env[lo:hi].copy()
    picks: list[int] = []
    for _ in range(count):
        if seg.size == 0 or not np.isfinite(seg).any():
            break
        i = int(np.argmax(seg))
        if seg[i] <= -150:
            break
        picks.append(lo + i)
        seg[max(0, i - sep) : i + sep + 1] = -200
    return sorted(picks)


def report(rec: al.Recording) -> None:
    env, n = block_peaks(rec.samples24, rec.rate)
    per_s = rec.rate / n
    sep = int(MIN_SEP_MS / BLOCK_MS)
    print(f"\n{rec.wav.name}  ({rec.label}, {len(rec.prompts_s)} prompts)")
    print(
        f"  {'prompt':>6s}  {'t1':>6s}  {'peak1':>6s}  {'peak2':>6s}  {'diff':>5s}  {'gap':>5s}  "
        f"{'drop':>5s}  {'rise':>5s}  would fail"
    )
    for k, p in enumerate(rec.prompts_s):
        lo, hi = int(p * per_s), int((p + rec.window_s) * per_s)
        picks = strongest(env, lo, min(hi, len(env)), sep, 2)
        if len(picks) < 2:
            print(f"  {k + 1:6d}  fewer than two transients found")
            continue
        a, b = picks
        p1, p2 = float(env[a]), float(env[b])
        gap = (b - a) * BLOCK_MS
        between = float(np.min(env[a + 1 : b])) if b > a + 1 else min(p1, p2)
        drop = min(p1, p2) - between
        bg_lo = max(0, a - int(0.5 * per_s))
        bg = float(np.median(env[bg_lo : max(bg_lo + 1, a - int(0.05 * per_s))]))
        rise = p1 - bg
        fails = []
        if max(p1, p2) < LIMITS["peak"]:
            fails.append("peak")
        if abs(p1 - p2) > LIMITS["diff"]:
            fails.append("diff")
        if gap < LIMITS["gap_min"]:
            fails.append("gap<min")
        if gap > LIMITS["gap_max"]:
            fails.append("gap>max")
        if drop < LIMITS["drop"]:
            fails.append("drop")
        if rise < LIMITS["rise"]:
            fails.append("rise")
        print(
            f"  {k + 1:6d}  {a / per_s:6.2f}  {p1:6.1f}  {p2:6.1f}  {abs(p1 - p2):5.1f}  {gap:5.0f}  "
            f"{drop:5.1f}  {rise:5.1f}  {', '.join(fails) if fails else '-'}"
        )


def main() -> None:
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    recs = [al.load_recording(w) for w in al.find_wavs(sys.argv[1:])]
    recs = [r for r in recs if r.prompted]
    if not recs:
        sys.exit("No prompted recordings found.")
    for r in recs:
        report(r)
    print(
        "\nLimits used (defaults): "
        + ", ".join(f"{k} {v:g}" for k, v in LIMITS.items())
    )


if __name__ == "__main__":
    main()
