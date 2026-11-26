#!/usr/bin/env python3
"""Update the XIAO ESP32S3's firmware over Wi-Fi (the board's OTA service).

The CMake target "ota-flash" runs this with the board's address
(-DOTA_HOST) and the application image.  The image goes to the OTA slot
that is not running; the board checks it as written (its SHA-256), makes
it the one to boot and restarts.  The new image then waits for its
confirmation: this script connects again (for up to --wait seconds) and
confirms it once the board reports that it runs the image that was sent
(the ELF SHA-256 in its application description).  A reset before the
confirmation brings the previous image back.

The protocol (shell/port/ota_service.c): a 16-byte header ("XOTA", version
1, the command, flags, then the image's size or the expected ELF SHA-256's
first 8 bytes), the image, and one line of text from the board after the
header and after the image.

  --no-reboot   write and switch, do not restart: the new image starts at
                the next reset and still needs its confirmation
  --no-confirm  restart, do not confirm: the next reset rolls back

Exit status: 0 on success, 1 when the board refused or failed the update
or another image runs after the restart, 2 for any other failure (a bad
file, no connection).
"""

import argparse
import select
import socket
import struct
import sys
import time

PORT = 3232
VERSION = 1
CMD_WRITE = 1
CMD_CONFIRM = 2
FLAG_RESTART = 0x01
CHUNK = 4096

# The image's layout (esp_app_format.h, esp_app_desc.h): the header, its
# first segment's header, then the application description.
IMAGE_MAGIC = 0xE9
HASH_APPENDED_OFFSET = 23
DESC_OFFSET = 0x20
DESC_MAGIC = 0xABCD5432
DESC_VERSION = DESC_OFFSET + 16
DESC_PROJECT = DESC_OFFSET + 48
DESC_ELF_SHA256 = DESC_OFFSET + 144     # file offset 0xb0, where the build writes it
SHA_PREFIX_BYTES = 8

# How long the board takes at most: the answer to the header (it may stop a
# USB audio stream first), a block of the image (a sector erase, tens of
# ms, and the network), the check after the image (the image read back
# twice, with its SHA-256).
HEADER_TIMEOUT_S = 15.0
SEND_TIMEOUT_S = 20.0
RESULT_TIMEOUT_S = 60.0
# After the restart: before the first attempt, between attempts.
RESTART_DELAY_S = 2.0
RETRY_DELAY_S = 1.0
CONNECT_TIMEOUT_S = 3.0


class BoardError(Exception):
    """The board answered "ERR ..." or something unexpected."""


def text(field):
    return field.split(b"\0", 1)[0].decode("ascii", "replace")


def read_image(path):
    with open(path, "rb") as f:
        image = f.read()
    if len(image) < DESC_ELF_SHA256 + 32 or image[0] != IMAGE_MAGIC:
        raise ValueError(f"{path}: not an ESP application image")
    if struct.unpack_from("<I", image, DESC_OFFSET)[0] != DESC_MAGIC:
        raise ValueError(f"{path}: no application description")
    if image[HASH_APPENDED_OFFSET] != 1:
        raise ValueError(f"{path}: no SHA-256 appended")
    return image


def header(command, flags, tail):
    return b"XOTA" + struct.pack("<BBBB", VERSION, command, flags, 0) + tail


def connect(host, port, timeout):
    sock = socket.create_connection((host, port), timeout=timeout)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return sock


def read_line(sock, timeout):
    """One line from the board, without its end; "" if it closed first."""
    sock.settimeout(timeout)
    data = b""
    while not data.endswith(b"\n"):
        chunk = sock.recv(256)
        if not chunk:
            break
        data += chunk
    return data.decode("ascii", "replace").strip()


def expect_ok(line, what):
    if line.startswith("OK"):
        return line[2:].strip()
    if not line:
        raise BoardError(f"the board closed the connection {what}")
    raise BoardError(f"the board answered {line!r} {what}")


class Progress:
    def __init__(self, total):
        self.total = total
        self.tty = sys.stderr.isatty()
        self.shown = -1

    def update(self, sent):
        step = sent * 100 // self.total if self.tty else sent * 10 // self.total * 10
        if step == self.shown or (step == 0 and sent < self.total):
            return
        self.shown = step
        line = f"  {step:3d} % ({sent} of {self.total} bytes)"
        if self.tty:
            sys.stderr.write("\r" + line)
            if sent == self.total:
                sys.stderr.write("\n")
        else:
            sys.stderr.write(line + "\n")
        sys.stderr.flush()


