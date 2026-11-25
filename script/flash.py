#!/usr/bin/env python3
"""Write images to the XIAO ESP32S3 with esptool, over USB or over UART0.

The CMake targets "flash" and "app-flash" run this with the esptool
"write-flash" arguments (offsets, files and flash parameters) after "--";
CMake derives them from sdkconfig.h and partitions.csv, and this script
passes them on unchanged.

The port:
  --port given        that port; a USB-serial adapter unless its USB ID is
                      the XIAO's USB Serial/JTAG (303a:1001)
  no --port           the XIAO's USB Serial/JTAG, found by its USB ID; any
                      other serial device has to be named

USB Serial/JTAG: esptool as it is, which resets the chip into the ROM's
download mode through the controller and resets it into the firmware after.

UART0 through a USB-serial adapter with only TX, RX and GND wired (no reset
lines): the firmware's shell is asked to restart into the download mode
("bootsel"), and once the ROM says it is waiting esptool writes at a higher
baud rate.  bootsel sets RTC_CNTL_FORCE_DOWNLOAD_BOOT, which survives every
reset short of a power cycle or the RESET button, so it is cleared first,
while the ROM still waits; the RTC watchdog reset of esptool's
"--after watchdog-reset" then boots the firmware.  With the bit cleared the
chip counts as booting from flash again, which lets timer group 0's
flash-boot watchdog run (its enable bit is set by every reset and the ROM's
download mode does not clear it); it would reset the chip within a second,
so it is switched off before the bit is cleared.  If no shell answers, or the
firmware restarts before it runs the command (no firmware, a hung one, or one
that keeps crashing), the user has to put the chip into download mode by
hand (hold B, tap R, release B); the script waits a bounded time for that.

Exit status: 0 on success, esptool's status if esptool fails, 2 otherwise.
"""

import argparse
import os
import subprocess
import sys
import time

import serial
import serial.tools.list_ports
from esptool.targets.esp32s3 import ESP32S3ROM

CHIP = "esp32s3"
XIAO_USB_ID = (0x303A, 0x1001)  # Espressif USB JTAG/serial debug unit

# What the ROM prints on UART0 when it enters the download mode.
ROM_DOWNLOAD_BANNER = b"waiting for download"

# Timer group 0's watchdog (soc/timer_group_reg.h): the configuration
# register with its flash-boot enable bit, and the write-protect register
# with the key that opens it.
TIMG0_WDTCONFIG0_REG = 0x6001F048
TIMG_WDT_FLASHBOOT_MOD_EN = 1 << 14
TIMG0_WDTWPROTECT_REG = 0x6001F064
TIMG_WDT_WKEY = 0x50D83AA1


class FlashError(Exception):
    """A failure the user is told about, with exit status 2."""


class EsptoolFailed(Exception):
    """esptool returned a non-zero status."""

    def __init__(self, status):
        super().__init__(status)
        self.status = status


def log(msg):
    print(f"flash.py: {msg}", flush=True)


def is_xiao(port_info):
    return (port_info.vid, port_info.pid) == XIAO_USB_ID


def select_port(requested, ports):
    """Returns (device, uses_usb_serial_jtag) for the port to write through.

    ports: what serial.tools.list_ports.comports() returns (objects with
    .device, .vid, .pid).
    """
    if requested:
        if not os.path.exists(requested):
            raise FlashError(f"{requested} does not exist")
        real = os.path.realpath(requested)
        for p in ports:
            if os.path.realpath(p.device) == real:
                return requested, is_xiao(p)
        return requested, False

    xiao = [p.device for p in ports if is_xiao(p)]
    if len(xiao) == 1:
        return xiao[0], True
    if not xiao:
        raise FlashError(
            "no XIAO ESP32S3 USB Serial/JTAG (USB ID 303a:1001) is attached.  "
            "To write through a USB-serial adapter on D6/D7, name its port: "
            "cmake -B build -DESPPORT=/dev/ttyUSBn")
    raise FlashError(
        f"more than one device with USB ID 303a:1001 ({', '.join(sorted(xiao))}); "
        "name the board's port: cmake -B build -DESPPORT=/dev/ttyACMn")


def open_console(device, baud):
    """Opens the UART with DTR asserted and RTS deasserted.  Some adapters
    pass no data while DTR is deasserted.  The
    two lines are not wired to the board; with an adapter's reset circuit
    this combination would hold IO0 low and leave EN alone, so it cannot
    reset the chip."""
    port = serial.Serial()
    port.port = device
    port.baudrate = baud
    port.timeout = 0.05
    port.write_timeout = 1.0
    port.dtr = True
    port.rts = False
    try:
        port.open()
    except serial.SerialException as e:
        raise FlashError(f"cannot open {device}: {e}") from e
    return port


def wait_for(port, pattern, timeout):
    """Reads until pattern has arrived (True) or timeout seconds passed (False)."""
    seen = b""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        seen += port.read(256)
        if pattern in seen:
            return True
        seen = seen[-len(pattern):] if len(seen) > 4096 else seen
    return False


