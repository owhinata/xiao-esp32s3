/* SPDX-License-Identifier: MIT */
/*
 * The functions the shared shell sources call by name that only the board
 * can provide: k_uptime_get() of <zephyr/kernel.h> (rp2040-connect defines
 * it in its compat/zephyr_init.c, with the work queues, which this board
 * does not use) and the device ID of <zephyr/drivers/hwinfo.h>.
 *
 * The layer's tick, zephyr_compat_tick(), is not called: it runs the
 * timeouts that z_add_timeout() adds (delayable work, k_timer), and nothing
 * linked here adds one.
 */
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>

#include "board.h"

/* Milliseconds since boot, on the layer's cycle counter (the SYSTIMER uptime in microseconds). */
int64_t k_uptime_get(void)
{
    return Z_UPTIME_MS(k_cycle_get_64());
}

/* "id": the factory MAC address, 6 bytes. */
ssize_t hwinfo_get_device_id(uint8_t *buffer, size_t length)
{
    uint8_t id[BOARD_DEVICE_ID_SIZE];

    board_device_id(id);
    if (length > sizeof(id)) {
        length = sizeof(id);
    }
    memcpy(buffer, id, length);
    return (ssize_t) length;
}
