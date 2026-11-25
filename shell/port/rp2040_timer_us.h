/* SPDX-License-Identifier: MIT */
/*
 * The board's microsecond counter under the name the copy of
 * rp2040-connect's time synchronization (shell/common/imu/time_sync.c)
 * reads it by: rp2040_timer_us() of rp2040-connect's
 * common/chip/rp2040/include/rp2040_timer_us.h, the RP2040 TIMER there.
 * This board has no such timer.  Here it is the uptime clock
 * (board_uptime_us(): SYSTIMER counter 0, from the crystal), which is also
 * the layer's cycle counter
 * (k_cycle_get_64()) and the base of the realtime clock
 * (zephyr_sys_clock.c), so the time synchronization's monotonic clock and
 * those of the layer are one clock, as on the RP2040.  The header has the
 * RP2040 name, as shell/port/nina_wifi.h has the NINA's, so that the copy
 * compiles unchanged (shell/port is first on the include path).  Any
 * context, as board_uptime_us().
 */
#ifndef SHELL_PORT_RP2040_TIMER_US_H
#define SHELL_PORT_RP2040_TIMER_US_H

#include <stdint.h>

#include "board.h"

static inline uint64_t rp2040_timer_us(void)
{
    return board_uptime_us();
}

#endif /* SHELL_PORT_RP2040_TIMER_US_H */