def send_image(sock, image):
    """Sends the image; stops at a line the board sends meanwhile ("ERR ...")."""
    progress = Progress(len(image))
    sock.settimeout(SEND_TIMEOUT_S)
    for pos in range(0, len(image), CHUNK):
        readable, _, _ = select.select([sock], [], [], 0)
        if readable:
            raise BoardError(f"the board answered {read_line(sock, SEND_TIMEOUT_S)!r} "
                             f"after {pos} bytes")
        try:
            sock.sendall(image[pos:pos + CHUNK])
        except OSError as e:
            try:
                line = read_line(sock, 2.0)
            except OSError:
                line = ""
            raise BoardError(f"the board answered {line!r} after {pos} bytes" if line
                             else f"sending failed after {pos} bytes: {e}") from e
        progress.update(min(pos + CHUNK, len(image)))


def write(host, port, image, restart):
    with connect(host, port, CONNECT_TIMEOUT_S * 3) as sock:
        sock.sendall(header(CMD_WRITE, FLAG_RESTART if restart else 0,
                            struct.pack("<II", len(image), 0)))
        slot = expect_ok(read_line(sock, HEADER_TIMEOUT_S), "to the header")
        print(f"Writing {len(image)} bytes to {slot}")
        start = time.monotonic()
        send_image(sock, image)
        print("Sent; the board checks the image")
        expect_ok(read_line(sock, RESULT_TIMEOUT_S), "after the image")
        print(f"Written and checked in {time.monotonic() - start:.1f} s; {slot} boots next")
    return slot


def confirm(host, port, sha, wait):
    """Confirms the image with this ELF SHA-256 once it runs; the board's answer."""
    deadline = time.monotonic() + wait
    time.sleep(RESTART_DELAY_S)
    last = "no connection"
    while time.monotonic() < deadline:
        try:
            with connect(host, port, CONNECT_TIMEOUT_S) as sock:
                sock.sendall(header(CMD_CONFIRM, 0, sha))
                line = read_line(sock, HEADER_TIMEOUT_S)
        except OSError as e:
            last = str(e)
            time.sleep(RETRY_DELAY_S)
            continue
        if line.startswith("ERR busy") or not line:
            last = line or "closed"
            time.sleep(RETRY_DELAY_S)
            continue
        return line
    raise ConnectionError(f"the board did not answer within {wait:.0f} s ({last})")


def main():
    parser = argparse.ArgumentParser(
        description="Update the XIAO ESP32S3's firmware through its OTA service")
    parser.add_argument("host", help="the board's IP address (the shell's \"ifconfig\")")
    parser.add_argument("image", help="the application image (xiao_esp32s3_threadx.bin)")
    parser.add_argument("--port", type=int, default=PORT, help=f"TCP port (default {PORT})")
    parser.add_argument("--no-reboot", action="store_true",
                        help="do not restart the board after the update")
    parser.add_argument("--no-confirm", action="store_true",
                        help="do not confirm the new image after the restart")
    parser.add_argument("--wait", type=float, default=60.0,
                        help="seconds to wait for the board after the restart (default 60)")
    args = parser.parse_args()

    try:
        image = read_image(args.image)
    except (OSError, ValueError) as e:
        print(f"ota: {e}", file=sys.stderr)
        return 2
    sha = image[DESC_ELF_SHA256:DESC_ELF_SHA256 + SHA_PREFIX_BYTES]
    print(f"{args.image}: {text(image[DESC_PROJECT:DESC_PROJECT + 32])} "
          f"{text(image[DESC_VERSION:DESC_VERSION + 32])}, ELF SHA-256 {sha.hex()}")

    try:
        write(args.host, args.port, image, not args.no_reboot)
        if args.no_reboot:
            print("Not restarted: the new image starts at the next reset, "
                  "pending its confirmation")
            return 0
        if args.no_confirm:
            print("Restarting; not confirmed: the next reset goes back to the previous image")
            return 0
        print("Restarting; waiting for the new image")
        line = confirm(args.host, args.port, sha, args.wait)
        if line.startswith("ERR other"):
            print(f"ota: another image runs ({line[len('ERR other '):]}): the new image "
                  "did not start and the previous one is back", file=sys.stderr)
            return 1
        print("Confirmed: " + expect_ok(line, "to the confirmation"))
        return 0
    except BoardError as e:
        print(f"\nota: {e}", file=sys.stderr)
        return 1
    except OSError as e:
        print(f"\nota: {args.host}:{args.port}: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
