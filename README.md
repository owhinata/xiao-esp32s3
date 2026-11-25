# ThreadX on the Seeed Studio XIAO ESP32S3

Bare-metal [Eclipse ThreadX](https://github.com/eclipse-threadx/threadx) on the
XIAO ESP32S3, built on
[esp-hal-3rdparty](https://github.com/espressif/esp-hal-3rdparty) (an
unmodified mirror of ESP-IDF's components, used here without ESP-IDF's build
system, FreeRTOS or the second-stage bootloader).

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
| Console | USB Serial/JTAG on the USB-C connector; polled transmit, interrupt-driven receive |
| Shell | The shell of [rp2040-connect](https://github.com/owhinata/rp2040-connect), copied into `shell/common/` |
| Tick | 1000 Hz from CCOMPARE0 (CPU interrupt 6, level 1) |
| Uptime clock | SYSTIMER counter 0 (16 MHz, from the crystal) |
| LED | GPIO21, active low |
| CPU clock | 240 MHz from the PLL (`BOARD_CPU_FREQ_MHZ`) |
| Image | Booted directly by the ROM; code and constants execute from flash |

## Build

```sh
python3 -m venv .venv && .venv/bin/pip install esptool
cmake -S . -B build -G Ninja
cmake --build build
```

The first configure downloads the compiler (~90 MB) into `tools/` and the
two upstream source trees (ThreadX and esp-hal-3rdparty, ~370 MB) into
`lib/`, each at the commit pinned in `CMakeLists.txt`.  Neither directory is
tracked by this repository.
Outputs are `build/xiao_esp32s3_threadx.elf`, `.bin` (ESP image) and `.map`.

## Run

```sh
cmake --build build --target flash    # write the image to flash offset 0
```

`flash` **replaces the bootloader that is at offset 0** (whatever firmware was
on the board stops booting until it is written back; the ROM download mode
itself cannot be erased, so the board is always recoverable with esptool).

esptool resets the board into download mode by itself through USB Serial/JTAG,
so the buttons are only needed if the running firmware has taken the USB pins
away: hold **B** (BOOT), tap **R** (RESET), release **B**.

Pass `-DESPPORT=/dev/ttyACMn` at configure time if more than one serial device
is attached.

On the console (any terminal on the board's `/dev/ttyACM*`):

```
XIAO ESP32S3: ThreadX 6.5.2, CPU clock 240000000 Hz, 1000 ticks/s

xiao:~$ help
Available commands:
  bootsel  : Restart into the ROM download mode (for esptool)
  devmem   : Read or write memory: devmem <address> [<width> [<value>]]
  dmesg    : Show logs stored in RAM
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
opened later gets a prompt on Enter.

`bootsel` keeps its name from rp2040-connect; on this board it restarts into
the ROM download mode, where esptool can connect.  A reset from esptool or the
RESET button returns to the firmware.  `id` prints the factory MAC address
from eFuse.

`dmesg` shows a log kept in RAM that survives a reset, so it also holds the
lines of earlier boots.  rp2040-connect's `date` and `fault` are not built:
nothing here sets a wall clock, and the fault record is laid out for a
Cortex-M.  `ps` has no CPU column, because this port does not account run time
per thread.

## How it boots

There is no second-stage bootloader and no ESP-IDF start-up code.

1. The image is built with `esptool elf2image --ram-only-header`.  The mask ROM
   sees only its RAM segments, loads them into internal SRAM and jumps to
   `_start` (`port/startup.c`).  `script/add_boot_digest.py` adds the SHA-256
   the ROM checks at that point.
2. `_start` masks interrupts, points VECBASE at the vectors, clears `.bss` and
   turns off the watchdogs the ROM armed.
3. Clocks: ESP-IDF's own `bootloader_clock_configure()` turns the PLL on and
   runs the CPU from it at 80 MHz.
4. `flash_map_image()` (`port/flash_map.c`) finds the two flash segments that
   sit behind the RAM segments in the same image, sets up the cache and MMU
   through the HAL and the flash pins and timing through `bootloader_support`,
   and maps them: code at 0x42000000, constants at 0x3C000000.
5. CPU to 240 MHz, console, `main()`.

Everything up to step 4 has to run from RAM; the linker script
(`ldscript/xiao_esp32s3.ld`) lists what is kept there and why, and describes
the memory layout.  Two facts that are easy to get wrong and cost a debugging
session here:

- The instruction cache uses the first 16 KiB of SRAM0 as its own memory.
  Anything linked there works until the first instruction is fetched from
  flash, and is then overwritten.
- ESP-IDF's bootloader only reads flash as data.  Executing from it also needs
  the cache geometry set and the ROM told where code ends and data begins in
  the shared MMU table (`rom_config_instruction_cache_mode`,
  `Cache_Set_IDROM_MMU_Size`), which ESP-IDF does in its application start-up.

What is still missing compared with an ESP-IDF start-up:

- `rtc_init()` is not called.  It trims the internal voltage regulators from
  eFuse calibration values; without it they run at the defaults compiled into
  `rtc_init.c`.  Needed before the radio is used, and the conservative choice
  if 240 MHz ever proves unstable on a board.
- PSRAM (8 MB on this board) is not initialized.
- Only core 0 runs.  ThreadX (non-SMP) is single-core; the layer is built with
  `portNUM_PROCESSORS=1`.

## Layout

```
cmake/          toolchain file (downloads the compiler), upstream fetch
ldscript/       linker script
script/         build helpers
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
| `esp_hal_wdt/wdt_hal_iram.c` | Watchdog control |
| `esp_hal_cache/cache_hal.c`, `mmu_hal.c` | Cache and MMU for the flash mapping |
| `bootloader_support/src/bootloader_clock_init.c` | Clock set-up before the flash is mapped |
| `bootloader_support/bootloader_flash/src/bootloader_flash_config_esp32s3.c` | Flash pin drive, dummy-phase levels, chip-select timing |
| `esp_hal_systimer/systimer_hal.c`, `esp_hw_support/port/esp32s3/systimer.c` | Microsecond uptime clock |
| `esp_hw_support/port/esp32s3/rtc_clk.c`, `rtc_clk_init.c`, `esp_hw_support/regi2c_ctrl.c`, `esp_hal_regi2c/esp32s3/rom.regi2c.ld` | PLL and CPU clock |
| `esp_rom/patches/esp_rom_cache_esp32s2_esp32s3.c` | Work-arounds for ROM cache bugs |
| `esp_rom/patches/esp_rom_sys.c` + ROM | `esp_rom_printf`, console channel, ticks-per-us |
| LL headers of `esp_hal_gpio`, `esp_hal_usb`, `hal` | GPIO, USB Serial/JTAG, eFuse |
| `soc/`, `esp_rom/` linker scripts | Peripheral and ROM symbol addresses |

Espressif's Xtensa layer is the RTOS-neutral half of what ESP-IDF's FreeRTOS
port is built on.  It calls the RTOS through the names in `xtensa_rtos.h`,
which `port/threadx/inc/xtensa_rtos.h` maps to ThreadX.

**ThreadX**

`lib/threadx/common` is the kernel.  The upstream Xtensa port
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
| `src/tx_initialize_low_level.c`, `tx_thread_interrupt_control.c`, `tx_xtensa_stack_error_handler.c` | upstream | Rewritten for the layer's API and GCC |
| `src/tx_clib_newlib.c` | new | Replaces upstream `tx_clib_lock.c`, which does not build against Espressif's newlib.  Per-thread `_reent`, `__retarget_lock_*` on `TX_MUTEX`, `_sbrk_r`. |
| `src/tx_port_asm.S` | new | Coprocessor save-area lookup (taken from upstream `xtensa_coproc_handler.S`), exception hook, interrupt stack |
| `src/tx_port_offset_check.c` | new | Build-time check of the TCB offsets the assembly uses.  The upstream Xtensa port has no such check; the name follows the one upstream's Cortex-R ports use. |
| `inc/tx_user.h` | new | Kernel options: 1 kHz tick, timers processed in the tick interrupt, and the per-thread pointer the shell's kernel layer needs |
| `inc/xtensa_rtos.h` | new | Maps the layer's RTOS callouts to ThreadX (upstream has a file of the same name for its own copy of the layer) |
| `inc/freertos/xtensa_rtos.h` | new | Include-path shim for `panic_handler_asm.S` |

**rp2040-connect**

`shell/common/` holds the 60 files of rp2040-connect's `common/` directory
(commit `72f3485`) that this firmware uses, at the same relative paths: the
shell engine, the generic commands, and the Zephyr-compatibility kernel layer
they are written against.  All but two are byte-identical copies;
`shell/ORIGIN.md` lists them, the changes, and their licences (MIT,
Apache-2.0 for the parts derived from Zephyr, BSD-3-Clause for getopt and
fnmatch).  `shell/port/` is this board's side of that code: the console
transport, the kernel port header, the log back end, and the commands only the
board can answer (`version`, `free`, `ps`).

**This project** (`port/`)

| File | Role |
|---|---|
| `startup.c` | Reset entry, clocks |
| `flash_map.c` | Finds the flash segments in the image and maps them.  ESP-IDF's code for this is part of its image loader and cannot be called on its own, so this follows it; the flash pin and timing set-up it needs is called from `bootloader_support` |
| `esp_compat/` | What the Espressif sources expect from parts of ESP-IDF that are not built or not in the mirror: a `freertos/FreeRTOS.h` that provides critical sections only, small stand-in headers for `esp_sleep` and `esp_mspi`, and documented stubs |
| `board.c` | LED, clock readback, uptime, console (with its receive interrupt), device ID, restart |
| `syscalls.c` | newlib fd 0-2 on the console; once the shell runs, stdout and stderr go through the shell's console layer |
| `panic.c` | `panicHandler` / `xt_unhandled_exception`: register dump |
| `include/sdkconfig.h` | The few `CONFIG_` symbols the HAL headers need |

The C library is usable from threads and from `tx_application_define()`, not
from interrupt handlers (output written from one bypasses the shell's console
layer and goes to the port directly).  The same goes for the FPU and the vector extension:
using a coprocessor in an interrupt handler ends in a panic.

## Bring-up checklist

Everything below has been observed on the board with the current image.  In
the order a failure would show up:

1. The banner appears after `flash`.  No output at all: the fault is before
   or in the console, i.e. `_start`, the flash mapping, or USB Serial/JTAG.
2. The banner shows the configured clock (240 MHz).
3. The prompt appears and `help` lists the commands.  Banner but no prompt:
   the first context switch, the tick interrupt, or the receive interrupt.
4. `uptime` twice: seconds and ticks advance together (1 tick = 1 ms).
5. `ps` lists `blink`, `console` and `(interrupts)` with their free stack.
6. `id` prints the MAC address esptool reports.
7. `devmem 0` ends in a register dump (the fault path), `reboot` comes back
   to the banner, `bootsel` lets esptool connect.
8. The LED blinks at 1 Hz.

Every unhandled exception, including kernel and double exceptions, prints a
register dump through the ROM printf and halts.  The handler is kept in RAM so
that it also works when the flash mapping is what failed.
