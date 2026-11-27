#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Stream a USB audio adapter of the Arduino UNO Q as AudioStudio's XAUD stream.

Runs on the Linux side of the UNO Q (Python 3, numpy). AudioStudio starts it
over ssh (targets/unoq.json):

    flock -n -F -E 75 /home/arduino python3 -B ~/audiostudio-unoq/audio_stream.py

flock takes the lock that the other users of the adapter on the target take
too; while one of them holds it, flock exits with 75 and this program does
not run.

It starts arecord (48 kHz, 2 channels, S16_LE, raw to standard output, with
the arguments of zaitaku-mealpace's device/unoq/audio_in.py), takes the left
channel, decimates it by 3 to 16 kHz with a 61-tap low-pass filter (the
coefficients of scipy's resample_poly(x, 1, 3): a Kaiser window with beta 5,
cut-off at 1/3, normalised to a sum of 1), and writes to standard output the
XAUD version 1 stream that the XIAO's audio service sends
(shell/port/audio_service.c): a 16-byte header ("XAUD", version 1, format 1
= signed 16-bit little-endian mono, 2 reserved bytes, the rate 16000 as 32
bits, 4 reserved bytes), then blocks of an 8-byte header (the index of the
block's first sample, 32 bits, wrapping; the sample count, 16 bits; flags,
16 bits) and the samples. Blocks have 320 samples (20 ms); the first one has
flag bit 1 (the first block of a stream); when arecord gives nothing for a
second, a block with no samples and no flag keeps the connection alive.

arecord reports an overrun ("overrun!!! (at least N ms long)") without the
number of frames it dropped, so the loss cannot be put into the sample
index: each overrun is counted and written to standard error.

It ends, and ends arecord, when its standard output is closed (EPIPE), on
SIGTERM, SIGHUP or SIGINT, and when arecord ends. It writes no file and sends
the audio nowhere but its standard output.

  --device PCM   the ALSA capture device (default hw:CARD=Audio,DEV=0); only
                 hw: and plughw: card and device names are taken
  --stdin        read raw 48 kHz stereo S16_LE from standard input instead of
                 starting arecord (for tests); ends at the end of the input,
                 with the samples of the last, shorter block

Exit status: 0 when stopped (EPIPE, a signal, the end of --stdin), 1 when
arecord cannot be started or ends by itself with an error, 2 for a bad
argument.
"""

import argparse
import collections
import errno
import os
import re
import select
import signal
import stat
import struct
import subprocess
import sys
import threading

import numpy as np

RATE_IN = 48000
CHANNELS = 2
FRAME_BYTES = 2 * CHANNELS  # S16_LE
DECIMATE = 3
RATE_OUT = RATE_IN // DECIMATE
TAPS = 61
HALF = (TAPS - 1) // 2  # 30: the filter's delay in input samples
KAISER_BETA = 5.0
READ_BYTES = 65536

MAGIC = b"XAUD"
VERSION = 1
FORMAT_S16LE_MONO = 1
STREAM_HEADER = struct.Struct("<4sBBHII")
BLOCK_HEADER = struct.Struct("<IHH")
BLOCK_SAMPLES = 320
FLAG_NEW_STREAM = 0x0002
KEEPALIVE_S = 1.0

# The PCM names zaitaku-mealpace's audio_in.py takes: hw or plughw with a
# card (and device). Plugins such as tee: or file: could copy the input to a
# file, so they are not taken.
PCM_RE = r"(plug)?hw:(CARD=[A-Za-z0-9_]+(,DEV=[0-9]+)?|[0-9]+(,[0-9]+)?)"
DEFAULT_DEVICE = "hw:CARD=Audio,DEV=0"
ARECORD = "arecord"
# Output to standard output only. No -q: arecord then reports overruns.
ARECORD_ARGS = (
    "-f", "S16_LE", "-r", "48000", "-c", "2", "-t", "raw",
    "-B", "2000000", "-F", "125000", "-v", "-",
)  # fmt: skip
STOP_WAIT_S = 2.0
STDERR_TAIL = 8


class Stop(Exception):
    """A signal asked the program to end."""


class InputError(Exception):
    """The input cannot be opened."""


def lowpass_taps():
    """The 61 taps: sinc with cut-off 1/3 under a Kaiser window (beta 5), sum 1."""
    m = np.arange(TAPS) - (TAPS - 1) / 2
    h = np.sinc(m / DECIMATE) / DECIMATE * np.kaiser(TAPS, KAISER_BETA)
    return h / h.sum()


class Decimator:
    """Decimation by 3, block by block.

    Output k is the sum over i of taps[i] * x[3k + 30 - i], with x = 0 before
    the first input sample; it is given once input 3k + 30 has come, so the
    output lags the input by 30 samples. The state is the input that later
    outputs still need.
    """

    def __init__(self):
        self._taps_reversed = lowpass_taps()[::-1].copy()
        self._buf = np.zeros(HALF)  # starts with the zeros before input 0
        self._buf_start = -HALF  # input index of _buf[0]
        self._inputs = 0
        self._next = 0  # index of the next output

    def process(self, block):
        """Takes input samples (float64); returns the outputs they complete."""
        self._buf = np.concatenate((self._buf, np.asarray(block, dtype=np.float64)))
        self._inputs += len(block)
        end = (self._inputs - 1 - HALF) // DECIMATE + 1
        count = max(0, end - self._next)
        if count == 0:
            return np.zeros(0)
        first = DECIMATE * self._next - HALF - self._buf_start
        span = DECIMATE * count
        y = np.zeros(count)
        for i in range(TAPS):
            start = first + i
            y += self._taps_reversed[i] * self._buf[start : start + span : DECIMATE]
        self._next = end
        keep = DECIMATE * self._next - HALF - self._buf_start
        self._buf = self._buf[keep:].copy()
        self._buf_start += keep
        return y


def to_s16(y):
    """Rounds to the nearest integer and clips to 16 bits."""
    return np.clip(np.rint(y), -32768, 32767).astype("<i2")


class LeftChannel:
    """Raw S16_LE stereo bytes to the left channel as float64; a partial frame is kept for the next call."""

    def __init__(self):
        self._rest = b""

    def push(self, data):
        data = self._rest + data
        whole = len(data) // FRAME_BYTES * FRAME_BYTES
        self._rest = data[whole:]
        frames = np.frombuffer(data[:whole], dtype="<i2")
        return frames[::CHANNELS].astype(np.float64)


def write_all(fd, data):
    view = memoryview(data)
    while view:
        n = os.write(fd, view)
        view = view[n:]


class XaudWriter:
    """Writes the stream header and the blocks of 320 samples."""

    def __init__(self, fd):
        self._fd = fd
        self._index = 0
        self._pending = np.zeros(0, dtype="<i2")
        self._first = True

    def header(self):
        write_all(self._fd, STREAM_HEADER.pack(MAGIC, VERSION, FORMAT_S16LE_MONO, 0, RATE_OUT, 0))

    def push(self, samples):
        self._pending = np.concatenate((self._pending, samples))
        while len(self._pending) >= BLOCK_SAMPLES:
            self._block(self._pending[:BLOCK_SAMPLES])
            self._pending = self._pending[BLOCK_SAMPLES:]

    def keepalive(self):
        write_all(self._fd, BLOCK_HEADER.pack(self._index, 0, 0))

    def finish(self):
        """Writes the samples of a last, shorter block."""
        if len(self._pending):
            self._block(self._pending)
            self._pending = self._pending[:0]

    def _block(self, samples):
        flags = FLAG_NEW_STREAM if self._first else 0
        self._first = False
        write_all(self._fd, BLOCK_HEADER.pack(self._index, len(samples), flags) + samples.tobytes())
        self._index = (self._index + len(samples)) & 0xFFFFFFFF


def lock_fds(fd_dir="/proc/self/fd"):
    """The inherited directory descriptors (above 2): the lock flock -F holds.

    They are passed on to arecord, so that the lock stays while arecord runs,
    also if this program is killed. Raises InputError when the descriptors
    cannot be checked (then arecord is not started).
    """
    try:
        names = os.listdir(fd_dir)
    except OSError as e:
        raise InputError(f"cannot list the inherited descriptors ({fd_dir}): {e}") from None
    out = []
    for name in names:
        if not name.isdigit() or int(name) <= 2:
            continue
        fd = int(name)
        try:
            st = os.fstat(fd)
        except OSError as e:
            if e.errno == errno.EBADF:
                continue  # the descriptor listdir used, closed by now
            raise InputError(f"cannot check inherited descriptor {fd}: {e}") from None
        if stat.S_ISDIR(st.st_mode):
            out.append(fd)
    return tuple(sorted(out))


class Arecord:
    """arecord as a child process; its standard error read line by line on a thread."""

    def __init__(self, device):
        argv = [ARECORD, "-D", device, *ARECORD_ARGS]
        try:
            self.proc = subprocess.Popen(
                argv,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                pass_fds=lock_fds(),
            )
        except OSError as e:
            raise InputError(f"cannot start {ARECORD}: {e}") from None
        self.overruns = 0
        self.tail = collections.deque(maxlen=STDERR_TAIL)
        self._err = threading.Thread(target=self._read_stderr, daemon=True)
        self._err.start()

    def fileno(self):
        return self.proc.stdout.fileno()

    def _read_stderr(self):
        try:
            for raw in self.proc.stderr:
                line = raw.decode("utf-8", "replace").rstrip()
                self.tail.append(line)
                if "overrun" in line.lower():
                    self.overruns += 1
                    note(f"arecord overrun {self.overruns} (the samples it dropped are not counted): {line}")
        except (OSError, ValueError):
            pass

    def stop(self):
        """SIGTERM, then SIGKILL after STOP_WAIT_S. Returns the exit code."""
        if self.proc.poll() is None:
            try:
                self.proc.terminate()
                self.proc.wait(timeout=STOP_WAIT_S)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
            except ProcessLookupError:
                pass
        for f in (self.proc.stdout, self.proc.stderr):
            try:
                f.close()
            except OSError:
                pass
        return self.proc.returncode

    def stderr_tail(self):
        self._err.join(timeout=0.5)
        return list(self.tail)


def note(text):
    print("audio_stream: " + text, file=sys.stderr, flush=True)


def on_signal(signum, frame):
    raise Stop()


def pump(fd, writer, keepalive):
    """Reads from fd until its end, writes the stream. keepalive: send a block when nothing came for a second."""
    left = LeftChannel()
    decimator = Decimator()
    while True:
        if keepalive and not select.select([fd], [], [], KEEPALIVE_S)[0]:
            writer.keepalive()
            continue
        data = os.read(fd, READ_BYTES)
        if not data:
            return
        writer.push(to_s16(decimator.process(left.push(data))))


def main():
    p = argparse.ArgumentParser(description="Stream the USB audio adapter as AudioStudio's XAUD stream (16 kHz mono).")
    p.add_argument("--device", default=DEFAULT_DEVICE, help=f"ALSA capture device (default {DEFAULT_DEVICE})")
    p.add_argument("--stdin", action="store_true", help="read raw 48 kHz stereo S16_LE from standard input")
    args = p.parse_args()
    if not re.fullmatch(PCM_RE, args.device):
        p.error(f"--device takes hw:CARD=<name>[,DEV=<n>] or hw:<n>[,<n>] (or plughw:): {args.device!r}")

    for signum in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT):
        signal.signal(signum, on_signal)
    writer = XaudWriter(sys.stdout.fileno())
    arecord = None
    status = 0
    try:
        writer.header()
        if args.stdin:
            pump(sys.stdin.fileno(), writer, keepalive=False)
            writer.finish()
        else:
            arecord = Arecord(args.device)
            pump(arecord.fileno(), writer, keepalive=True)
            code = arecord.stop()
            if code != 0:
                tail = arecord.stderr_tail()
                note(f"arecord ended with exit code {code}" + (": " + tail[-1] if tail else ""))
                status = 1
    except InputError as e:
        note(str(e))
        status = 1
    except BrokenPipeError:
        # The reader went away. Nothing more can be written: standard output
        # goes to /dev/null, so that the interpreter's exit does not fail on it.
        devnull = os.open(os.devnull, os.O_WRONLY)
        os.dup2(devnull, sys.stdout.fileno())
        os.close(devnull)
    except Stop:
        pass
    finally:
        for signum in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT):
            signal.signal(signum, signal.SIG_IGN)
        if arecord is not None:
            arecord.stop()
    return status


if __name__ == "__main__":
    sys.exit(main())
