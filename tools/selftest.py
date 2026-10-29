#!/usr/bin/env python3
"""Checks the tooling before you need it:

  1. capture protocol: the C frame encoder of the recorder firmware is compiled
     natively and its byte stream (with garbage, a corrupted frame and a
     dropped frame mixed in) is decoded by the Python side;
  2. lab pipeline: synthetic recordings are replayed through YOUR
     mic_frontend and clap_detector (skipped until both are implemented).

  python tools/selftest.py
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
import audiolib as al  # noqa: E402

GEN_C = r"""
#include <stdio.h>
#include <stdint.h>
#include "frame.h"
int main(void) {
    static int32_t s[256];
    static uint8_t out[FRAME_BYTES(256)];
    fwrite("boot garbage AUD", 1, 16, stdout);          /* bootloader text incl. a false magic */
    for (uint32_t seq = 0; seq < 12; seq++) {
        for (int i = 0; i < 256; i++) {
            int32_t v = (int32_t)((seq * 256 + i) * 2731 % 16777216) - 8388608;
            s[i] = v;
        }
        size_t len = frame_encode(out, sizeof out, 1000 + seq, seq == 5 ? 1 : 0, s, 256);
        if (seq == 3) continue;                          /* dropped frame */
        if (seq == 7) out[40] ^= 0xFF;                   /* corrupted frame: CRC must reject it */
        fwrite(out, 1, len, stdout);
        if (seq == 9) fwrite("xx", 1, 2, stdout);        /* noise between frames */
    }
    return 0;
}
"""


def expected_samples(seq: int) -> np.ndarray:
    i = np.arange(256, dtype=np.int64)
    return (((seq * 256 + i) * 2731) % 16777216 - 8388608).astype(np.int32)


def check_protocol(cc: str) -> bool:
    print("1. capture protocol")
    with tempfile.TemporaryDirectory() as tmp:
        gen = Path(tmp) / "gen.c"
        gen.write_text(GEN_C)
        exe = Path(tmp) / ("gen.exe" if os.name == "nt" else "gen")
        rec_src = TOOLS / "recorder" / "src"
        res = subprocess.run([cc, "-std=c11", "-O1", f"-I{rec_src}", str(gen), str(rec_src / "frame.c"),
                              "-o", str(exe)], capture_output=True, text=True)
        if res.returncode != 0:
            print("   FAIL: could not compile the frame encoder:\n" + res.stderr)
            return False
        stream = subprocess.run([str(exe)], capture_output=True).stdout

    dec = al.FrameDecoder()
    trk = al.SequenceTracker()
    frames, gaps = [], 0
    for k in range(0, len(stream), 97):  # odd chunk size, like a serial port
        for f in dec.feed(stream[k : k + 97]):
            gaps += trk.check(f)
            frames.append(f)
    ok = True

    def expect(cond: bool, what: str) -> None:
        nonlocal ok
        print(f"   {'ok  ' if cond else 'FAIL'} {what}")
        ok &= cond

    seqs = [f.seq - 1000 for f in frames]
    expect(seqs == [0, 1, 2, 4, 5, 6, 8, 9, 10, 11], f"frames decoded in order, 3 dropped and 7 rejected: {seqs}")
    expect(dec.crc_errors >= 1, f"corrupted frame rejected by CRC ({dec.crc_errors} CRC error)")
    expect(trk.frames_lost == 2 and gaps == 512, f"gaps detected: {trk.frames_lost} frames, {gaps} samples")
    expect(trk.overflow_frames == 1, "overflow flag seen once")
    expect(all(np.array_equal(al.payload_to_int24(f.payload), expected_samples(f.seq - 1000)) for f in frames),
           "every sample value identical to what the encoder sent")
    rt = al.payload_to_int24(al.int24_to_payload(np.array([-8388608, -1, 0, 1, 8388607])))
    expect(rt.tolist() == [-8388608, -1, 0, 1, 8388607], "24-bit pack/unpack round trip incl. extremes")
    return ok


def check_lab() -> bool:
    print("2. lab pipeline on synthetic recordings")
    import clap_lab  # noqa: E402

    clap_lab.build_harness(force=True)
    with tempfile.TemporaryDirectory() as tmp:
        recs = [al.load_recording(p) for p in al.synth_set(Path(tmp), seed=1)]
        res = subprocess.run([str(clap_lab.EXE)], input=b"", capture_output=True)
        if res.returncode == 3:
            print("   skipped: lib/mic_frontend or lib/clap_detector not implemented yet")
            return True
        per_file = clap_lab.evaluate(recs, [clap_lab.to_words(r) for r in recs], {})
        total, by_label = clap_lab.totals(per_file)
        for label, s in sorted(by_label.items()):
            print(f"   {label:22s} prompts {s.prompts:3d}  hits {s.hits:3d}  false {s.false:3d}")
        print(f"   total: detection {clap_lab.fmt_rate(total).strip()}, {total.false} false triggers")
        print("   (synthetic scenes: a healthy detector gets close to 100 % and 0 false; real recordings decide)")
    return True


def main() -> None:
    cc = os.environ.get("CC") or shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")
    if not cc:
        sys.exit("No C compiler found (gcc). See stage 1.3 of the build guide.")
    ok = check_protocol(cc)
    ok &= check_lab()
    print("\nselftest " + ("passed" if ok else "FAILED"))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
