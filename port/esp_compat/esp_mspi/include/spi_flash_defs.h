/*
 * Stand-in for "spi_flash_defs.h" of ESP-IDF's esp_mspi component, which
 * esp-hal-3rdparty does not carry (its spi_flash/include/esp_flash_chips/
 * copy only includes this one).
 *
 * Its only user here is bootloader_flash_config_esp32s3.c, of which
 * flash_map_image() calls the pad, dummy and chip-select set-up.  The one
 * command that file names, with upstream's value, belongs to
 * bootloader_init_spi_flash(), which is not linked.
 */
#ifndef ESP_COMPAT_SPI_FLASH_DEFS_H
#define ESP_COMPAT_SPI_FLASH_DEFS_H

#define CMD_RESUME              0x7A    /* clear the flash's suspend bit */

#endif
