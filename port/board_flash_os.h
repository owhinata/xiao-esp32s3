/*
 * Between port/board_flash.c (the driver's start-up and the calls, in
 * flash) and port/board_flash_os.c (its OS functions, in RAM).
 */
#ifndef BOARD_FLASH_OS_H
#define BOARD_FLASH_OS_H

#include <stdint.h>

#include "esp_flash.h"
#include "esp_flash_chips/esp_flash_types.h"

/* The table that esp_flash_default_chip uses once the driver has started. */
extern const esp_flash_os_functions_t board_flash_os_functions;

/*
 * The regions that writes and erases may go to, each one range of the
 * flash or closed (all are closed at start-up):
 *   SETTINGS  the "settings" partition, the persistent store
 *   OTADATA   the "otadata" partition, which records the OTA slot to boot
 *   OTA_SLOT  the OTA slot that is not running, while an update writes it
 *             (no code opens it: the update's transfer is not built)
 */
enum board_flash_region {
    BOARD_FLASH_REGION_SETTINGS,
    BOARD_FLASH_REGION_OTADATA,
    BOARD_FLASH_REGION_OTA_SLOT,
    BOARD_FLASH_REGION_COUNT
};

/*
 * From then on writes and erases inside [offset, offset + size) are let
 * through (size 0 closes the region).  The caller has checked the range
 * (port/board_flash.c): this only records it.
 */
void board_flash_os_set_region(enum board_flash_region region, uint32_t offset, uint32_t size);

#endif
