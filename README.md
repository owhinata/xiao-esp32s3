# ThreadX on the Seeed Studio XIAO ESP32S3

Bare-metal [Eclipse ThreadX](https://github.com/eclipse-threadx/threadx) on the
XIAO ESP32S3, built on
[esp-hal-3rdparty](https://github.com/espressif/esp-hal-3rdparty) (an
unmodified mirror of ESP-IDF's components, used here without ESP-IDF's build
system or FreeRTOS).  The board boots through ESP-IDF's own second-stage
bootloader, built from the same sources.

Status: **runs on a XIAO ESP32S3** (ESP32-S3R8, chip revision v0.2), executing
from flash at 240 MHz, with an interactive shell on the USB console.  See
[Bring-up checklist](#bring-up-checklist) for what has been observed.

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
| Uptime clock | SYSTIMER counter 0 (16 MHz, from the crystal) |
| Run times | ThreadX execution profile kit on the CPU cycle counter |
| LED | GPIO21, active low |
| CPU clock | 240 MHz from the PLL (`BOARD_CPU_FREQ_MHZ`) |
| Flash | ESP-IDF's layout: second-stage bootloader at 0x0, partition table at 0x8000, the firmware as the factory application at 0x10000 |

## Build

```sh
python3 -m venv .venv && .venv/bin/pip install esptool
cmake -S . -B build -G Ninja
cmake --build build
```

The first configure downloads the compiler (~90 MB) into `tools/` and the
upstream sources into `lib/`, each at the commit pinned in `CMakeLists.txt`:
ThreadX and esp-hal-3rdparty (~370 MB), and three directories of ESP-IDF
itself that the bootloader needs and the mirror does not carry (~4 MB).
Neither directory is tracked by this repository.

Outputs in `build/`: `bootloader.bin`, `partition-table.bin` and
`xiao_esp32s3_threadx.bin` (the application), with their `.elf` and `.map`.

## Run

```sh
cmake --build build --target flash        # bootloader, partition table and application
cmake --build build --target app-flash    # the application only (0x10000)
```

`flash` **replaces whatever is at the start of the flash** (the firmware that
was on the board stops booting until it is written back; the ROM download mode
itself cannot be erased, so the board is always recoverable with esptool).
`app-flash` is enough once the bootloader and the partition table are on the
board.

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
no shell answers (no firmware, or a hung one) does it ask for the buttons:
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
  ps       : List the tasks
  reboot   : Restart the board
  sleep    : Sleep for <ms> milliseconds (Zephyr's kernel sleep)
  uptime   : Show the time since the start (SYSTIMER) and the ticks
  version  : Show the versions of the kernel, HAL and compiler
```

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

`dmesg` shows a log kept in RAM that survives a reset, so it also holds the
lines of earlier boots.

`ps` lists each thread with its free stack and its share of the CPU since the
previous `ps`, then the time spent idle and in interrupt handlers, and
`cpu_active` (everything but idle):

```
NAME                 STATE PRIO STACK FREE    CPU
blink                B       10       3680    0.0%
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

`date` counts from 1970-01-01 at boot: nothing sets the clock yet.
`sys_clock_settime()` is there for a future time source.

After a fatal error the board restarts by itself.  `fault` then shows the
record of the error that ended the last run (the reason, EXCCAUSE, the
registers, the task, the time); the banner shows it once and `dmesg` has its
lines.  The record is gone after the next restart.  No tick for 300 ms
(interrupts masked that long, or an interrupt handler that does not return)
is such an error too: the interrupt watchdog's first stage, a level-4
interrupt, reports it with the registers of the code that was stuck (`Fatal
error: interrupt watchdog`).  If even that cannot run, its second stage
resets the chip 0.6 s later without a record, and the next start says so
from the chip's reset reason (`Fatal error: interrupt watchdog reset`).  Its layout is this
board's (`shell/port/fault_record.h`), not rp2040-connect's Cortex-M one.

## How it boots

The boot chain is ESP-IDF's, without ESP-IDF's application start-up code.

1. The mask ROM loads the second-stage bootloader from flash offset 0.
2. The bootloader (`bootloader/CMakeLists.txt`; 75 unmodified ESP-IDF sources)
   sets up the clocks (PLL, CPU at 80 MHz), the flash and the cache, reads the
   partition table (`partitions.csv`: nvs, phy_init, a 7 MB factory
   application), verifies the application image, loads its RAM segments, maps
   its flash segments and jumps to `_start`.  Its log goes to UART0 and to the
   USB console.
3. `_start` (`port/startup.c`) does what ESP-IDF's application start-up does
   at this point, in the same order: vectors and CPU state, `.bss`, the cache
   geometry and the boundary between code and data in the shared MMU table,
   CPU to 240 MHz, the RTC watchdog the bootloader armed for the application
   off, then the console and `main()`.

The memory layout and the list of code that has to run from RAM are in
`ldscript/xiao_esp32s3.ld`, with the reasons.  Points that are easy to get
wrong:

- The instruction cache uses the first 16 KiB of SRAM0 as its own memory.
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

- `rtc_init()`, which trims the internal voltage regulators from eFuse
  calibration values.  Needed before the radio is used.
- PSRAM (8 MB on this board) is not initialized.
- Only core 0 runs.  ThreadX (non-SMP) is single-core; core 1 is left as the
  ROM leaves it.

## Layout

```
bootloader/     build of ESP-IDF's second-stage bootloader
cmake/          toolchain file (downloads the compiler), upstream fetch
partitions.csv  partition table
script/         flash tool (and its tests, which need no board)
ldscript/       linker script
shell/common/   shell engine and commands, copied from rp2040-connect
shell/port/     this board's part of the shell
lib/            upstream sources, fetched by CMake; read-only, not tracked
port/           board and start-up code
port/threadx/   ThreadX architecture port
src/            application
```

## What comes from where

The rule is to take code from esp-hal-3rdparty wherever it can be compiled
as it is, and to write only what connects it to ThreadX and to this board.

**esp-hal-3rdparty, unmodified**

| Source | Role |
|---|---|
| `xtensa/xtensa_vectors.S` | Exception/interrupt vectors, window handlers, interrupt dispatch, coprocessor exception |
| `xtensa/xtensa_context.S` | `_xt_context_save/restore`, coprocessor save/restore/release |
| `xtensa/xtensa_intr.c`, `xtensa_intr_asm.S` | Handler tables, `xt_set_interrupt_handler()`, `xt_ints_on/off()` |
| `xtensa/esp32s3/libxt_hal.a` | Xtensa HAL (window spill, extra-state save) |
| `esp_system/port/arch/xtensa/panic_handler_asm.S` | `_xt_panic` |
| `esp_hal_wdt/wdt_hal_iram.c` | Watchdog control (interrupt watchdog, RTC watchdog) |
| `esp_system/port/soc/esp32s3/highint_hdl.S` | `xt_highint4`: the interrupt watchdog's level-4 interrupt into `panicHandler` |
| `bootloader/subproject/main/bootloader_start.c`, `bootloader_support/`, and what they need of `efuse`, `esp_rom`, `esp_hw_support`, `log`, `hal` | The second-stage bootloader, with ESP-IDF's own linker scripts |
| `esp_app_format/esp_app_desc.c` | The application descriptor the bootloader expects at the start of the image |
| `partition_table/gen_esp32part.py`, `check_sizes.py` | Partition table generation and size checks |
| `esp_hal_systimer/systimer_hal.c`, `esp_hw_support/port/esp32s3/systimer.c` | Microsecond uptime clock |
| `esp_hw_support/port/esp32s3/rtc_clk.c`, `esp_hw_support/regi2c_ctrl.c`, `esp_hal_regi2c/esp32s3/rom.regi2c.ld` | CPU clock |
| `esp_rom/patches/esp_rom_cache_esp32s2_esp32s3.c` | Work-arounds for ROM cache bugs |
| `esp_rom/patches/esp_rom_sys.c` + ROM | `esp_rom_printf`, console channel, ticks-per-us |
| `esp_hal_uart/uart_hal.c`, `soc/esp32s3/gpio_periph.c` | UART0 and its pins |
| LL headers of `esp_hal_gpio`, `esp_hal_usb`, `esp_hal_uart`, `hal` | GPIO, USB Serial/JTAG, UART, eFuse |
| `soc/`, `esp_rom/` linker scripts | Peripheral and ROM symbol addresses |

**ESP-IDF, unmodified**

The mirror lacks three directories the bootloader cannot be built without:
`esp_image_verify` (the image loader), `esp_bootloader_format` and
`esp_mspi/include`.  They are fetched from ESP-IDF itself, at the commit the
mirror was generated from, into `lib/esp-idf`.

Espressif's Xtensa layer is the RTOS-neutral half of what ESP-IDF's FreeRTOS
port is built on.  It calls the RTOS through the names in `xtensa_rtos.h`,
which `port/threadx/inc/xtensa_rtos.h` maps to ThreadX.

**ThreadX**

`lib/threadx/common` is the kernel, and
`utility/execution_profile_kit/tx_execution_profile.c` accounts the run time of
threads, interrupt handlers and idle; both are built unmodified.  The kit is
told about interrupt entry through a hook Espressif's dispatcher already has
(`CONFIG_PM_TRACE`), which `port/threadx/inc/xtensa_rtos.h` maps to
`_tx_execution_isr_enter`.  The upstream Xtensa port
(`lib/threadx/ports/xtensa/xcc`) is **not built**: it brings its own copy of
the Xtensa layer, written for the Cadence toolchain.  Its ThreadX-specific
files were copied to `port/threadx/` and adapted to Espressif's layer.  The
"Origin" column says which files those are and which are new here:

| File | Origin | State |
|---|---|---|
| `src/tx_thread_context_save.S`, `tx_thread_context_restore.S`, `tx_thread_schedule.S`, `tx_thread_stack_build.S`, `tx_thread_system_return.S` | upstream | Verbatim copies |
| `inc/tx_api_asm.h` | upstream | Verbatim copy |
| `src/tx_timer_interrupt.S` | upstream | One symbol renamed (`_xt_tick_divisor`) |
| `inc/tx_port.h` | upstream | Interrupt lockout and `_xt_coproc_release()` adapted; see the note at the top of the file |
| `src/tx_initialize_low_level.c`, `tx_thread_interrupt_control.c` | upstream | Rewritten for the layer's API and GCC |
| `src/tx_clib_newlib.c` | new | Replaces upstream `tx_clib_lock.c`, which does not build against Espressif's newlib.  Per-thread `_reent`, `__retarget_lock_*` on `TX_MUTEX`, `_sbrk_r`. |
| `src/tx_port_asm.S` | new | Coprocessor save-area lookup (taken from upstream `xtensa_coproc_handler.S`), exception hook, interrupt stack |
| `src/tx_port_offset_check.c` | new | Build-time check of the TCB offsets the assembly uses.  The upstream Xtensa port has no such check; the name follows the one upstream's Cortex-R ports use. |
| `inc/tx_user.h` | new | Kernel options: 1 kHz tick, timers processed in the tick interrupt, and the per-thread pointer the shell's kernel layer needs |
| `inc/xtensa_rtos.h` | new | Maps the layer's RTOS callouts to ThreadX; the tick goes through the board's `board_tick_interrupt()`, which feeds the interrupt watchdog (upstream has a file of the same name for its own copy of the layer) |
| `inc/freertos/xtensa_rtos.h` | new | Include-path shim for `panic_handler_asm.S` |

**rp2040-connect**

`shell/common/` holds the 62 files of rp2040-connect's `common/` directory
(commit `72f3485`) that this firmware uses, at the same relative paths: the
shell engine, the generic commands, and the Zephyr-compatibility kernel layer
they are written against.  Most are byte-identical copies;
`shell/ORIGIN.md` lists them, the changes, and their licences (MIT,
Apache-2.0 for the parts derived from Zephyr, BSD-3-Clause for getopt and
fnmatch).  `shell/port/` is this board's side of that code: the console
transport, the kernel port header, the log back end, the realtime clock, the
fault record, and the commands only the board can answer (`version`, `free`,
`ps`).

**This project** (`port/`)

| File | Role |
|---|---|
| `startup.c` | Application start-up after the bootloader |
| `esp_compat/` | What the Espressif sources expect from parts of ESP-IDF that are not built or not in the mirror: a `freertos/FreeRTOS.h` that provides critical sections only, and small stand-in headers for `esp_sleep` and `esp_mspi` |
| `board.c` | LED, clock readback, uptime, console on USB Serial/JTAG and UART0 (receive interrupts, UART transmit interrupt and queue), device ID, restart, reset reason |
| `watchdog.c` | The interrupt watchdog (started by the first tick, fed by every tick), and the watchdogs of the fault path and the restart |
| `syscalls.c` | newlib fd 0-2 on the console; once the shell runs, stdout and stderr go through the shell's console layer |
| `panic.c` | Fatal errors (`xt_unhandled_exception`, `panicHandler` including the interrupt watchdog, ThreadX's stack check, `_exit`): register dump, record kept across the restart in `.noinit`, restart |
| `include/sdkconfig.h` | The `CONFIG_` symbols the compiled sources need, shared by the bootloader and the application |

The C library is usable from threads and from `tx_application_define()`, not
from interrupt handlers (output written from one bypasses the shell's console
layer and goes to the port directly).  The same goes for the FPU and the vector extension:
using a coprocessor in an interrupt handler ends in a panic.

## Bring-up checklist

Everything below has been observed on the board with the current image over
USB.  The UART0 console and flashing through it have not been tried on a board
yet.  In the order a failure would show up:

1. After `flash`, the ROM log is followed by the bootloader's
   (`boot: ESP-IDF ... 2nd stage bootloader`, the partition table, the image
   segments, `Loaded app from partition at offset 0x10000`) and then the
   banner.  Nothing after the ROM's `entry` line: the bootloader.  Nothing
   after `Loaded app`: `_start`.  A reset about 9 s after boot: the RTC
   watchdog was not turned off.
2. The banner shows the configured clock (240 MHz).
3. The prompt appears and `help` lists the commands.  Banner but no prompt:
   the first context switch, the tick interrupt, or the receive interrupt.
4. `uptime` twice: seconds and ticks advance together (1 tick = 1 ms).
5. `ps` lists `blink`, `console`, `(idle)` and `(interrupts)`; the CPU column
   adds up to about 100% and rises for `console` when the shell is kept busy.
   `date` advances with `uptime`.
6. `id` prints the MAC address esptool reports.
7. `devmem 0` ends in a register dump and a restart by itself; the banner
   then shows the record, `fault` shows it again, and after a `reboot`
   `fault` says there was none.  Writing 0 to the first word of a thread's
   stack (`devmem <address of blink_stack> 32 0`) ends the same way with
   `Stack overflow in thread "blink"`.  `reboot` comes back
   to the banner, `bootsel` lets esptool connect, `dmesg` still shows the
   lines of earlier boots after a `reboot`, and `app-flash` alone updates the
   firmware.
8. The LED blinks at 1 Hz.
9. The interrupt watchdog, with temporary code: a thread that keeps
   interrupts masked, or an interrupt handler that does not return, ends in
   `*** Interrupt watchdog: no tick for 300 ms` and a restart, and `fault`
   names the thread and the PC.  With every level masked
   (`tx_interrupt_control(15)`) the ROM log shows `rst:0x8 (TG1WDT_SYS_RST)`
   and the banner `Fatal error: interrupt watchdog reset`.  Idle, nothing
   restarts.

Every unhandled exception, including kernel and double exceptions, a thread's
stack overflow, a tick that stalls for 300 ms (the interrupt watchdog) and
`_exit()` print a register dump through the ROM printf,
keep a record in RAM that survives the reset, and restart the board.  The
handler runs from RAM and seals the record before it reads flash, so the
record survives even when the flash mapping is what failed (the dump itself
goes through console code in flash).  Not exercised on the board: `_exit()`,
a break instruction, a second fault inside the handler, and a handler that
hangs (it stops the interrupt watchdog and arms the RTC watchdog for 2 s, so
that the board still restarts).
