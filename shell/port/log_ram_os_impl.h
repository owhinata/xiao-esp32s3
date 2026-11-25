/* SPDX-License-Identifier: MIT */
/*
 * The board's part of the RAM log (shell/common/compat/port/log_ram_os.h,
 * read only by shell/common/compat/log/log_ram.c):
 *
 * - the ring lives in .noinit, which _start neither clears nor loads
 *   (ldscript/xiao_esp32s3.ld), so it survives "reboot" and the RESET
 *   button but not a power cycle;
 * - the lock is the ThreadX port's interrupt lockout, usable in interrupt
 *   handlers and before the kernel runs;
 * - the clock is the board's uptime;
 * - there is no other log to attach to.
 */
#ifndef SHELL_PORT_LOG_RAM_OS_IMPL_H
#define SHELL_PORT_LOG_RAM_OS_IMPL_H

#include <stdint.h>

#include "tx_api.h"

#include "board.h"

#define LOG_RAM_OS_RETAINED(name) name __attribute__((section(".noinit.log_ram")))

typedef UINT log_ram_os_irqstate_t;

static inline log_ram_os_irqstate_t log_ram_os_irq_save(void)
{
    return XTOS_SET_INTLEVEL(XCHAL_EXCM_LEVEL);
}

static inline void log_ram_os_irq_restore(log_ram_os_irqstate_t state)
{
    XTOS_RESTORE_INTLEVEL(state);
}

static inline uint64_t log_ram_os_uptime_us(void)
{
    return board_uptime_us();
}

static inline void log_ram_os_attach(void)
{
}

#endif /* SHELL_PORT_LOG_RAM_OS_IMPL_H */
