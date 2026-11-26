# ThreadX on the Seeed Studio XIAO ESP32S3

Bare-metal [Eclipse ThreadX](https://github.com/eclipse-threadx/threadx) on the
XIAO ESP32S3, built on
[esp-hal-3rdparty](https://github.com/espressif/esp-hal-3rdparty) (an
unmodified mirror of ESP-IDF's components, used here without ESP-IDF's build
system or FreeRTOS; the FreeRTOS API that ESP-IDF code is written against
comes from ThreadX's FreeRTOS compatibility layer, a modified copy in
`port/threadx/freertos/`).  The board boots through ESP-IDF's own second-stage
bootloader, built from the same sources.

Status: **runs on a XIAO ESP32S3** (ESP32-S3R8, chip revision v0.2), executing
from flash at 240 MHz, with an interactive shell on the USB console and on
UART0.

| | |
|---|---|
| MCU | ESP32-S3 (Xtensa LX7, core 0 only) |
| Kernel | ThreadX 6.5.2 (`v6.5.2.202603_rel`), `common/` only |
| Xtensa layer | esp-hal-3rdparty `components/xtensa` (vectors, interrupt dispatch, context switch primitives) |
| HAL | esp-hal-3rdparty `sync/master.c` at `130cda98` (ESP-IDF master of 2026-11-15, v6.3 development) |
| Toolchain | `xtensa-esp-elf` 16.1.0_20260609 (the one ESP-IDF names at that commit), fetched on first configure |
| Console | USB Serial/JTAG on the USB-C connector and UART0 on D6/D7 (115200 8N1), together; receive by interrupt on both, UART transmit by interrupt through a 4 KiB ring, USB transmit polled |
| Shell | The shell of [rp2040-connect](https://github.com/owhinata/rp2040-connect), copied into `shell/common/` |
| Tick | 1000 Hz from CCOMPARE0 (CPU interrupt 6, level 1) |
| Watchdog | ESP-IDF's interrupt watchdog: timer group 1, fed by every tick; 300 ms without a tick ends in the fault report, 600 ms more in a system reset |
| Uptime clock | SYSTIMER counter 0 (16 MHz, from the crystal), through `esp_timer_get_time()`; counts from the start of the application |
| Timers | ESP-IDF's esp_timer: a SYSTIMER alarm interrupt and its task on the FreeRTOS layer |
| Run times | ThreadX execution profile kit on the CPU cycle counter |
| LED | GPIO21, active low, toggled by an esp_timer periodic timer |
| CPU clock | 240 MHz from the PLL (`esp_clk_init()`, `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ`) |
| Heap | ESP-IDF's heap (`heap_caps` on TLSF; `malloc()` is esp_libc's): about 220 KiB of internal RAM and the RTC FAST memory, usable from interrupt handlers |
| Log | ESP-IDF's log (`ESP_LOGx`) goes into the RAM log that `dmesg` shows |
| Radio | Espressif's Wi-Fi driver (esp_wifi with the binary Wi-Fi and PHY libraries, wpa_supplicant, mbedTLS) in station mode: `wifi scan`, `wifi connect`, `wifi status`, `wifi disconnect`, and `wifi cred` for stored networks with automatic connection |
| USB host | Espressif's USB Host Library (espressif/esp-usb) with external hubs, on the USB OTG controller (full speed) through the internal PHY of the USB-C connector; started by `usb host start`.  While it runs, the USB-C console is off |
| Audio input | A USB Audio Class microphone on the USB host: UAC 1.0 through Espressif's UAC host driver (esp-usb `usb_host_uac`), UAC 2.0 through `port/usb/audio_uac2.c`.  16-bit PCM, mixed to one channel, decimated by 3 (a 51-tap FIR) from 48 kHz or taken as it is at 16 kHz, high-passed at 30 Hz to remove the DC offset, and freed of mains hum by notches at 50 Hz (or 60 Hz) and its harmonics up to 420 Hz, for `port/usb/audio.h`; the `uac` command |
| TCP/IP | lwIP (Espressif's fork with ESP-IDF's port) on the Wi-Fi station: IPv4, DHCP.  rp2040-connect's socket layer (Zephyr's BSD socket API) runs on it, and the shell is also on Telnet (port 23).  `zperf` runs iperf3 TCP tests in both directions, and `ntp` keeps the clock of `date` on the NTP server at the default gateway |
| Flash | ESP-IDF's layout: second-stage bootloader at 0x0, partition table at 0x8000, a 28 KiB `settings` partition at 0x9000, two 3 MB application slots (`ota_0` at 0x10000, `ota_1` at 0x310000) and `otadata` at 0x610000, which records the slot to boot; the last 1.9 MB are unassigned |
| Settings | rp2040-connect's key-value store (Zephyr settings on Zephyr NVS) in the `settings` partition, written through ESP-IDF's esp_flash driver; the `kv` command |

