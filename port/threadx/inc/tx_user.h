/*
 * ThreadX build options for this port.
 *
 * Also included by assembly sources and by Espressif's Xtensa layer (through
 * xtensa_rtos.h), so it may hold preprocessor definitions only, and values
 * must be plain numbers without C suffixes.  C-only text is under
 * #ifndef __ASSEMBLER__.
 */
#ifndef TX_USER_H
#define TX_USER_H

/*
 * Tick rate.  The CCOMPARE reload value is derived from it at start-up.
 * 1 ms is what the Zephyr compatibility layer under the shell is written for
 * (shell/common/compat/port/threadx/zephyr_kernel_os_impl.h refuses anything
 * else).
 */
#define TX_TIMER_TICKS_PER_SECOND       1000

/*
 * Sleeps and timeouts end in the tick interrupt itself; there is no timer
 * thread.  The layer relies on it: a waiter's timeout must not depend on the
 * priority of another thread.  The expiration routines therefore run on the
 * interrupt stack (TX_SYSTEM_STACK_SIZE).
 */
#define TX_TIMER_PROCESS_IN_ISR

/*
 * Per-thread pointers, which tx_thread_create() clears:
 *   tx_thread_zephyr_local  the shell's Zephyr layer (Zephyr's thread custom
 *                           data): the console a thread's output goes to;
 *   txfr_thread_ptr         ThreadX's FreeRTOS compatibility layer
 *                           (tx_freertos.c, which requires this name): the
 *                           FreeRTOS task a thread belongs to, NULL for a
 *                           thread made with tx_thread_create();
 *   tx_thread_pthread_local the values of the thread-specific data keys
 *                           (pthread_getspecific(),
 *                           port/esp_compat/pthread_local.c), one per key.
 * The text is C; assembly sources never expand it.
 */
#define TX_PTHREAD_KEYS_MAX             4
#define TX_THREAD_USER_EXTENSION                                               \
    VOID *tx_thread_zephyr_local;                                              \
    VOID *txfr_thread_ptr;                                                     \
    VOID *tx_thread_pthread_local[TX_PTHREAD_KEYS_MAX];

/* Detect thread stack overflow at context switches. */
#define TX_ENABLE_STACK_CHECKING

/*
 * Run time of each thread, of the interrupt handlers and of the scheduler's
 * wait for "ps": ThreadX's execution profile kit
 * (lib/threadx/utility/execution_profile_kit, built unmodified).  The port's
 * assembly calls it at every switch into a thread (tx_thread_schedule.S),
 * out of one (tx_thread_system_return.S) and at the exit of an interrupt
 * (tx_thread_context_restore.S); the entry of an interrupt is
 * xtensa_rtos.h's mapping of the layer's dispatcher hook.
 *
 * Its clock is the CPU cycle counter CCOUNT (240 MHz, one RSR instruction).
 * The kit takes a 32-bit source that wraps at TX_EXECUTION_MAX_TIME_SOURCE
 * and computes each span with one wrap: a span is cut by every interrupt
 * that goes through _tx_thread_context_save(), the 1 ms tick included, so
 * it stays far below the 17.9 s of a full turn unless interrupts are
 * masked that long.  The totals are 64-bit (584 years of cycles).  A span
 * that starts when CCOUNT is exactly 0 is not counted (the kit takes a
 * start of 0 for "not running").  CCOUNT and SYSTIMER both come from the
 * crystal, so cycles convert to the microseconds of the uptime exactly.
 */
#define TX_EXECUTION_PROFILE_ENABLE
#define TX_EXECUTION_MAX_TIME_SOURCE    0xFFFFFFFF

#ifndef __ASSEMBLER__
/*
 * Read by the kit's C code only, after its typedef of
 * EXECUTION_TIME_SOURCE_TYPE.  The instruction of Espressif's
 * xt_utils_get_cycle_count(), written out so that no Espressif header (and
 * with it sdkconfig.h) reaches every source that includes tx_api.h.
 */
#define TX_EXECUTION_TIME_SOURCE                                               \
    __extension__ ({                                                           \
        unsigned long tx_ccount_;                                              \
        __asm__ volatile ("rsr.ccount %0" : "=a" (tx_ccount_));               \
        (EXECUTION_TIME_SOURCE_TYPE) tx_ccount_;                               \
    })
#endif

#endif
