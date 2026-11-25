# ThreadX on the Seeed Studio XIAO ESP32S3

Bare-metal [Eclipse ThreadX](https://github.com/eclipse-threadx/threadx) on the
XIAO ESP32S3, built on
[esp-hal-components](https://github.com/espressif/esp-hal-components) (the HAL
part of ESP-IDF, without ESP-IDF, FreeRTOS or the second-stage bootloader).

Status: **runs on a XIAO ESP32S3** (ESP32-S3R8, chip revision v0.2), executing
from flash at 240 MHz.  Both demo threads run; one tick is exactly 2400000 CPU
cycles and the CPU clock measures 240 MHz within 60 ppm of a host clock; no
watchdog reset.  See [Bring-up checklist](#bring-up-checklist).

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
| CPU clock | 240 MHz from the PLL (`BOARD_CPU_FREQ_MHZ`) |
| Image | Booted directly by the ROM; code and constants execute from flash |

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

Expected on the console (any terminal on the board's `/dev/ttyACM*`):

```
XIAO ESP32S3: ThreadX 6.5.2, CPU clock 240000000 Hz, 100 ticks/s
hello 0  tick=0  ccount=...
hello 1  tick=100  ccount=...
```

and the orange LED toggling every 0.5 s.  `ccount` is the CPU cycle counter;
it advances by 100 ticks x 2400000 cycles per line.  The clock figure is read
back from the clock registers.

The banner and `hello 0` are printed before a terminal can attach after
`flash`; to see them, keep the terminal open and reset the board.

## How it boots

There is no second-stage bootloader and no ESP-IDF start-up code.

1. The image is built with `esptool elf2image --ram-only-header`.  The mask ROM
   sees only its RAM segments, loads them into internal SRAM and jumps to
   `_start` (`port/startup.c`).  `script/add_boot_digest.py` adds the SHA-256
   the ROM checks at that point.
2. `_start` masks interrupts, points VECBASE at the vectors, clears `.bss` and
   turns off the watchdogs the ROM armed.
3. Clocks, as ESP-IDF's bootloader does: PLL on, CPU at 80 MHz (`rtc_clk_init`).
4. `flash_map_image()` (`port/flash_map.c`) finds the two flash segments that
   sit behind the RAM segments in the same image, sets up the cache and MMU
   through the HAL, and maps them: code at 0x42000000, constants at 0x3C000000.
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
cmake/          toolchain file (downloads the compiler)
ldscript/       linker script
script/         build helpers
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
| `hal/cache_hal.c`, `hal/mmu_hal.c` | Cache and MMU for the flash mapping |
| `esp_hw_support/port/esp32s3/rtc_clk.c`, `rtc_clk_init.c`, `esp_hw_support/regi2c_ctrl.c` | PLL and CPU clock |
| `esp_rom/patches/esp_rom_cache_esp32s2_esp32s3.c` | Work-arounds for ROM cache bugs |
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
| `startup.c` | Reset entry, clocks |
| `flash_map.c` | Maps the flash part of the image (follows ESP-IDF's `bootloader_support`, which is not in esp-hal-components) |
| `esp_compat/` | What esp-hal-components sources expect from the rest of ESP-IDF: a `freertos/FreeRTOS.h` that provides critical sections only, and documented stubs |
| `board.c` | LED, clock readback, console |
| `syscalls.c` | newlib fd 0-2 on the console |
| `panic.c` | `panicHandler` / `xt_unhandled_exception`: register dump |
| `include/sdkconfig.h` | The few `CONFIG_` symbols the HAL headers need |

The C library is usable from threads and from `tx_application_define()`, not
from interrupt handlers.  The same goes for the FPU and the vector extension:
using a coprocessor in an interrupt handler ends in a panic.

## Bring-up checklist

Items 1 to 3 have been observed on the board with the current image.  The LED
(item 4) and a power cycle (item 5) were observed with the earlier RAM-only
image and have not been looked at again since.  In the order a failure would
show up:

1. The banner appears after `flash`.  No output at all: the fault is before
   or in the console, i.e. `_start`, the load addresses, or USB Serial/JTAG.
2. The banner shows the configured clock (240 MHz).
3. `hello` lines arrive once per second and `tick` advances by 100 per line.
   Banner but no `hello`: the first context switch or the tick interrupt.
   Wrong rate: the clock readback in `board_cpu_freq_hz()`.
4. The LED blinks at 1 Hz.
5. The same after a power cycle (the ROM arms its watchdogs on every boot from
   flash; a reset loop every few seconds points at `watchdogs_disable()`).

Every unhandled exception, including kernel and double exceptions, prints a
register dump through the ROM printf and halts.  The handler is kept in RAM so
that it also works when the flash mapping is what failed.
