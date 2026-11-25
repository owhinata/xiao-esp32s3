/*
 * ThreadX build options for this port.
 *
 * Also included by assembly sources and by Espressif's Xtensa layer (through
 * xtensa_rtos.h), so it may hold preprocessor definitions only, and values
 * must be plain numbers without C suffixes.
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
 * One pointer per thread for the layer (Zephyr's thread custom data): the
 * console a thread's output goes to.  tx_thread_create() clears it.  The
 * text is C; assembly sources never expand it.
 */
#define TX_THREAD_USER_EXTENSION        VOID *tx_thread_zephyr_local;

/* Detect thread stack overflow at context switches. */
#define TX_ENABLE_STACK_CHECKING

#endif