def enter_download_mode(port, prompt, shell_timeout, rom_timeout, button_timeout):
    """Brings the chip on the UART into the ROM's download mode."""
    port.reset_input_buffer()
    port.write(b"\r")
    if wait_for(port, prompt, shell_timeout):
        log("the shell answered; sending bootsel")
        port.write(b"bootsel\r")
        if wait_for(port, ROM_DOWNLOAD_BANNER, rom_timeout):
            log("the ROM waits for download")
            return
        # A firmware that restarts over and over prints a prompt and is
        # gone before it runs the command.
        log(f"bootsel was sent, but the ROM did not report the download mode "
            f"within {rom_timeout:g} s")
    else:
        log(f"no shell answered on {port.port}")

    log(f"Hold B (BOOT), tap R (RESET), release B; waiting {button_timeout:g} s "
        f"for the ROM")
    if not wait_for(port, ROM_DOWNLOAD_BANNER, button_timeout):
        raise FlashError(
            "the ROM did not report the download mode.  Check the wiring "
            "(adapter TX to D7, RX to D6, GND to GND, 3.3 V levels) and the port")
    log("the ROM waits for download")


def run_esptool(cmd):
    log(" ".join(cmd))
    try:
        status = subprocess.call(cmd)
    except OSError as e:
        raise FlashError(f"cannot run {cmd[0]}: {e}") from e
    if status != 0:
        raise EsptoolFailed(status)


def flash_usb(args, device):
    run_esptool([args.esptool, "--chip", CHIP, "--port", device,
                 "write-flash", *args.write_flash])


def flash_uart(args, device):
    port = open_console(device, args.console_baud)
    try:
        enter_download_mode(port, args.prompt.encode(), args.shell_timeout,
                            args.rom_timeout, args.button_timeout)
    except serial.SerialException as e:
        raise FlashError(f"{device}: {e}") from e
    finally:
        port.close()

    common = [args.esptool, "--chip", CHIP, "--port", device, "--before", "no-reset"]
    # Talking to the ROM itself: the flash-boot watchdog off, then the forced
    # download boot cleared.
    rom = [*common, "--after", "no-reset", "--no-stub", "write-mem"]
    run_esptool([*rom, hex(TIMG0_WDTWPROTECT_REG), hex(TIMG_WDT_WKEY)])
    run_esptool([*rom, hex(TIMG0_WDTCONFIG0_REG), "0", hex(TIMG_WDT_FLASHBOOT_MOD_EN)])
    run_esptool([*rom, hex(ESP32S3ROM.RTC_CNTL_OPTION1_REG), "0",
                 hex(ESP32S3ROM.RTC_CNTL_FORCE_DOWNLOAD_BOOT_MASK)])
    run_esptool([*common, "--baud", str(args.flash_baud), "--after", "watchdog-reset",
                 "write-flash", *args.write_flash])


def parse_args(argv):
    parser = argparse.ArgumentParser(
        description="Write images to the XIAO ESP32S3 over USB Serial/JTAG or UART0.")
    parser.add_argument("--port", default="",
                        help="serial port (default: the XIAO's USB Serial/JTAG, by USB ID)")
    parser.add_argument("--esptool", required=True, help="esptool executable")
    parser.add_argument("--prompt", required=True, help="the shell's prompt")
    parser.add_argument("--console-baud", type=int, required=True,
                        help="baud rate of the UART console and the ROM")
    parser.add_argument("--flash-baud", type=int, required=True,
                        help="baud rate of the transfer over a UART")
    parser.add_argument("--shell-timeout", type=float, default=2.0,
                        help="seconds to wait for the prompt")
    parser.add_argument("--rom-timeout", type=float, default=5.0,
                        help="seconds from bootsel to the ROM's download banner")
    parser.add_argument("--button-timeout", type=float, default=30.0,
                        help="seconds to wait for the buttons when no shell answers")
    parser.add_argument("write_flash", nargs=argparse.REMAINDER,
                        help="-- followed by the esptool write-flash arguments")
    args = parser.parse_args(argv)
    if args.write_flash[:1] == ["--"]:
        args.write_flash = args.write_flash[1:]
    if not args.write_flash:
        parser.error("no write-flash arguments after --")
    return args


def main():
    args = parse_args(sys.argv[1:])
    try:
        device, usb = select_port(args.port, serial.tools.list_ports.comports())
        if usb:
            log(f"{device}: USB Serial/JTAG")
            flash_usb(args, device)
        else:
            log(f"{device}: UART0 through a USB-serial adapter")
            flash_uart(args, device)
    except FlashError as e:
        log(f"error: {e}")
        return 2
    except EsptoolFailed as e:
        log(f"esptool failed (exit status {e.status})")
        return e.status
    return 0


if __name__ == "__main__":
    sys.exit(main())
