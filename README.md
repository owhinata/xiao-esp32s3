# ThreadX on the Seeed Studio XIAO ESP32S3

Bare-metal [Eclipse ThreadX](https://github.com/eclipse-threadx/threadx) on the
XIAO ESP32S3, built on
[esp-hal-components](https://github.com/espressif/esp-hal-components) (the HAL
part of ESP-IDF, without ESP-IDF, FreeRTOS or the second-stage bootloader).

Status: **boots from flash and from RAM on a XIAO ESP32S3** (ESP32-S3R8, chip
revision v0.2).  Both threads run; one tick is exactly 200000 CPU cycles and
the CPU clock measures 20 MHz +26 ppm against a host clock over two minutes;
no watchdog reset.  See [Bring-up checklist](#bring-up-checklist).

| | |
|---|---|
| MCU | ESP32-S3 (Xtensa LX7, core 0 only) |
| Kernel | ThreadX 6.5.2 (`v6.5.2.202603_rel`), `common/` only |
| Xtensa layer | esp-hal-components `components/xtensa` (vectors, interrupt dispatch, context switch primitives) |
| HAL | esp-hal-components `sync-3-master` (ESP-IDF master as of 2024-12) |
| Toolchain | `xtensa-esp-elf` 14.2.0_20241119, fetched on first configure |
| Console | USB Serial/JTAG on the USB-C connector, polled |
| Tick | 100 Hz from CCOMPARE0 (CPU interrupt 6, level 1) |
| LED | GPIO21, active low |
| Image | RAM-only, loaded by the ROM bootloader |

## Build

```sh
git submodule update --init --depth 1
python3 -m venv .venv && .venv/bin/pip install esptool
cmake -S . -B build -G Ninja
cmake --build build
```

The first configure downloads the compiler (~110 MB) into `tools/`.
Outputs are `build/xiao_esp32s3_threadx.elf`, `.bin` (ESP image) and `.map`.

## Run

Put the board in ROM download mode first: hold **B** (BOOT), tap **R** (RESET),
release **B**.  It enumerates as `303a:1001` (USB JTAG/serial debug unit).

```sh
cmake --build build --target run      # load into RAM and start; flash untouched
cmake --build build --target flash    # write the image to flash offset 0
```

`run` is the one to use while iterating: it costs no flash wear and leaves the
board's existing firmware alone.  `flash` makes the image boot on power-up, and
**replaces the bootloader that is currently at offset 0** (the factory firmware
stops booting until it is written back; the ROM download mode itself cannot be
erased, so the board is always recoverable with esptool).

Pass `-DESPPORT=/dev/ttyACMn` at configure time if more than one serial device
is attached.

Expected on the console (any terminal on the board's `/dev/ttyACM*`):

```
XIAO ESP32S3: ThreadX 6.5.2, CPU clock 20000000 Hz, 100 ticks/s
hello 0  tick=0  ccount=...
hello 1  tick=100  ccount=...
```

and the orange LED toggling every 0.5 s.  `ccount` is the CPU cycle counter;
it advances by 100 ticks x 200000 cycles per line.  The clock figure is
whatever the ROM selected, read back from the clock registers: 20 MHz (the
40 MHz crystal divided by two) after a boot from flash.

esptool resets the board by itself through USB Serial/JTAG, so the buttons are
only needed if the running firmware has taken the USB pins away.  The banner
and `hello 0` are printed before a terminal can attach after `run` or `flash`;
to see them, keep the terminal open and reset the board.

## How it boots

There is no second-stage bootloader.  The image is in ESP image format with
every segment in internal SRAM, so the mask ROM loads it and jumps to `_start`
(`port/startup.c`), which masks interrupts, points VECBASE at the vectors,
clears `.bss`, turns off the watchdogs the ROM armed, and calls `main()`.  Memory layout and the reasons for it are in
`ldscript/xiao_esp32s3_ram.ld`.

Consequences of this first step, in order of how soon they will matter:

- Code and data share 512 KiB of SRAM (128 KiB code / ~300 KiB data as split
  now).  Executing from flash needs the cache and MMU set up in `_start`
  (`hal/mmu_hal.c`, `hal/cache_hal.c`) and an image built with
  `esptool elf2image --ram-only-header`.
- The clock tree is not programmed, so the CPU runs at the ROM's 20 MHz.
  Raising it to 80/160/240 MHz means using the rest of
  `esp_hw_support/port/esp32s3/rtc_clk.c` (PLL setup, which needs `regi2c`).
- Only core 0 runs.  ThreadX (non-SMP) is single-core; the layer is built with
  `portNUM_PROCESSORS=1`.

## Layout

```
cmake/          toolchain file (downloads the compiler)
ldscript/       linker script
lib/            upstream submodules, read-only
port/           board and start-up code
port/threadx/   ThreadX architecture port
src/            application
```

## What comes from where

The rule is to take code from esp-hal-components wherever it can be compiled
as it is, and to write only what connects it to ThreadX and to this board.

**esp-hal-components, unmodified**

| Source | Role |
|---|---|
| `xtensa/xtensa_vectors.S` | Exception/interrupt vectors, window handlers, interrupt dispatch, coprocessor exception |
| `xtensa/xtensa_context.S` | `_xt_context_save/restore`, coprocessor save/restore/release |
| `xtensa/xtensa_intr.c`, `xtensa_intr_asm.S` | Handler tables, `xt_set_interrupt_handler()`, `xt_ints_on/off()` |
| `xtensa/esp32s3/libxt_hal.a` | Xtensa HAL (window spill, extra-state save) |
| `esp_system/port/arch/xtensa/panic_handler_asm.S` | `_xt_panic` |
| `hal/wdt_hal_iram.c` | Watchdog control |
| `esp_hw_support/port/esp32s3/rtc_clk.c` | CPU clock readback (`rtc_clk_cpu_freq_get_config`) |
| `esp_rom/patches/esp_rom_sys.c` + ROM | `esp_rom_printf`, console channel, ticks-per-us |
| `hal/esp32s3/include/hal/*_ll.h` | GPIO, USB Serial/JTAG |
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
| `inc/tx_user.h` | new | Kernel options |
| `inc/xtensa_rtos.h` | new | Maps the layer's RTOS callouts to ThreadX (upstream has a file of the same name for its own copy of the layer) |
| `inc/freertos/xtensa_rtos.h` | new | Include-path shim for `panic_handler_asm.S` |

**This project** (`port/`)

| File | Role |
|---|---|
| `startup.c` | Reset entry |
| `board.c` | LED, clock readback, console |
| `syscalls.c` | newlib fd 0-2 on the console |
| `panic.c` | `panicHandler` / `xt_unhandled_exception`: register dump |
| `include/sdkconfig.h` | The few `CONFIG_` symbols the HAL headers need |

The C library is usable from threads and from `tx_application_define()`, not
from interrupt handlers.  The same goes for the FPU and the vector extension:
using a coprocessor in an interrupt handler ends in a panic.

## Bring-up checklist

All five items have been observed on the board.  In the order a failure would
show up:

1. `run` loads and the banner appears.  No output at all: the fault is before
   or in the console, i.e. `_start`, the load addresses, or USB Serial/JTAG.
2. The banner shows a plausible clock (20 MHz after ROM boot).
3. `hello` lines arrive once per second and `tick` advances by 100 per line.
   Banner but no `hello`: the first context switch or the tick interrupt.
   Wrong rate: the clock readback in `board_cpu_freq_hz()`.
4. The LED blinks at 1 Hz.
5. The same after `flash` and a power cycle (this is the path where the ROM
   watchdogs are armed; a reset loop every few seconds points at
   `watchdogs_disable()`).

Every unhandled exception, including kernel and double exceptions, prints a
register dump through the ROM printf and halts.  That output needs the console,
so a fault before `console_init()` in `_start` is silent.
