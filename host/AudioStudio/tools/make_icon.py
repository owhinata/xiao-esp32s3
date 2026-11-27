#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Draws the application icon: a waveform over the bands of a spectrogram.

    python3 tools/make_icon.py src/AudioStudio.App/Assets/icon.png

Needs nothing but the Python standard library (the PNG is written with
zlib). The result is committed; run this only to change the icon.
"""
import math
import struct
import sys
import zlib

SIZE = 256
SCALE = 4                     # drawn larger and reduced, for smooth edges
N = SIZE * SCALE


def png(path, pixels):
    """pixels: rows of (r, g, b, a) tuples."""
    raw = b"".join(b"\0" + bytes(c for p in row for c in p) for row in pixels)

    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data
                + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


def colour(x, y):
    """The large picture's colour at (x, y), or None outside the rounded square."""
    margin, radius = N * 0.06, N * 0.18
    cx = min(max(x, margin + radius), N - margin - radius)
    cy = min(max(y, margin + radius), N - margin - radius)
    if math.hypot(x - cx, y - cy) > radius:
        return None
    # Background: bands of a spectrogram, brighter towards the bottom (low frequencies).
    t = y / N
    band = 0.5 + 0.5 * math.sin(t * 18 + math.sin(x / N * 6) * 1.5)
    level = band * t
    r = 20 + 200 * level
    g = 12 + 70 * level
    b = 60 + 60 * (1 - level)
    # Waveform: a decaying burst around the middle line.
    u = x / N
    envelope = math.exp(-((u - 0.5) / 0.22) ** 2)
    wave = N * 0.5 - N * 0.30 * envelope * math.sin(u * 2 * math.pi * 9)
    if abs(y - wave) < N * 0.022:
        return (90, 200, 250)
    return (r, g, b)


def main(path):
    rows = []
    for py in range(SIZE):
        row = []
        for px in range(SIZE):
            acc = [0.0, 0.0, 0.0, 0.0]
            for sy in range(SCALE):
                for sx in range(SCALE):
                    c = colour(px * SCALE + sx + 0.5, py * SCALE + sy + 0.5)
                    if c is not None:
                        acc[0] += c[0]
                        acc[1] += c[1]
                        acc[2] += c[2]
                        acc[3] += 1
            n = acc[3]
            if n == 0:
                row.append((0, 0, 0, 0))
            else:
                row.append((int(acc[0] / n), int(acc[1] / n), int(acc[2] / n), int(255 * n / SCALE ** 2)))
        rows.append(row)
    png(path, rows)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
