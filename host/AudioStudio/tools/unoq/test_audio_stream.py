#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests of audio_stream.py, on any computer with Python 3 and numpy:

    python3 -B -m unittest discover -s tools/unoq -p 'test_*.py'

The decimation is checked against the fixtures in
tests/AudioStudio.Core.Tests/Fixtures/ (written by make_unoq_fixtures.py
from zaitaku-mealpace's features_np.decimate3). The arecord path runs with a
fake arecord first on the PATH; nothing here touches an audio device.
"""

import os
import select
import signal
import struct
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.dont_write_bytecode = True
sys.path.insert(0, str(HERE))
import audio_stream  # noqa: E402
from make_unoq_fixtures import synthetic_input  # noqa: E402

FIXTURES = HERE.parent.parent / "tests" / "AudioStudio.Core.Tests" / "Fixtures"
TOOL = [sys.executable, "-B", str(HERE / "audio_stream.py")]


def parse(data):
    """(rate, [(index, flags, samples)]) of an XAUD stream; checks the header and the framing."""
    magic, version, fmt, _, rate, _ = struct.unpack_from("<4sBBHII", data, 0)
    assert (magic, version, fmt) == (b"XAUD", 1, 1), (magic, version, fmt)
    off, blocks = 16, []
    while off < len(data):
        assert off + 8 <= len(data), "a cut block header"
        index, count, flags = struct.unpack_from("<IHH", data, off)
        off += 8
        assert count <= 1024 and off + 2 * count <= len(data), "a cut block"
        blocks.append((index, flags, np.frombuffer(data, "<i2", count, off)))
        off += 2 * count
    return rate, blocks


def read_exactly(stream, n, timeout=10.0):
    data = b""
    end = time.monotonic() + timeout
    while len(data) < n:
        if not select.select([stream], [], [], max(0.0, end - time.monotonic()))[0]:
            raise AssertionError(f"only {len(data)} of {n} bytes within {timeout} s")
        chunk = os.read(stream.fileno(), n - len(data))
        if not chunk:
            raise AssertionError(f"the end after {len(data)} of {n} bytes")
        data += chunk
    return data


def gone(pid, timeout=5.0):
    """The process has ended (and is not a zombie) within the timeout."""
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        try:
            with open(f"/proc/{pid}/stat") as f:
                if f.read().rsplit(")", 1)[1].split()[0] == "Z":
                    return True
        except FileNotFoundError:
            return True
        time.sleep(0.05)
    return False


class Decimation(unittest.TestCase):
    def test_stdin_gives_the_fixture(self):
        raw = synthetic_input().tobytes()
        p = subprocess.run(TOOL + ["--stdin"], input=raw, capture_output=True, timeout=60)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(p.stderr, b"")
        self.assertEqual(p.stdout, (FIXTURES / "unoq_stream.xaud").read_bytes())
        rate, blocks = parse(p.stdout)
        self.assertEqual(rate, 16000)
        expected = np.frombuffer((FIXTURES / "unoq_decimate3.s16").read_bytes(), "<i2")
        samples = np.concatenate([b[2] for b in blocks])
        self.assertEqual(len(samples), len(expected) - 10)
        self.assertTrue(np.array_equal(samples, expected[: len(samples)]))
        index = 0
        for i, (block_index, flags, s) in enumerate(blocks):
            self.assertEqual(block_index, index)
            self.assertEqual(flags, 2 if i == 0 else 0)
            if i < len(blocks) - 1:
                self.assertEqual(len(s), 320)
            index += len(s)

    def test_blocks_of_any_size_give_the_same(self):
        raw = synthetic_input()
        left = raw[:, 0].astype(np.float64)
        whole = audio_stream.Decimator().process(left)
        for size in (1, 2, 3, 7, 160, 4801):
            d = audio_stream.Decimator()
            parts = [d.process(left[i : i + size]) for i in range(0, len(left), size)]
            self.assertTrue(np.array_equal(np.concatenate(parts), whole), size)

    def test_a_partial_frame_waits_for_the_rest(self):
        data = synthetic_input(300).tobytes()
        ch = audio_stream.LeftChannel()
        got = np.concatenate([ch.push(data[i : i + 5]) for i in range(0, len(data), 5)])
        self.assertTrue(np.array_equal(got, synthetic_input(300)[:, 0].astype(np.float64)))

    def test_closed_output_ends_it(self):
        p = subprocess.Popen(TOOL + ["--stdin"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        read_exactly(p.stdout, 16)
        p.stdout.close()
        chunk = bytes(48000 * 4 // 10)
        try:
            for _ in range(200):
                p.stdin.write(chunk)
                p.stdin.flush()
                if p.poll() is not None:
                    break
        except BrokenPipeError:
            pass
        self.assertEqual(p.wait(timeout=10), 0)
        self.assertEqual(p.stderr.read(), b"")
        p.stderr.close()
        try:
            p.stdin.close()
        except BrokenPipeError:
            pass

    def test_a_bad_device_is_refused(self):
        p = subprocess.run(TOOL + ["--device", "file:/tmp/x"], capture_output=True, timeout=30)
        self.assertEqual(p.returncode, 2)
        self.assertEqual(p.stdout, b"")


FAKE_ARECORD = textwrap.dedent("""\
    #!{python}
    import os, sys, time
    with open({log!r}, "a") as f:
        f.write(f"{{os.getpid()}} {{' '.join(sys.argv[1:])}}\\n")
    mode = {mode!r}
    if mode == "fail":
        print("arecord: main:850: audio open error: No such file or directory", file=sys.stderr, flush=True)
        sys.exit(1)
    print("Plug PCM: Hardware PCM card 1 'USB Audio' device 0 subdevice 0", file=sys.stderr, flush=True)
    if mode == "silent":
        time.sleep(1.6)
    frame = (1000).to_bytes(2, "little", signed=True) + (-7).to_bytes(2, "little", signed=True)
    for n in range(100000):
        sys.stdout.buffer.write(frame * 4800)
        sys.stdout.buffer.flush()
        if n == 2:
            print("overrun!!! (at least 1.234 ms long)", file=sys.stderr, flush=True)
        time.sleep(0.05)
    """)


class Live(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.log = Path(self.dir.name) / "arecord.log"

    def tearDown(self):
        self.dir.cleanup()

    def start(self, mode="run", args=()):
        fake = Path(self.dir.name) / "arecord"
        fake.write_text(FAKE_ARECORD.format(python=sys.executable, log=str(self.log), mode=mode))
        fake.chmod(0o755)
        env = dict(os.environ, PATH=self.dir.name + os.pathsep + os.environ.get("PATH", ""))
        return subprocess.Popen(TOOL + list(args), stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, env=env)

    def arecord(self):
        end = time.monotonic() + 10
        while time.monotonic() < end:
            if self.log.exists() and self.log.read_text().strip():
                pid, _, argv = self.log.read_text().splitlines()[0].partition(" ")
                return int(pid), argv
            time.sleep(0.05)
        raise AssertionError("arecord was not started")

    def blocks(self, p, n):
        rate, _ = parse(read_exactly(p.stdout, 16))
        self.assertEqual(rate, 16000)
        out = []
        for _ in range(n):
            index, count, flags = struct.unpack("<IHH", read_exactly(p.stdout, 8))
            out.append((index, flags, np.frombuffer(read_exactly(p.stdout, 2 * count), "<i2")))
        return out

    def test_arecord_gets_the_arguments_and_the_left_channel_is_sent(self):
        p = self.start(args=["--device", "hw:CARD=Mic,DEV=1"])
        try:
            blocks = self.blocks(p, 20)
            pid, argv = self.arecord()
            self.assertEqual(argv, "-D hw:CARD=Mic,DEV=1 " + " ".join(audio_stream.ARECORD_ARGS))
            self.assertEqual(blocks[0][1], 2)
            self.assertTrue(all(f == 0 for _, f, _ in blocks[1:]))
            self.assertTrue(all(len(s) == 320 for _, _, s in blocks))
            self.assertEqual([i for i, _, _ in blocks], [320 * k for k in range(20)])
            # A constant 1000 on the left (-7 on the right) once the filter has filled.
            self.assertTrue(np.all(blocks[-1][2] == 1000))
        finally:
            p.send_signal(signal.SIGTERM)
            self.assertEqual(p.wait(timeout=10), 0)
            p.stdout.close()
            err = p.stderr.read().decode()
            p.stderr.close()
        self.assertTrue(gone(pid), "arecord still runs")
        self.assertIn("arecord overrun 1", err)

    def test_sighup_ends_it_and_arecord(self):
        p = self.start()
        self.blocks(p, 2)
        pid, _ = self.arecord()
        p.send_signal(signal.SIGHUP)
        self.assertEqual(p.wait(timeout=10), 0)
        p.stdout.close()
        p.stderr.close()
        self.assertTrue(gone(pid), "arecord still runs")

    def test_closed_output_ends_it_and_arecord(self):
        p = self.start()
        self.blocks(p, 2)
        pid, _ = self.arecord()
        p.stdout.close()
        self.assertEqual(p.wait(timeout=10), 0)
        p.stderr.close()
        self.assertTrue(gone(pid), "arecord still runs")

    def test_a_keepalive_while_arecord_gives_nothing(self):
        p = self.start(mode="silent")
        try:
            index, flags, samples = self.blocks(p, 1)[0]
            self.assertEqual((index, flags, len(samples)), (0, 0, 0))
        finally:
            p.send_signal(signal.SIGTERM)
            p.wait(timeout=10)
            p.stdout.close()
            p.stderr.close()

    def test_arecord_that_fails_ends_it_with_its_message(self):
        p = self.start(mode="fail")
        out, err = p.communicate(timeout=10)
        self.assertEqual(p.returncode, 1)
        self.assertEqual(out, struct.pack("<4sBBHII", b"XAUD", 1, 1, 0, 16000, 0))
        self.assertIn("arecord ended with exit code 1: arecord: main:850: audio open error", err.decode())


if __name__ == "__main__":
    unittest.main()
