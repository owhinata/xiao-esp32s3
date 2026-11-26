#!/usr/bin/env python3
"""Receive the XIAO ESP32S3's 16 kHz microphone samples over Wi-Fi.

Connects to the board's audio stream service (shell/port/audio_service.c,
TCP port 3233), checks the stream header and every block, and prints one
status line per second: the samples received in that second, the gaps
(samples the board lost, a jump of the block index) and whether the board
reports no stream.  The stream itself is started on the board's shell
("usb host start", "uac start").

The protocol: a 16-byte stream header ("XAUD", version 1, format 1 =
signed 16-bit little-endian mono, 2 reserved bytes, the sample rate as a
32-bit integer, 4 reserved bytes), then blocks of an 8-byte header (the
index of the block's first sample, 32 bits, wrapping; the sample count,
16 bits, up to 1024; flags, 16 bits: bit 0 no stream runs, bit 1 the first
block of a new stream) and the samples.  Integers are little-endian.

  --wav FILE    write the samples to a mono 16-bit WAV file, each gap
                filled with zeros
  --seconds N   stop after N seconds (default: until interrupted)

Exit status: 0 when stopped by --seconds or Ctrl-C, 1 for a protocol error
or a connection that ended or went silent, 2 when the connection could not
be made.
"""

import argparse
import socket
import struct
import sys
import time
import wave

PORT = 3233
MAGIC = b"XAUD"
VERSION = 1
FORMAT_S16LE_MONO = 1
STREAM_HEADER = struct.Struct("<4sBBHII")
BLOCK_HEADER = struct.Struct("<IHH")
COUNT_MAX = 1024
FLAG_IDLE = 0x0001
FLAG_NEW_STREAM = 0x0002
FLAGS_KNOWN = FLAG_IDLE | FLAG_NEW_STREAM
CONNECT_TIMEOUT_S = 5.0
# The board sends a block at least once a second.
SILENCE_TIMEOUT_S = 5.0


class ProtocolError(Exception):
    pass


class Closed(Exception):
    pass


def recv_exact(sock, n):
    buf = bytearray()
    while len(buf) < n:
        try:
            chunk = sock.recv(n - len(buf))
        except socket.timeout:
            raise Closed(f"nothing received for {SILENCE_TIMEOUT_S:.0f} s") from None
        except OSError as e:
            raise Closed(f"connection error: {e}") from None
        if not chunk:
            raise Closed("the board closed the connection")
        buf += chunk
    return bytes(buf)


def read_stream_header(sock):
    magic, version, fmt, _, rate, _ = STREAM_HEADER.unpack(recv_exact(sock, STREAM_HEADER.size))
    if magic != MAGIC:
        raise ProtocolError(f"not an audio stream (magic {magic!r})")
    if version != VERSION:
        raise ProtocolError(f"protocol version {version}, this script knows {VERSION}")
    if fmt != FORMAT_S16LE_MONO:
        raise ProtocolError(f"sample format {fmt}, this script knows {FORMAT_S16LE_MONO}")
    if rate == 0:
        raise ProtocolError("sample rate 0")
    return rate


class Totals:
    def __init__(self):
        self.samples = 0
        self.blocks = 0
        self.gaps = 0
        self.gap_samples = 0
        self.new_streams = 0
        self.idle = False


def main():
    p = argparse.ArgumentParser(description="Receive the board's 16 kHz audio stream.")
    p.add_argument("--host", required=True, help="the board's IP address (\"ifconfig\" on the board)")
    p.add_argument("--port", type=int, default=PORT, help=f"TCP port (default {PORT})")
    p.add_argument("--wav", metavar="FILE", help="write the samples to this WAV file")
    p.add_argument("--seconds", type=float, metavar="N", help="stop after N seconds")
    args = p.parse_args()

    try:
        sock = socket.create_connection((args.host, args.port), timeout=CONNECT_TIMEOUT_S)
    except OSError as e:
        print(f"Could not connect to {args.host}:{args.port}: {e}", file=sys.stderr)
        return 2
    sock.settimeout(SILENCE_TIMEOUT_S)

    wav = None
    total = Totals()
    status = 0
    start = time.monotonic()
    try:
        try:
            rate = read_stream_header(sock)
        except Closed as e:
            raise Closed(f"{e} before the stream header (another client is connected?)") from None
        print(f"Connected to {args.host}:{args.port}: {rate} Hz, 16-bit mono", flush=True)
        if args.wav:
            wav = wave.open(args.wav, "wb")
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(rate)

        expected = None             # index of the next sample, None before the first block
        second = Totals()
        next_report = start + 1.0
        while True:
            now = time.monotonic()
            if args.seconds is not None and now - start >= args.seconds:
                break
            if now >= next_report:
                elapsed = now - start
                state = "idle (no stream on the board)" if second.idle else "streaming"
                print(f"{elapsed:6.0f} s  {second.samples:6d} samples/s  gaps {second.gaps} "
                      f"({second.gap_samples} samples)  {state}; total {total.samples} samples, "
                      f"gaps {total.gaps} ({total.gap_samples} samples), "
                      f"new streams {total.new_streams}", flush=True)
                second = Totals()
                second.idle = total.idle
                next_report += 1.0
                if next_report < now:
                    next_report = now + 1.0

            index, count, flags = BLOCK_HEADER.unpack(recv_exact(sock, BLOCK_HEADER.size))
            if count > COUNT_MAX:
                raise ProtocolError(f"block at {index}: count {count} > {COUNT_MAX}")
            if flags & ~FLAGS_KNOWN:
                print(f"Block at {index}: unknown flags 0x{flags:04x}", flush=True)
            if flags & FLAG_IDLE and count != 0:
                raise ProtocolError(f"block at {index}: no-stream flag with {count} samples")
            data = recv_exact(sock, 2 * count) if count else b""

            if expected is not None and index != expected:
                gap = (index - expected) & 0xFFFFFFFF
                if gap >= 0x80000000:
                    raise ProtocolError(f"index went back: {index}, expected {expected}")
                print(f"Gap: {gap} samples lost before index {index}", flush=True)
                for t in (total, second):
                    t.gaps += 1
                    t.gap_samples += gap
                if wav is not None:
                    wav.writeframes(bytes(2 * gap))
            expected = (index + count) & 0xFFFFFFFF

            if flags & FLAG_NEW_STREAM:
                print(f"New stream at index {index}", flush=True)
                total.new_streams += 1
            idle = bool(flags & FLAG_IDLE)
            if idle != total.idle:
                print("The board reports no stream" if idle else "The stream runs", flush=True)
            total.idle = second.idle = idle
            for t in (total, second):
                t.blocks += 1
                t.samples += count
            if wav is not None and count:
                wav.writeframes(data)
    except KeyboardInterrupt:
        pass
    except (ProtocolError, Closed) as e:
        print(f"Stopped: {e}", file=sys.stderr)
        status = 1
    finally:
        sock.close()
        if wav is not None:
            wav.close()

    elapsed = time.monotonic() - start
    print(f"{total.samples} samples in {total.blocks} blocks over {elapsed:.1f} s, "
          f"gaps {total.gaps} ({total.gap_samples} samples), new streams {total.new_streams}"
          + (f"; written to {args.wav}" if args.wav else ""), flush=True)
    return status


if __name__ == "__main__":
    sys.exit(main())
