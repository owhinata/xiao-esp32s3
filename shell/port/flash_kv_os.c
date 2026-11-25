/* SPDX-License-Identifier: MIT */
/*
 * The OS's part of the key-value area (shell/common/compat/port/flash_kv_os.h)
 * on this board: the "settings" partition of partitions.csv, which
 * flash_kv.h names as FLASH_KV_OFFSET and FLASH_KV_SIZE, read, programmed
 * and erased through the flash driver (port/board_flash.h).  The copies of
 * Zephyr's NVS and settings keep the store there in the Zephyr build's
 * format through the flash device of shell/common/compat/zephyr_flash_kv.c,
 * which calls these functions.  The counterpart of rp2040-connect's
 * common/chip/rp2040/rp2040_flash_kv.c.
 *
 * Safety: an erase or a program outside the area could destroy the
 * bootloader, the partition table or the firmware.  Every request is
 * checked three times before the flash is touched: in zephyr_flash_kv.c
 * (against the constants of flash_kv.h), here (against the constants and
 * against the bounds that flash_kv_os_initialize() sets once the partition
 * table on the board has been found to agree), and by the flash driver's
 * region check (port/board_flash_os.c: the partition's offset and size from
 * the partition table at configure time, and only once the table on the
 * board agrees).  The build checks that flash_kv.h and partitions.csv name
 * the same area.  Nothing here erases at the start.
 *
 * A page to program is 0xff around the data (flash_kv_write_pages()):
 * programming 0xff leaves a byte as it is, so only the span from the first
 * to the last byte that is not 0xff is written, which keeps the window with
 * the cache off short (an 8-byte entry of NVS is one program command of
 * the chip instead of four).  Reads go through the driver (SPI1), never
 * through the cache: they show what the flash holds right after a program
 * or an erase.  This file runs with the cache on and may be in the flash.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include "app_log.h"
#include "board_flash.h"
#include "flash_kv.h"
#include "flash_kv_os.h"

LOG_MODULE_REGISTER(flash_kv, LOG_LEVEL_INF);

_Static_assert(FLASH_KV_OFFSET == BOARD_SETTINGS_OFFSET && FLASH_KV_SIZE == BOARD_SETTINGS_SIZE,
               "flash_kv.h and the \"settings\" partition of partitions.csv differ");

/* The area's bounds as flash offsets once the table has been checked; 0 and 0 refuse everything. */
static uint32_t kv_lo;
static uint32_t kv_hi;

int flash_kv_os_initialize(void)
{
    kv_lo = 0;
    kv_hi = 0;
    if (!board_flash_settings_ok()) {
        LOG_ERR("Key-value area 0x%06x-0x%06x is not the \"settings\" partition of the partition "
                "table on the board (flash the partition table: the \"flash\" target)",
                (unsigned int) FLASH_KV_OFFSET, (unsigned int) (FLASH_KV_END - 1u));
        return -ENODEV;
    }
    kv_lo = FLASH_KV_OFFSET;
    kv_hi = FLASH_KV_END;
    return 0;
}

bool flash_kv_os_ready(void)
{
    return kv_lo != 0;
}

/* A program (page_offset, 256) or erase (sector_offset, 4096) request inside the area and the bounds. */
static bool kv_request_ok(uint32_t offset, uint32_t count, const char *what)
{
    if (flash_kv_range_ok(offset, count, FLASH_KV_OFFSET, FLASH_KV_END, count)
        && flash_kv_range_ok(offset, count, kv_lo, kv_hi, count)) {
        return true;
    }
    LOG_ERR("Refused flash %s at 0x%08lx, %lu bytes", what, (unsigned long) offset,
            (unsigned long) count);
    return false;
}

int flash_kv_os_read(uint32_t offset, void *buf, uint32_t len)
{
    if (!flash_kv_range_ok(offset, len, FLASH_KV_OFFSET, FLASH_KV_END, 1)) {
        LOG_ERR("Refused flash read at 0x%08lx, %lu bytes", (unsigned long) offset,
                (unsigned long) len);
        return -EINVAL;
    }
    return board_flash_read(offset, buf, len);
}

int flash_kv_os_program(uint32_t page_offset, const uint8_t page[FLASH_KV_PAGE_SIZE])
{
    uint32_t first = 0;
    uint32_t end = FLASH_KV_PAGE_SIZE;

    if (!kv_request_ok(page_offset, FLASH_KV_PAGE_SIZE, "write")) {
        return -EINVAL;
    }
    while (first < end && page[first] == FLASH_KV_ERASE_VALUE) {
        first++;
    }
    while (end > first && page[end - 1u] == FLASH_KV_ERASE_VALUE) {
        end--;
    }
    if (first == end) {
        return 0;
    }
    return board_flash_write(page_offset + first, page + first, end - first);
}

int flash_kv_os_erase(uint32_t sector_offset)
{
    if (!kv_request_ok(sector_offset, FLASH_KV_SECTOR_SIZE, "erase")) {
        return -EINVAL;
    }
    return board_flash_erase(sector_offset, FLASH_KV_SECTOR_SIZE);
}
