#!/usr/bin/env python3
"""Replay recordings through YOUR clap detector and tune its settings.

  python tools/clap_lab.py synth  recordings/synth          # synthetic test set
  python tools/clap_lab.py replay recordings/               # score with defaults
  python tools/clap_lab.py replay recordings/ --set clap_onset_db=10 --set mic_hp_hz=150
  python tools/clap_lab.py tune   recordings/ --grid clap_onset_db=9,12,15 --grid mic_hp_hz=20,150,300
  python tools/clap_lab.py plot   recordings/20261102-153000_double_clap.wav

replay and tune compile tools/replay/clap_replay.c together with
lib/mic_frontend and lib/clap_detector (your implementations) using gcc, so
the numbers are exactly what the firmware will do. Setting names are the
same as in lib/settings/settings.h; tune prints ready-to-paste "set" lines.

Scoring: in a prompted recording a prompt counts as detected when a double
clap's first clap lies within [prompt, prompt + window]. Every other
detection, in any recording, is a false trigger.
Targets: CLAP-07 >= 90 % quiet, CLAP-09 >= 70 % with shower, CLAP-08 about
0 false triggers (one per week of real use).
"""
from __future__ import annotations

import argparse
import itertools
import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parent
sys.path.insert(0, str(TOOLS))
import audiolib as al  # noqa: E402

EXE = TOOLS / "build" / ("clap_replay.exe" if os.name == "nt" else "clap_replay")
LIBS = ("clap_detector", "mic_frontend")
# The device's factory defaults: keep in step with settings_defaults() in lib/settings/settings.c.
# Every run starts from these (not from clap_config_default()), so the lab measures what the device does.
DEFAULTS = {"clap_min_gap_ms": 150, "clap_max_gap_ms": 600, "clap_level_diff_db": 18.0,
            "clap_quiet_before_ms": 300, "clap_quiet_after_ms": 400, "clap_gap_factor": 1.5, "clap_drop_db": 12.0,
            "clap_onset_db": 20.0, "clap_min_peak_dbfs": -30.0, "mic_hp_hz": 20.0, "mic_shift": 16}
SETTING_KEYS = tuple(DEFAULTS)
# clap_config_default(): the values the unit tests and the synthetic scenes are built on. The synthetic
# shower scene puts claps only about 18 dB above the noise, so it needs these rather than the tuned DEFAULTS.
SYNTH_SETTINGS = {"clap_level_diff_db": "6.0", "clap_onset_db": "12.0", "clap_min_peak_dbfs": "-40.0"}


def changes_from_defaults(settings: dict[str, str]) -> int:
    return sum(1 for k, v in settings.items() if float(v) != float(DEFAULTS[k]))


# ---------------------------------------------------------------------------
# Harness
# ---------------------------------------------------------------------------
def build_harness(force: bool = False) -> Path:
    srcs = [TOOLS / "replay" / "clap_replay.c"]
    deps = list(srcs)
    incs = []
    for lib in LIBS:
        d = ROOT / "lib" / lib
        srcs += sorted(d.glob("*.c"))
        deps += sorted(d.glob("*.[ch]"))
        incs.append(f"-I{d}")
    if not force and EXE.exists() and all(EXE.stat().st_mtime >= p.stat().st_mtime for p in deps):
        return EXE
    cc = os.environ.get("CC") or shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")
    if not cc:
        sys.exit("No C compiler found (gcc). See stage 1.3 of the build guide.")
    EXE.parent.mkdir(exist_ok=True)
    cmd = [cc, "-O2", "-std=c11", *incs, *map(str, srcs), "-lm", "-o", str(EXE)]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        sys.exit("Building the replay harness failed:\n" + " ".join(cmd) + "\n" + res.stderr)
    return EXE


def to_words(rec: al.Recording) -> bytes:
    return (rec.samples24.astype("<i4") << 8).tobytes()


def run_harness(words: bytes, rate: int, settings: dict[str, str]) -> list[tuple[int, int, int]]:
    args = [str(EXE), "--rate", str(rate)]
    merged = {k: str(v) for k, v in DEFAULTS.items()}
    merged.update(settings)
    for k, v in merged.items():
        args += ["--set", f"{k}={v}"]
    res = subprocess.run(args, input=words, capture_output=True)
    if res.returncode == 3:
        sys.exit("The harness could not create the frontend or detector. Either a setting is invalid, or "
                 "lib/mic_frontend / lib/clap_detector is not implemented yet (run 'pio test -e native').")
    if res.returncode != 0:
        sys.exit(f"Harness failed ({res.returncode}): {res.stderr.decode(errors='replace')}")
    events = []
    for line in res.stdout.decode().splitlines():
        parts = line.split()
        if parts and parts[0] == "E":
            events.append((int(parts[1]), int(parts[2]), int(parts[3])))
    return events


