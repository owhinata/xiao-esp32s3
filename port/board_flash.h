/*
 * The flash, through ESP-IDF's esp_flash driver (port/board_flash.c,
 * port/board_flash_os.c), for the persistent store.  Only the "settings"
 * partition of partitions.csv, the otadata partition and, while an update
 * writes it, the OTA slot that is not running (both written by ESP-IDF's
 * OTA code through the driver) can be written or erased; the
 * build passes the offset and size of "settings", read from the partition
 * table at configure time, as BOARD_SETTINGS_OFFSET and BOARD_SETTINGS_SIZE
 * to the sources that need them.
 *
 * No Espressif type appears here: the shell's side of the store includes
 * this header and must not see sdkconfig.h.
 */
#ifndef BOARD_FLASH_H
#define BOARD_FLASH_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Whether the partition table in flash (at CONFIG_PARTITION_TABLE_OFFSET) is
 * valid and has the "settings" partition exactly where the build expects
 * it, with the same type and subtype.  Checked once at start-up; false also
 * when the flash driver did not start.  While false, writes and erases are
 * refused.
 */
bool board_flash_settings_ok(void);

/*
 * Read, program and erase the flash at flash offsets, from threads or
 * before the kernel runs, not from interrupt handlers.  buf must be in
 * internal RAM.  An erase takes whole 4 KiB sectors.  Every interrupt up
 * to XCHAL_EXCM_LEVEL is masked for one read chunk (up to 16 KiB), one
 * 256-byte program chunk or one sector erase at a time, and the chip is
 * idle at the end of each: calls of different threads may interleave
 * between those steps.  0, or a negative errno: -EINVAL for a request
 * outside the flash, a write or erase outside the partitions that are
 * open (nothing is touched) or a buffer outside internal RAM, -ENODEV when the
 * driver (reads) or the partition table (writes, erases) is not usable,
 * -ETIMEDOUT when the chip did not finish in the driver's time, -EIO for
 * any other failure.
 */
int board_flash_read(uint32_t offset, void *buf, uint32_t len);
int board_flash_write(uint32_t offset, const void *buf, uint32_t len);
int board_flash_erase(uint32_t offset, uint32_t len);

/*
 * Opens writes and erases to the OTA slot [offset, offset + size) for an
 * update (port/ota_slots.c), or closes them again.  The range must be
 * whole sectors inside the chip, above the first 64 KiB, and must not
 * overlap the application partition this image runs from or otadata (as
 * the start-up found them in the partition table): -EINVAL otherwise,
 * -ENODEV when the partition table was not usable.  One slot at a time;
 * opening replaces the slot that was open.  From threads.
 */
int board_flash_ota_slot_open(uint32_t offset, uint32_t size);
void board_flash_ota_slot_close(void);

#endif
