/* SPDX-License-Identifier: MIT */
/*
 * What the ThreadX part of the Zephyr compatibility layer
 * (shell/common/compat/port/threadx/zephyr_kernel_os_impl.h, which includes
 * this header) takes from the ThreadX port and the board: here the Xtensa
 * port of port/threadx on the XIAO ESP32S3.
 *
 *   z_os_in_isr()          an interrupt handler is running: the port counts
 *                          interrupt nesting in _tx_thread_system_state
 *                          (values from TX_INITIALIZE_IN_PROGRESS up mean
 *                          that the kernel is not running yet)
 *   z_tx_irq_save(),       the port's TX_DISABLE and TX_RESTORE: PS.INTLEVEL
 *   z_tx_irq_restore(s)    raised to XCHAL_EXCM_LEVEL; the state is the
 *                          previous PS
 *   z_os_busy_wait_us(us)  spins on the CPU cycle counter
 *   z_os_clock_us()        the board's uptime (SYSTIMER)
 *   Z_TX_ASSERT(test)      a failed test ends in the fault handler's
 *                          register dump
 *
 * The board functions are called rather than inlined so that no Espressif
 * header, and with it no sdkconfig.h, reaches the shared sources: both
 * sdkconfig.h and the layer's log give a meaning to CONFIG_LOG_DEFAULT_LEVEL.
 */
#ifndef SHELL_PORT_ZEPHYR_OS_PORT_H
#define SHELL_PORT_ZEPHYR_OS_PORT_H

#include <stdbool.h>
#include <stdint.h>

#include "tx_api.h"
#include "tx_initialize.h"
#include "tx_thread.h"

#include "board.h"

static inline bool z_os_in_isr(void)
{
    ULONG state = TX_THREAD_GET_SYSTEM_STATE();

    return state != 0u && state < TX_INITIALIZE_IN_PROGRESS;
}

static inline UINT z_tx_irq_save(void)
{
    return XTOS_SET_INTLEVEL(XCHAL_EXCM_LEVEL);
}

static inline void z_tx_irq_restore(UINT state)
{
    XTOS_RESTORE_INTLEVEL(state);
}

static inline void z_os_busy_wait_us(uint32_t us)
{
    board_delay_us(us);
}

static inline uint64_t z_os_clock_us(void)
{
    return board_uptime_us();
}

/* Stops at the spot: the break instruction ends in panicHandler(). */
#define Z_TX_ASSERT(test)                                                      \
    do {                                                                       \
        if (!(test)) {                                                         \
            __builtin_trap();                                                  \
        }                                                                      \
    } while (0)

#endif /* SHELL_PORT_ZEPHYR_OS_PORT_H */
