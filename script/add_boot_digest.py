#!/usr/bin/env python3
"""Add the SHA-256 digest the ESP32-S3 ROM bootloader looks for.

"esptool elf2image --ram-only-header" writes an image whose header counts
only the RAM segments:

    header | RAM segments | checksum | pad segment | flash segment | ...

The ROM loads the RAM segments, then compares the 32 bytes that follow the
checksum with the SHA-256 of everything before them, whatever the header's
"hash appended" flag says.  esptool puts the header of the first pad segment
there, so every boot logs "SHA-256 comparison failed ... Attempting to boot
anyway".

This script puts the digest where the ROM reads it and moves the pad segment
header behind it, shrinking that pad by the same 32 bytes, so nothing after it
moves:

    header | RAM segments | checksum | SHA-256 | pad segment (-32) | flash segment | ...

port/flash_map.c skips the digest when it walks the image.
"""
import hashlib
import struct
import sys

HEADER_SIZE = 24
SEG_HEADER_SIZE = 8
DIGEST_SIZE = 32


def main(path):
    with open(path, "rb") as f:
        image = bytearray(f.read())

    if image[0] != 0xE9:
        sys.exit(f"{path}: not an ESP image")

    offset = HEADER_SIZE
    for _ in range(image[1]):
        _, length = struct.unpack_from("<II", image, offset)
        offset += SEG_HEADER_SIZE + length
    offset = (offset + 16) & ~15            # checksum is the last byte of a 16-byte block

    addr, length = struct.unpack_from("<II", image, offset)
    if addr != 0 or length < DIGEST_SIZE:
        sys.exit(f"{path}: no pad segment after the checksum "
                 f"(load address {addr:#x}, length {length}); already processed?")
    pad = image[offset + SEG_HEADER_SIZE:offset + SEG_HEADER_SIZE + length]
    if any(pad):
        sys.exit(f"{path}: pad segment is not empty")

    image[offset:offset + DIGEST_SIZE] = hashlib.sha256(image[:offset]).digest()
    struct.pack_into("<II", image, offset + DIGEST_SIZE, 0, length - DIGEST_SIZE)

    with open(path, "wb") as f:
        f.write(image)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(f"usage: {sys.argv[0]} IMAGE.bin")
    main(sys.argv[1])
