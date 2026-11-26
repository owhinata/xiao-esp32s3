# Where the ThreadX port comes from

The kernel (`lib/threadx/common`) and the execution profile kit
(`utility/execution_profile_kit/tx_execution_profile.c`) are built from the
fetched ThreadX tree, unmodified.  ThreadX is under the MIT licence; the
copies and adaptations below keep their headers.

## The Xtensa port

The upstream Xtensa port (`lib/threadx/ports/xtensa/xcc`) is not built: it
brings its own copy of the Xtensa layer, written for the Cadence toolchain.
This firmware uses Espressif's Xtensa layer instead (esp-hal-3rdparty
`components/xtensa`), the RTOS-neutral half of what ESP-IDF's FreeRTOS port is
built on.  That layer calls the RTOS through the names in `xtensa_rtos.h`,
which `inc/xtensa_rtos.h` maps to ThreadX.  The ThreadX-specific files of the
upstream port were copied here and adapted to Espressif's layer.

Paths are relative to `port/threadx/`.  "upstream" is
`lib/threadx/ports/xtensa/xcc` at the commit `CMakeLists.txt` pins.

| File | Origin | State |
|---|---|---|
| `src/tx_thread_context_save.S`, `tx_thread_context_restore.S`, `tx_thread_schedule.S`, `tx_thread_stack_build.S` | upstream | Verbatim copies |
| `src/tx_thread_system_return.S` | upstream | Interrupts are disabled before the call of `_tx_execution_thread_exit()`, not after it (windowed ABI, XEA2).  Upstream calls the execution profile kit with interrupts enabled: an interrupt between the kit's reading of the time and of the thread's start time leaves a start time later than the time read, and the kit, taking that for a wrap of the 32-bit source, adds a full turn (17.9 s) to the thread's total. |
| `inc/tx_api_asm.h` | upstream | Verbatim copy |
| `src/tx_timer_interrupt.S` | upstream | One symbol renamed (`_xt_tick_divisor`) |
| `inc/tx_port.h` | upstream | Interrupt lockout and `_xt_coproc_release()` adapted, and the thread completed and terminated extensions reclaim the thread's C library reent (`TX_CLIB_THREAD_EXIT_EXTENSION`); see the note at the top of the file.  Upstream reclaims it only in the entry wrapper, after a return from the entry: a thread that is terminated (FreeRTOS's `vTaskDelete()`, the shell's thread exit) keeps what newlib allocated for it, and newlib's `_reclaim_reent()` does nothing for the reent `_impure_ptr` points at, which in the wrapper is the thread's own, so the return leaked it as well. |
| `src/tx_initialize_low_level.c`, `tx_thread_interrupt_control.c` | upstream | Rewritten for the layer's API and GCC |
| `src/tx_clib_newlib.c` | new | Replaces upstream `tx_clib_lock.c`, which does not build against Espressif's newlib.  Per-thread `_reent`, `__retarget_lock_*` and ESP-IDF's `_lock_*` on `TX_MUTEX`; no heap (`malloc()` is ESP-IDF's).  `_tx_clib_reent_cleanup()` is called once per run of a thread, from the completed and terminated extensions, and moves the thread (and `_impure_ptr`, when it is the current one) to the global reent before `_reclaim_reent()`. |
| `src/tx_port_asm.S` | new | Coprocessor save-area lookup (taken from upstream `xtensa_coproc_handler.S`), exception hook, interrupt stack |
| `src/tx_port_offset_check.c` | new | Build-time check of the TCB offsets the assembly uses.  The upstream Xtensa port has no such check; the name follows the one upstream's Cortex-R ports use. |
| `inc/tx_user.h` | new | Kernel options: 1 kHz tick, timers processed in the tick interrupt, and the per-thread pointer the shell's kernel layer needs |
| `inc/xtensa_rtos.h` | new | Maps the layer's RTOS callouts to ThreadX; the tick goes through the board's `board_tick_interrupt()`, which feeds the interrupt watchdog (upstream has a file of the same name for its own copy of the layer) |
| `inc/freertos/xtensa_rtos.h` | new | Include-path shim for `panic_handler_asm.S` |

The execution profile kit is told about interrupt entry through a hook
Espressif's dispatcher already has (`CONFIG_PM_TRACE`), which
`inc/xtensa_rtos.h` maps to `_tx_execution_isr_enter`.

## The FreeRTOS compatibility layer

`freertos/tx_freertos.c` and `freertos/FreeRTOS.h` are modified copies of
ThreadX's FreeRTOS compatibility layer,
`utility/rtos_compatibility_layers/FreeRTOS/` of the ThreadX repository at
the commit `CMakeLists.txt` pins (93387b0a, tag v6.5.2.202603_rel).  The
layer's other files are not copied: its `task.h`, `queue.h`, `semphr.h`,
`event_groups.h` and `timers.h` are empty, and ESP-IDF sources include
`port/esp_compat/freertos/include/freertos/` instead.  The copies keep the
upstream formatting and comments.

The layer is configured by `port/esp_compat/freertos/include/FreeRTOSConfig.h`
(FreeRTOS priority p is ThreadX priority 31 - p; the objects it creates come
from its own byte pool, `configTOTAL_HEAP_SIZE`; its thread
`txfr_idle_task` deletes the tasks that end), which also holds the one
implementation of the critical sections.  What ESP-IDF's FreeRTOS adds to
the API is in `port/esp_compat/freertos/`.

The changes, with the lines of the upstream file they replace:

| File, upstream lines | Change | Why |
|---|---|---|
| `tx_freertos.c` 93-136 (`txfr_idle_task_entry`), `FreeRTOS.h` 161-174 (`p_stack`) | The idle task frees a deleted task's stack as well as its task block, for tasks `xTaskCreate()` allocated; `xTaskCreate()` keeps the stack's address in the task block. | Upstream frees only the task block, so every task that ends leaves its stack in the pool. |
| `tx_freertos.c` 117-120, `FreeRTOS.h` 92-95 (`TX_FREERTOS_TASK_DELETE_HOOK`) | The idle task calls `TX_FREERTOS_TASK_DELETE_HOOK(thread)` for every deleted task before it deletes the thread (empty by default; the configuration runs the thread-specific data destructors). | ESP-IDF's FreeRTOS frees a task's local storage values when it frees the task, whoever deleted it; the Wi-Fi driver keeps a semaphore per thread there. |
| `tx_freertos.c` 306-461 (`xTaskCreateStatic`, `xTaskCreate`), `FreeRTOS.h` 102-104, 161-174 (`name`, `configMAX_TASK_NAME_LEN`) | The task's name is copied into the task block (at most `configMAX_TASK_NAME_LEN` - 1 characters; NULL gives "") and the thread is named with the copy. | FreeRTOS copies the name; upstream hands the caller's pointer to the thread, which dangles when the caller's string goes away. |
| `tx_freertos.c` 523-530 (`xTaskGetCurrentTaskHandle`) | Returns NULL when there is no current thread. | Upstream dereferences `tx_thread_identify()`, which is NULL in an interrupt handler that interrupted the scheduler's idle wait and before the kernel runs. |
| `tx_freertos.c` 652-672 (`pcTaskGetName`) | The name is read into a `CHAR *`. | Upstream passes a `const char **` where `tx_thread_info_get()` takes a `CHAR **`, which GCC 14 and later reject (`-Wincompatible-pointer-types`). |
| `tx_freertos.c` 732-741 (`vTaskDelayUntil`) | The tick count is read whole, and the task does not sleep when the wake time has already passed. | Upstream truncates it to 16 bits (wrong after 65 s) and, when a period was overrun, sleeps for nearly 2^32 ticks. |
| `tx_freertos.c` 301-304 (`vTaskDelay`) | A delay of 0 yields (`tx_thread_relinquish()`). | FreeRTOS's `vTaskDelay(0)` yields; `tx_thread_sleep(0)` returns at once. |
| `tx_freertos.c` 266-278 (`xTaskGetSchedulerState`) | `taskSCHEDULER_NOT_STARTED` before the kernel runs, during `tx_application_define()` and until the first thread runs. | Upstream says "running" then, and ESP-IDF's log takes its mutex and its tick-based time stamp on that answer. |
| `tx_freertos.c` 761-869 (`ulTaskNotifyTake`, `xTaskNotifyWait`), 969-977 (`xTaskNotifyAndQuery`) | A task that was notified between its check and its wait takes the value itself instead of returning an uninitialised one; the notifier hands the value over once (it clears the task's pointer to it).  `ulTaskNotifyTake(pdFALSE)` keeps the notification pending while the count stays above 0 and does not decrement a count of 0.  `xTaskNotifyWait()` clears `ulBitsToClearOnEntry` when it has to wait, accepts a NULL `pulNotificationValue` and stores the value on a timeout too. | Upstream hands the value over only when the task is already suspended, so a notification in that window returns garbage (esp_timer's task waits this way); a count above 1 was lost after the first take; the entry mask was ignored. |
| `tx_freertos.c` 1001-1035 (`xTaskNotifyStateClear`) | Returns whether a notification was pending, and clears it; no failure when none was. | Upstream ends in `TX_FREERTOS_ASSERT_FAIL()` when no notification is pending, and returns whether the task was waiting. |
| `tx_freertos.c` 1165, 1186 (`xSemaphoreCreateMutex`, `xSemaphoreCreateMutexStatic`) | Plain mutexes inherit priority (`TX_INHERIT`), as the recursive ones already do.  A second take by the owner still fails (`xSemaphoreTake()`). | FreeRTOS mutexes inherit the priority of the most urgent waiter; lwIP's `sys_arch_protect()` and the Wi-Fi driver's mutexes rely on it. |
| `tx_freertos.c` 1423-1437 (`uxSemaphoreGetCount`) | For a mutex: 1 when free, 0 when taken. | Upstream reads the unused semaphore of a mutex and ends in `TX_FREERTOS_ASSERT_FAIL()`. |
| `tx_freertos.c` 1439-1444 (`xSemaphoreGetMutexHolder`) | NULL when the mutex is free, read with interrupts masked. | Upstream dereferences the owner, which is NULL when the mutex is free. |
| `tx_freertos.c` 2153-2156 (`xEventGroupWaitBits`) | When the wait ends without its condition, returns the group's bits. | FreeRTOS returns the bits at that moment; upstream returns 0. |
| `tx_freertos.c` 2345-2349 (`xTimerCreateStatic`), 2450-2458 (`xTimerChangePeriod`), 2584 (`vTimerSetReloadMode`), 2675 (`uxTimerGetReloadMode`), 2652-2656 (`xTimerGetExpiryTime`) | The field `one_shot` (which holds "auto-reload") is set for static timers and by `vTimerSetReloadMode()`, and read the right way round by `uxTimerGetReloadMode()`; `xTimerChangePeriod()` records the new period; `xTimerGetExpiryTime()` unmasks the interrupts on its error path. | Upstream turns a static auto-reload timer into a one-shot one at its first reset, reports the reload mode inverted, resets to the old period after a change, and returns with interrupts masked on that error. |
| `tx_freertos.c` 1679-1749 (`xQueueSendToFront`) | The space is waited for before preemption is disabled; the interrupts are masked only for the copy; every return enables preemption again, and the switch to a thread readied meanwhile is made. | For a queue in a queue set upstream masks the interrupts and disables preemption before it waits for space: a full queue returns with both left so (the board stops), a wait cannot suspend with preemption disabled, and the second `TX_DISABLE` overwrites the saved interrupt level, so even the success returns with the interrupts masked. |
| `tx_freertos.c` 1841-1857 (`xQueuePeek`) | Preemption and the interrupts are restored before the error return, and the reader readied by the put back is switched to. | Upstream returns with both left disabled when the put fails, and switches only at the next interrupt. |
| `tx_freertos.c` 1992-2050 (`xQueueOverwrite`, `xQueueOverwriteFromISR`) | Into an empty queue the item takes a free slot (the write semaphore's count) like `xQueueSend()`, and the queue set is told; the decision, the copy and the post are one section with the interrupts masked.  When every slot is held by a sender or receiver between its semaphore and its copy, the task variant sleeps a tick and tries again, the ISR variant returns `pdFAIL`. | Upstream stores into an empty queue without taking the slot, so a following `xQueueSend()` to a length-1 queue finds room again, writes the same storage and posts a second read token: two receives for one item. |
| `FreeRTOS.h` 271-274, 312-315, `tx_freertos.c` 236-253 | `portENTER_CRITICAL()`, `portEXIT_CRITICAL()`, `taskENTER_CRITICAL()` and `taskEXIT_CRITICAL()` are defaults the configuration can replace, as the layer already allows for `portDISABLE_INTERRUPTS()` and `taskENTER_CRITICAL_FROM_ISR()`; `vPortEnterCritical()` and `vPortExitCritical()` exist only with the defaults. | One critical section for the stock and the ESP-IDF forms.  Upstream's enables the interrupts when `_tx_thread_preempt_disable` drops to 0 whatever they were before, and does not switch to a thread readied inside. |
| `FreeRTOS.h` 580-585 (`pdMS_TO_TICKS`) | Computed in 64 bits; `pdTICKS_TO_MS()` and `portTICK_PERIOD_MS` added, as FreeRTOS 11 defines them. | Upstream's product overflows 32 bits above 4 294 967 ms at a 1 kHz tick (71.6 minutes). |