def parse_sets(items: list[str] | None) -> dict[str, str]:
    out: dict[str, str] = {}
    for item in items or []:
        if "=" not in item:
            sys.exit(f"--set expects key=value, got '{item}'")
        k, v = item.split("=", 1)
        if k not in SETTING_KEYS:
            sys.exit(f"Unknown setting '{k}'. Known: {', '.join(SETTING_KEYS)}")
        out[k] = v
    return out


def load_all(paths: list[str]) -> list[al.Recording]:
    wavs = al.find_wavs(paths)
    if not wavs:
        sys.exit("No .wav files found.")
    recs = [al.load_recording(w) for w in wavs]
    for r in recs:
        if r.rate != al.SAMPLE_RATE:
            sys.exit(f"{r.wav}: sample rate {r.rate} Hz, expected {al.SAMPLE_RATE} Hz")
    return recs


def evaluate(recs: list[al.Recording], words: list[bytes], settings: dict[str, str]):
    per_file = []
    for rec, w in zip(recs, words):
        ev = run_harness(w, rec.rate, settings)
        per_file.append((rec, ev, al.score_events(rec, [e[0] for e in ev])))
    return per_file


def totals(per_file) -> tuple[al.Score, dict[str, al.Score]]:
    total = al.Score()
    by_label: dict[str, al.Score] = {}
    for rec, _ev, s in per_file:
        total.add(s)
        by_label.setdefault(rec.label, al.Score()).add(s)
    return total, by_label


def fmt_rate(s: al.Score) -> str:
    return f"{100 * s.rate:5.1f} %" if s.rate is not None else "    –  "


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------
def cmd_synth(args) -> None:
    paths = al.synth_set(Path(args.out), seed=args.seed)
    for p in paths:
        print(f"wrote {p}")
    print("\nThese are synthetic, made with the same model as the unit tests. They check the tooling and "
          "your implementation end to end; real recordings are what count for tuning.\n"
          "Replay them with the detector's own defaults, which they are built for:\n"
          "  python tools/clap_lab.py replay " + str(args.out) + " "
          + " ".join(f"--set {k}={v}" for k, v in SYNTH_SETTINGS.items()))


def cmd_replay(args) -> None:
    build_harness(args.rebuild)
    recs = load_all(args.paths)
    settings = parse_sets(args.set)
    per_file = evaluate(recs, [to_words(r) for r in recs], settings)
    print(f"{'file':44s} {'label':20s} {'len':>7s} {'prompts':>7s} {'hits':>5s} {'rate':>8s} {'false':>5s}")
    for rec, ev, s in per_file:
        print(f"{rec.wav.name[:44]:44s} {rec.label[:20]:20s} {rec.seconds:6.0f}s {s.prompts:7d} {s.hits:5d} "
              f"{fmt_rate(s):>8s} {s.false:5d}")
        if args.verbose:
            for first, second, conf in ev:
                kind = "hit" if first / rec.rate in s.hit_times_s else "FALSE"
                print(f"    {first / rec.rate:8.2f} s  gap {1000 * (second - first) / rec.rate:5.0f} ms  "
                      f"confirmed +{1000 * (conf - second) / rec.rate:4.0f} ms  {kind}")
    total, by_label = totals(per_file)
    print("\nby label:")
    for label, s in sorted(by_label.items()):
        print(f"  {label:28s} prompts {s.prompts:4d}  hits {s.hits:4d}  rate {fmt_rate(s)}  false {s.false:3d}  "
              f"({s.seconds / 60:.1f} min)")
    hours = total.seconds / 3600
    print(f"\nTOTAL  detection {fmt_rate(total)} ({total.hits}/{total.prompts})   false triggers {total.false} "
          f"in {hours:.2f} h" + (f" = {total.false / hours:.1f} per hour" if hours > 0 else ""))
    if settings:
        print("settings: " + ", ".join(f"{k}={v}" for k, v in settings.items()))


