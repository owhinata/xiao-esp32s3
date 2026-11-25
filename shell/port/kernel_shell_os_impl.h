/* SPDX-License-Identifier: MIT */
/*
 * The board's part of the generic shell commands
 * (shell/common/shell/kernel_shell.c reads this file through
 * shell/common/shell/port/kernel_shell_os.h, where each item is described).
 */
#ifndef SHELL_PORT_KERNEL_SHELL_OS_IMPL_H
#define SHELL_PORT_KERNEL_SHELL_OS_IMPL_H

#include <stddef.h>
#include <stdint.h>

#include "tx_api.h"

#include "board.h"

static inline uint32_t kernel_shell_os_tick_count(void)
{
    return (uint32_t) tx_time_get();
}

static inline void kernel_shell_os_restart_delay(uint32_t delay_ms)
{
    (void) tx_thread_sleep((ULONG) ((uint64_t) delay_ms * TX_TIMER_TICKS_PER_SECOND / 1000u));
}

static inline void kernel_shell_os_reboot(uint32_t delay_ms)
{
    kernel_shell_os_restart_delay(delay_ms);
    board_restart();
}

/* "bootsel": this chip's counterpart of the USB loader is the ROM download mode. */
static inline void kernel_shell_os_bootsel(uint32_t delay_ms)
{
    kernel_shell_os_restart_delay(delay_ms);
    board_restart_to_download();
}

/*
 * No record is kept across a fatal error: the fault handlers print a
 * register dump and halt (port/panic.c).
 */
static inline const struct fault_record *kernel_shell_os_fault_last(void)
{
    return NULL;
}

#endif /* SHELL_PORT_KERNEL_SHELL_OS_IMPL_H */