## Build

```sh
python3 -m venv .venv && .venv/bin/pip install esptool
cmake -S . -B build -G Ninja
cmake --build build
```

The first configure downloads the compiler (~90 MB) into `tools/` and the
upstream sources into `lib/`, each at the commit pinned in `CMakeLists.txt`:
ThreadX and esp-hal-3rdparty (~370 MB), a few directories of ESP-IDF
itself that the mirror does not carry (~4 MB), the TLSF allocator of
ESP-IDF's heap, the esp32s3 part of Espressif's binary PHY and Wi-Fi
libraries (~6 MB), the parts of Espressif's mbedTLS that are built
(~7 MB), and lwIP (Espressif's fork, ~5 MB).
Neither directory is tracked by this repository.

Outputs in `build/`: `bootloader.bin`, `partition-table.bin`,
`ota_data_initial.bin` (a blank `otadata`) and `xiao_esp32s3_threadx.bin`
(the application), with their `.elf` and `.map`.

## Run

```sh
cmake --build build --target flash        # bootloader, partition table, blank otadata and application
cmake --build build --target app-flash    # the application (ota_0, 0x10000) and blank otadata
```

`flash` **replaces whatever is at the start of the flash** (the firmware that
was on the board stops booting until it is written back; the ROM download mode
itself cannot be erased, so the board is always recoverable with esptool).
`app-flash` is enough once the bootloader and the partition table are on the
board.  Neither target writes the `settings` partition.  Both write a blank
`otadata`, so that the bootloader starts the image just written to `ota_0`
whatever slot ran before (ESP-IDF's `app-flash` leaves `otadata` alone).

Both targets go through `script/flash.py`, which finds the board's USB
Serial/JTAG port by its USB ID (303a:1001) and lets esptool reset the chip into
download mode and back.

**Without USB.**  The console is also on UART0, and the board can be flashed
through it with a plain USB-serial adapter (3.3 V logic, three wires, no
reset circuit):

| XIAO | Adapter |
|---|---|
| D6 (GPIO43, TX) | RX |
| D7 (GPIO44, RX) | TX |
| GND | GND |

```sh
cmake -S . -B build -DESPPORT=/dev/ttyUSB0    # stays set; -DESPPORT= goes back to USB
cmake --build build --target app-flash
```

The tool then sends `bootsel` to the shell, waits for the ROM's download mode,
switches off the flash-boot watchdog of timer group 0 (which would reset the
chip once the next step is done), clears the bit that forced the download
mode, writes at `ESPBAUD` (460800 by default) and restarts the firmware with a
watchdog reset.  No button is needed.  Only when
no shell answers or the firmware restarts before it runs the command (no
firmware, a hung one, or one that keeps crashing) does it ask for the buttons:
hold **B** (BOOT), tap **R** (RESET), release **B**.

On the console (any terminal on the board's `/dev/ttyACM*`, or on the adapter
at 115200):

```
XIAO ESP32S3: ThreadX 6.5.2, CPU clock 240000000 Hz, 1000 ticks/s

xiao:~$ help
Available commands:
  bootsel  : Restart into the ROM download mode (for esptool)
  date     : Show the date and time (UTC)
  devmem   : Read or write memory: devmem <address> [<width> [<value>]]
  dmesg    : Show logs stored in RAM
  fault    : Show the fatal error that ended the last run
  free     : Show the use of malloc()
  help     : Prints the help message.
  id       : Show the board's unique ID (the factory MAC address)
  ifconfig : Show the network interface: ifconfig [wlan0]
             (MAC address, state, IPv4 address, default router, netmask)
  kv       : Application key-value storage
  ntp      : Network time synchronization
  ota      : Firmware update slots (OTA)
  ps       : List the tasks
  reboot   : Restart the board
  sleep    : Sleep for <ms> milliseconds (Zephyr's kernel sleep)
  uac      : USB audio input (UAC 1.0 or 2.0 microphone)
  uptime   : Show the time since the start (SYSTIMER) and the ticks
  usb      : USB host
  version  : Show the versions of the kernel, HAL and compiler
  wifi     : Wi-Fi commands
  zperf    : Zperf commands
```

**Settings.**  `kv set <key> <text>`, `kv sethex <key> <hex>`, `kv get <key>`
(prints hexadecimal) and `kv delete <key>` keep values in the `settings`
partition; they survive resets, power cycles and `app-flash`.  The store has
rp2040-connect's format (Zephyr NVS), not ESP-IDF's NVS.  It only uses the
`settings` partition, and only when that holds its own data or is erased:
with anything else in the area, or with a partition table that has no
`settings`, every `kv` command fails (the reason is in `dmesg`).  `kv wipe
confirm` erases the area; the `flash` target writes the partition table.
The stored Wi-Fi networks are in the same store: `kv` does not show them,
and `kv wipe confirm` erases them too.

While a flash sector is erased (about 40 ms) or a page is written (about
0.6 ms) the cache is off and interrupts are masked: the LED, the console and
the network pause for that long.

**Over Wi-Fi.**  `wifi cred add -s <ssid> -p <passphrase> -k 1` stores a
WPA2-PSK network in the settings partition (`-k 0` none, `-k 3`, `4`, `5`
WPA3-SAE; `wifi cred list` shows the stored networks with their passphrases,
`wifi cred delete <ssid>` removes one).  With a network stored the board
connects by itself about 4 s after it starts and tries again every 32 s
while that fails.  `wifi connect -s <ssid> -p <passphrase> -k 1` connects
once without storing anything; `wifi disconnect` also stops the automatic
connection until `wifi connect` or `wifi cred auto_connect` succeeds.
`ifconfig` shows the address DHCP gave, and `telnet <address>` from a PC on
that network opens the same shell with the same prompt.  One Telnet client
at a time: a second one is closed at once, and a client that vanishes
without closing is dropped after at most 90 s by TCP keepalive.  When the
access point goes away the address is kept for up to 120 s while the
station tries every 3 s to return, so a session gets through a loss of up
to a minute; after that the address is dropped and the stored networks are
tried every 32 s.  Telnet has no authentication.  The UART and USB console
keeps working beside it, and each console gets the output of its own
commands.

`usb host start`, given on UART0 or Telnet, turns the USB-C connector into
a full-speed USB host port: the console on that connector stops, and
`usb host stop` gives it back.  `usb status` shows the port, `usb list` the
devices, and `usb desc <address>` a device's descriptors, the
class-specific ones in hexadecimal.  The connector does not supply power:
5 V on the board's 5V pin feeds the board and the device.

While the host runs, the first USB Audio Class microphone that connects
(UAC 1.0 or 2.0) is opened.  `uac info` shows its formats, `uac start [<rate> [<channels>]]`
starts the stream (by default at 48000 Hz if the microphone offers it,
else 16000, else its highest rate up to 48000, and with one channel if it
has one; `uac info` shows the choice; a 16000 Hz stream skips the
decimation), `uac stop`
stops it, `uac level [<seconds>]` prints the peak and RMS level in dBFS
four times a second, and `uac stats` the counters, the measured sample
rate, the DC offset of the input and the number of samples at full scale.
`uac dump [<samples>]` prints consecutive 16 kHz samples as numbers.
`uac hum [50|60|off]` sets the mains frequency of the hum filter (50 by
default).
`uac volume [<0..100>]` shows the microphone's volume (percent of its
range and dB) and mute, or sets the volume; `uac mute on|off` sets the
mute.  Both work while the stream runs, and what was set last is set again
at every stream start until the next reboot.

The orange LED toggles every 0.5 s.  The banner is printed at boot; a terminal
opened later gets a prompt on Enter.  Output goes to both transports and input
is taken from either.  UART output is queued and leaves at 115200 in the
background; only an output longer than the 4 KiB queue makes its writer wait.
Outside threads (start-up, interrupt handlers, the fault report) the UART is
written directly, after what is still queued.

`bootsel` keeps its name from rp2040-connect; on this board it restarts into
the ROM download mode, where esptool can connect.  A reset from esptool or the
RESET button returns to the firmware.  `id` prints the factory MAC address
from eFuse.

`ota status` shows the two application slots: which one this image runs
from, which one the bootloader starts next, and for each its otadata entry
(sequence number and state) and its image's version and build time.

`dmesg` shows a log kept in RAM that survives a reset, so it also holds the
lines of earlier boots.

`ps` lists each thread with its free stack and its share of the CPU since the
previous `ps`, then the time spent idle and in interrupt handlers, and
`cpu_active` (everything but idle):

```
NAME                 STATE PRIO STACK FREE    CPU
txfr_idle_task       S       31       1116    0.0%
esp_timer            S        9       3596    0.0%
console              X       30       5312    0.1%
(idle)               -        -          -   99.8%
(interrupts)         -        -       3872    0.1%
STATE: X running, R ready, B blocked, S suspended
cpu_active: 0.2% in the 2.915 s since the previous "ps"
```

The `(interrupts)` figure is the time spent inside interrupt handlers.  Getting
into and out of an interrupt (saving and restoring the context) is not part of
it; that time is counted with the thread, or the idle time, that was
interrupted or resumed.

`date` counts from 1970-01-01 at boot until the first NTP reply, then shows
UTC.  While connected, the board asks the NTP server on the default gateway
(port 123) every 2 s; `ntp status` shows the exchanges and the estimate.
The clock is only ever stepped forward.

`zperf tcp upload <pc> 5201 10 1K` runs an iperf3 TCP test against
`iperf3 -s` on the PC.  `zperf tcp download 5201` starts an iperf3 server
for `iperf3 -c <board> -t 10` (one TCP stream; no `-R`, `-u` or `-P`), until
`zperf tcp download stop`.

After a fatal error the board restarts by itself.  `fault` then shows the
record of the error that ended the last run (the reason, EXCCAUSE, the
registers, the task, the time); the banner shows it once and `dmesg` has its
lines.  The record is gone after the next restart.  No tick for 300 ms
(interrupts masked that long, or an interrupt handler that does not return)
is such an error too: the interrupt watchdog's first stage, a level-4
interrupt, reports it with the registers of the code that was stuck (`Fatal
error: interrupt watchdog`).  If even that cannot run, its second stage
resets the chip 0.6 s later without a record, and the next start says so
from the chip's reset reason (`Fatal error: interrupt watchdog reset`).  A failed
`assert()`, `ESP_ERROR_CHECK()` or `abort()` in the Espressif sources (their
assertions are on, as in ESP-IDF) is recorded with the file and line.  Its layout is this
board's (`shell/port/fault_record.h`), not rp2040-connect's Cortex-M one.

## How it boots

The boot chain is ESP-IDF's, without ESP-IDF's application start-up code.

1. The mask ROM loads the second-stage bootloader from flash offset 0.
2. The bootloader (`bootloader/CMakeLists.txt`; 75 unmodified ESP-IDF sources)
   sets up the clocks (PLL, CPU at 80 MHz), the flash and the cache, reads the
   partition table (`partitions.csv`: settings, two application slots and
   otadata), picks the slot that otadata selects (`ota_0` when otadata is
   blank, which it then records), verifies the application image, loads its RAM segments, maps
   its flash segments and jumps to `_start`.  Its log goes to UART0 and to the
   USB console.
3. `_start` (`port/startup.c`) does what ESP-IDF's application start-up does
   at this point, in the same order: vectors and CPU state, `.bss`, the cache
   geometry and the boundary between code and data in the shared MMU table,
   `esp_rtc_init()` (the internal regulators trimmed from eFuse),
   `esp_mmu_map_init()` (ESP-IDF's MMU driver, through which the partition
   and OTA code map flash),
   `esp_clk_init()` (calibration of the RTC slow clock, CPU to 240 MHz), the
   peripheral clock gates as ESP-IDF leaves them (`esp_perip_clk_init()`),
   the cache error interrupt, the
   RTC watchdog the bootloader armed for the application off, the console,
   ESP-IDF's core start-up hooks (the heap, esp_timer's clock, the flash
   driver, which also checks that the partition table has `settings` and
   opens only that partition and otadata for writing), the
   constructors, then `main()`.  The later hooks run where ESP-IDF runs
   them: in `tx_application_define()` (esp_timer's task) and in the first
   task, "main", which then starts the Wi-Fi driver in station mode
   (ESP-IDF's event loop, `esp_wifi_init()`, `esp_wifi_start()`, which
   calibrates the PHY), then lwIP with the station's network interface
   the Telnet shell, the time synchronization and the iperf3 server's
   thread, and ends.  ESP-IDF's own
   start-up lines (`app_init`, `heap_init`) appear on the console before
   the banner.

The memory layout and the lists of code that runs from RAM are in
`ldscript/xiao_esp32s3.ld`, with the reasons.  There are two lists: the code
that has to run from RAM (before flash is mapped, while the cache is off,
the fault report), and the code that is there for speed, 58 KiB: the path
of a packet through the Wi-Fi libraries, lwIP and the heap, as ESP-IDF's
IRAM options place it.  The rest of that path (the kernel, esp_timer, the
sockets) runs from flash through a 32 KiB instruction cache, twice
ESP-IDF's default.  With all of the path in flash and a 16 KiB cache, TCP
gave 2.4 Mbit/s; now it gives about 10 Mbit/s (`zperf`, both directions)
with three quarters of the CPU idle.  Points that are easy to get wrong:

- The instruction cache uses SRAM0, all 32 KiB of it, as its own memory.
  Nothing may be linked there.
- The bootloader runs from the top of internal SRAM while it loads the
  application and refuses an image whose loaded segments reach into it, so
  everything loaded ends below 0x3FCDB700.  The memory above is free again once
  the application runs and is part of the heap.
- The bootloader maps the flash but leaves the cache geometry and
  `Cache_Set_IDROM_MMU_Size` to the application.  Without them the first
  instruction fetched from flash faults.
- The RAM log (`.dram0.noinit`) has to stay clear of what the bootloader and
  the ROM's download mode write; the linker script asserts it.

What ESP-IDF's application start-up does and this firmware does not:

- PSRAM (8 MB on this board) is not initialized.
- Only core 0 runs.  ThreadX (non-SMP) is single-core; core 1 is left as the
  ROM leaves it.

## Layout

```
bootloader/     build of ESP-IDF's second-stage bootloader
cmake/          toolchain file (downloads the compiler), upstream fetch
partitions.csv  partition table
script/         flash tool
ldscript/       linker script, and the check run after linking that the code
                that has to run from RAM references neither flash nor the
                code that is in RAM for speed
shell/common/   shell engine and commands, copied from rp2040-connect
shell/port/     this board's part of the shell
lib/            upstream sources, fetched by CMake; read-only, not tracked
port/           board and start-up code, and port/esp_compat: what the
                Espressif sources expect from parts of ESP-IDF that are not built
port/usb/       USB host: start, stop, the list of devices; the USB audio
                input (audio.c, audio_uac2.c)
port/threadx/   ThreadX architecture port, and its FreeRTOS compatibility layer
                (freertos/)
src/            application
```

The rule is to take code from Espressif wherever it can be compiled as it
is, and to write only what connects it to ThreadX and to this board.
`CMakeLists.txt` and `bootloader/CMakeLists.txt` list every upstream file
that is built, with the pinned commits; each file under `port/` says in its
header what it is for.  Files copied from elsewhere, and what was changed in
them, are recorded in [`port/threadx/ORIGIN.md`](port/threadx/ORIGIN.md)
(the ThreadX port and the FreeRTOS layer) and [`shell/ORIGIN.md`](shell/ORIGIN.md) (the shell);
licences are in [`LICENSE`](LICENSE).

The C library is usable from threads and from `tx_application_define()`, not
from interrupt handlers (output written from one bypasses the shell's console
layer and goes to the port directly).  The same goes for the FPU and the vector extension:
using a coprocessor in an interrupt handler ends in a panic.
