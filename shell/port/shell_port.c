/* SPDX-License-Identifier: MIT */
/*
 * The functions the shared shell sources call by name that only the board
 * can provide: the device ID of <zephyr/drivers/hwinfo.h> and the layer's
 * tick.
 */
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

#include "tx_api.h"
#include "tx_timer.h"

#include <zephyr/drivers/hwinfo.h>

#include "board.h"
#include "zephyr_compat_os.h"

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

/*
 * The layer counts its own ticks for the timers of delayable work items and
 * wants one zephyr_compat_tick() per kernel tick, from the tick interrupt.
 *
 * _tx_timer_interrupt (port/threadx/src/tx_timer_interrupt.S) calls this
 * hook once per interrupt, before it counts the tick; when it was held off
 * for longer than a tick it then counts several.  So the hook delivers the
 * tick about to be counted plus whatever the previous interrupt counted
 * beyond its one, read off ThreadX's own tick count.
 */
void tx_timer_user_isr(void);

void tx_timer_user_isr(void)
{
    static ULONG delivered;
    ULONG due = _tx_timer_system_clock + 1u - delivered;

    delivered += due;
    while (due-- > 0u) {
        zephyr_compat_tick();
    }
}