def cmd_tune(args) -> None:
    build_harness(args.rebuild)
    recs = load_all(args.paths)
    base = parse_sets(args.set)
    grid: list[tuple[str, list[str]]] = []
    for g in args.grid or []:
        if "=" not in g:
            sys.exit(f"--grid expects key=v1,v2,..., got '{g}'")
        k, vs = g.split("=", 1)
        if k not in SETTING_KEYS:
            sys.exit(f"Unknown setting '{k}'.")
        grid.append((k, vs.split(",")))
    if not grid:
        sys.exit("Give at least one --grid key=v1,v2,...")
    combos = [dict(zip([k for k, _ in grid], vals)) for vals in itertools.product(*[v for _, v in grid])]
    n_pos = sum(r.prompted for r in recs)
    print(f"{len(combos)} combinations x {len(recs)} recordings ({n_pos} prompted) ...")
    if not n_pos:
        print("Note: no prompted recordings, so only false triggers can be measured.")
    words = [to_words(r) for r in recs]

    def run(combo: dict[str, str]):
        settings = {**base, **combo}
        total, _ = totals(evaluate(recs, words, settings))
        return settings, total

    with ThreadPoolExecutor(max_workers=args.jobs or os.cpu_count() or 4) as pool:
        results = list(pool.map(run, combos))
    # fewest false triggers, then best detection, then fewest changes from the defaults
    results.sort(key=lambda r: (r[1].false, -(r[1].rate or 0.0), changes_from_defaults(r[0])))

    keys = [k for k, _ in grid]
    print("\n" + "  ".join(f"{k:>20s}" for k in keys) + f"  {'detection':>10s}  {'false':>5s}")
    for settings, total in results[: args.top]:
        print("  ".join(f"{settings[k]:>20s}" for k in keys) + f"  {fmt_rate(total):>10s}  {total.false:5d}")
    best_settings, best = results[0]
    print(f"\nBest: detection {fmt_rate(best)}, {best.false} false triggers. On the device console:")
    for k in keys:
        print(f"  set {k} {best_settings[k]}")
    print("\nBeware of tuning to this exact set of recordings: keep a few recordings aside and check the best "
          "settings on them with 'replay' before you trust them.")


def cmd_plot(args) -> None:
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        sys.exit("matplotlib is missing: pip install -r tools/requirements.txt")
    build_harness(args.rebuild)
    settings = parse_sets(args.set)
    for wav in al.find_wavs(args.paths):
        rec = al.load_recording(wav)
        ev = run_harness(to_words(rec), rec.rate, settings)
        s = al.score_events(rec, [e[0] for e in ev])
        hop = rec.rate // 100
        x = rec.samples24.astype(np.float64)
        x -= x.mean()
        n = len(x) // hop
        frames = x[: n * hop].reshape(n, hop)
        lvl = 20 * np.log10(np.maximum(np.sqrt(np.mean(frames ** 2, axis=1)) / al.FULL_SCALE_24, 1e-8))
        t = np.arange(n) * hop / rec.rate

        fig, ax = plt.subplots(figsize=(max(8.0, rec.seconds / 4), 3.6))
        ax.plot(t, lvl, lw=0.7, color="#37474f")
        for p in rec.prompts_s:
            ax.axvspan(p, p + rec.window_s, color="#90caf9", alpha=0.35, lw=0)
        for first, second, _conf in ev:
            hit = first / rec.rate in s.hit_times_s
            c = "#2e7d32" if hit else "#c62828"
            ax.axvline(first / rec.rate, color=c, lw=1.2)
            ax.axvline(second / rec.rate, color=c, lw=1.2, ls="--")
        ax.set_ylim(-100, 0)
        ax.set_xlim(0, rec.seconds)
        ax.set_xlabel("time (s)")
        ax.set_ylabel("level (dBFS, 10 ms RMS)")
        ax.set_title(f"{wav.name}  ·  {rec.label}  ·  hits {s.hits}/{s.prompts}  ·  false {s.false}  "
                     f"(blue: prompt windows, green: detected, red: false)", fontsize=9)
        ax.grid(alpha=0.3)
        out = Path(args.out) / (wav.stem + ".png") if args.out else wav.with_suffix(".png")
        out.parent.mkdir(parents=True, exist_ok=True)
        fig.tight_layout()
        fig.savefig(out, dpi=120)
        plt.close(fig)
        print(f"wrote {out}")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("synth", help="write a synthetic labelled test set")
    s.add_argument("out", nargs="?", default="recordings/synth")
    s.add_argument("--seed", type=int, default=1)

    for name, helptext in (("replay", "score recordings with one set of settings"),
                           ("tune", "grid-search settings"), ("plot", "level plot with detections (PNG)")):
        p = sub.add_parser(name, help=helptext)
        p.add_argument("paths", nargs="+", help=".wav files or folders")
        p.add_argument("--set", action="append", metavar="KEY=VALUE", help="override a setting (repeatable)")
        p.add_argument("--rebuild", action="store_true", help="force rebuilding the harness")
        if name == "replay":
            p.add_argument("-v", "--verbose", action="store_true", help="list every detection")
        if name == "tune":
            p.add_argument("--grid", action="append", metavar="KEY=V1,V2,...", help="values to try (repeatable)")
            p.add_argument("--top", type=int, default=10)
            p.add_argument("--jobs", type=int, default=0, help="parallel runs (default: CPU count)")
        if name == "plot":
            p.add_argument("--out", default=None, help="folder for the PNGs (default: next to the .wav)")

    args = ap.parse_args()
    {"synth": cmd_synth, "replay": cmd_replay, "tune": cmd_tune, "plot": cmd_plot}[args.cmd](args)


if __name__ == "__main__":
    main()
