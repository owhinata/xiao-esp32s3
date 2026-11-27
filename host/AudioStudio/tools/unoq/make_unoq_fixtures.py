#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Writes the fixtures of audio_stream.py from zaitaku-mealpace's decimation.

Run with the interpreter of the zaitaku-mealpace checkout (numpy only):

    <mealpace>/.venv/bin/python -B tools/unoq/make_unoq_fixtures.py \
        --mealpace <mealpace> --out tests/AudioStudio.Core.Tests/Fixtures

It imports features_np from <mealpace>/analysis and writes nothing into that
checkout. The input is synthetic_input(): formulas and a fixed linear
congruential generator, 0.6 s of 48 kHz stereo. It runs audio_stream.py
--stdin on that input, checks that its samples are the first samples of
features_np.decimate3 of the left channel (rounded and clipped to 16 bits),
and writes to --out:

  unoq_decimate3.s16  features_np.decimate3 of the left channel, rounded and
                      clipped, as 16-bit little-endian samples (all
                      ceil(n / 3) of them; audio_stream.py gives all but the
                      last 10, which need input after the end)
  unoq_stream.xaud    what audio_stream.py --stdin writes for the input

test_audio_stream.py checks audio_stream.py against these files, and the
AudioStudio tests stream unoq_stream.xaud through a fake ssh.
"""

import argparse
import subprocess
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
FRAMES = 28800  # 0.6 s at 48 kHz


def synthetic_input(frames=FRAMES):
    """(frames, 2) int16: tones in and above the 8 kHz pass band, noise, and a clipped burst on the left; a tone on the right."""
    t = np.arange(frames) / 48000.0
    state = 12345
    noise = np.empty(frames)
    for i in range(frames):
        state = (1103515245 * state + 12345) & 0x7FFFFFFF
        noise[i] = state / 0x7FFFFFFF * 2.0 - 1.0
    left = (9000 * np.sin(2 * np.pi * 440 * t) + 6000 * np.sin(2 * np.pi * 5000 * t)
            + 5000 * np.sin(2 * np.pi * 11000 * t) + 2500 * noise)
    left[12000:12600] = 40000 * np.sign(np.sin(2 * np.pi * 300 * t[12000:12600]))
    right = 4000 * np.sin(2 * np.pi * 1000 * t)
    both = np.stack([left, right], axis=1)
    return np.clip(np.rint(both), -32768, 32767).astype("<i2")


def run_tool(raw, python=sys.executable):
    p = subprocess.run([python, "-B", str(HERE / "audio_stream.py"), "--stdin"], input=raw, capture_output=True,
                       timeout=60, check=False)
    if p.returncode != 0:
        raise SystemExit(f"audio_stream.py --stdin: exit {p.returncode}: {p.stderr.decode(errors='replace')}")
    return p.stdout


def stream_samples(data):
    """The samples of an XAUD stream (the header and block framing are not checked here)."""
    off = 16
    out = []
    while off < len(data):
        count = int.from_bytes(data[off + 4 : off + 6], "little")
        off += 8
        out.append(np.frombuffer(data, "<i2", count, off))
        off += 2 * count
    return np.concatenate(out)


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--mealpace", required=True, type=Path, help="a zaitaku-mealpace checkout")
    p.add_argument("--out", required=True, type=Path, help="the fixtures directory")
    args = p.parse_args()
    sys.dont_write_bytecode = True
    sys.path.insert(0, str(args.mealpace / "analysis"))
    import features_np  # noqa: E402

    raw = synthetic_input()
    expected = np.clip(np.rint(features_np.decimate3(raw[:, 0].astype(np.float64))), -32768, 32767).astype("<i2")
    stream = run_tool(raw.tobytes())
    got = stream_samples(stream)
    if not np.array_equal(got, expected[: len(got)]) or len(got) != len(expected) - 10:
        raise SystemExit(f"audio_stream.py differs from features_np.decimate3 ({len(got)} samples, {len(expected)} expected)")
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "unoq_decimate3.s16").write_bytes(expected.tobytes())
    (args.out / "unoq_stream.xaud").write_bytes(stream)
    print(f"{len(got)} samples equal to features_np.decimate3; wrote {args.out}/unoq_decimate3.s16, unoq_stream.xaud")


if __name__ == "__main__":
    main()
