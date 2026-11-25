# Where the ThreadX port comes from

The kernel (`lib/threadx/common`), the execution profile kit
(`utility/execution_profile_kit/tx_execution_profile.c`) and the FreeRTOS
compatibility layer (`utility/rtos_compatibility_layers/FreeRTOS/tx_freertos.c`)
are built from the fetched ThreadX tree, unmodified.

The upstream Xtensa port (`lib/threadx/ports/xtensa/xcc`) is not built: it
brings its own copy of the Xtensa layer, written for the Cadence toolchain.
This firmware uses Espressif's Xtensa layer instead (esp-hal-3rdparty
`components/xtensa`), the RTOS-neutral half of what ESP-IDF's FreeRTOS port is
built on.  That layer calls the RTOS through the names in `xtensa_rtos.h`,
which `inc/xtensa_rtos.h` maps to ThreadX.  The ThreadX-specific files of the
upstream port were copied here and adapted to Espressif's layer.  ThreadX is
under the MIT licence; the copies keep their headers.

Paths are relative to `port/threadx/`.  "upstream" is
`lib/threadx/ports/xtensa/xcc` at the commit `CMakeLists.txt` pins.

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

The execution profile kit is told about interrupt entry through a hook
Espressif's dispatcher already has (`CONFIG_PM_TRACE`), which
`inc/xtensa_rtos.h` maps to `_tx_execution_isr_enter`.  The FreeRTOS layer is
configured by `port/esp_compat/freertos/include/FreeRTOSConfig.h`: FreeRTOS
priority p is ThreadX priority 31 - p, and the objects it creates come from
its own byte pool.  Its thread `txfr_idle_task` frees deleted tasks.
