/*
 * The flash: ESP-IDF's esp_flash driver on SPI1 (spi_flash: esp_flash_api.c,
 * the chip drivers, memspi_host_driver.c; esp_hal_mspi's HAL), for the
 * persistent store (board_flash.h).
 *
 * Start-up: the driver is set up where ESP-IDF's start-up does it, as a
 * start-up hook of the CORE stage at priority 130 (ESP-IDF's init_flash in
 * esp_flash_spi_init.c, whose own hook is left out by the linker script: it
 * installs ESP-IDF's OS functions, which need the scheduler lock, the
 * interrupt allocator's IRAM bookkeeping and the cache and mmap drivers).
 * Espressif's esp_flash_init_default_chip() runs with ESP-IDF's no-OS
 * functions, as in ESP-IDF, then the chip gets this board's
 * (port/board_flash_os.c).  The chip is identified (its JEDEC ID, the
 * driver for its vendor, its size, which must not be smaller than the
 * image header's 8 MB) and named in one line on the console.  A driver
 * that does not start leaves the flash unused rather than stopping the
 * boot (ESP-IDF asserts): the firmware runs without its store.
 *
 * Then the partition table in flash (at CONFIG_PARTITION_TABLE_OFFSET) is
 * read and checked with the second-stage bootloader's own check
 * (esp_partition_table_verify(), with its MD5), and the "settings"
 * partition looked up in it: name, type, subtype, offset and size must be
 * the ones partitions.csv gave the build.  A board that still has another
 * table (app-flash does not write it) keeps the store closed: no write and
 * no erase is let through (port/board_flash_os.c).
 *
 * This file runs with the cache on (it is in flash); everything it calls
 * that runs with the cache off is in RAM.
 */
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_private/esp_flash_internal.h"
#include "esp_flash_chips/spi_flash_chip_driver.h"
#include "esp_flash_partitions.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/startup_internal.h"

#include "board_flash.h"
#include "board_flash_os.h"

static const char TAG[] = "flash";

static bool driver_started;
static bool settings_found;

/* esp_flash_io_mode_t's names, as ESP-IDF's log prints them. */
static const char *io_mode_name(esp_flash_io_mode_t mode)
{
    static const char *const names[] = {
        [SPI_FLASH_SLOWRD] = "slowrd", [SPI_FLASH_FASTRD] = "fastrd", [SPI_FLASH_DOUT] = "dout",
        [SPI_FLASH_DIO] = "dio",       [SPI_FLASH_QOUT] = "qout",     [SPI_FLASH_QIO] = "qio",
    };

    return (unsigned) mode < sizeof(names) / sizeof(names[0]) && names[mode] != NULL ? names[mode] : "?";
}

/* Whether table (ESP_PARTITION_TABLE_MAX_LEN bytes) is valid and has the build's "settings". */
static bool settings_in_table(const esp_partition_info_t *table)
{
    static const char label[] = "settings";
    int count;

    _Static_assert(sizeof(label) <= sizeof(table->label), "the label fits");
    if (esp_partition_table_verify(table, false, &count) != ESP_OK) {
        return false;
    }
    for (int i = 0; i < count; i++) {
        const esp_partition_info_t *p = &table[i];

        if (p->magic == ESP_PARTITION_MAGIC && memcmp(p->label, label, sizeof(label)) == 0) {
            return p->type == BOARD_SETTINGS_TYPE && p->subtype == BOARD_SETTINGS_SUBTYPE
                   && p->pos.offset == BOARD_SETTINGS_OFFSET && p->pos.size == BOARD_SETTINGS_SIZE;
        }
    }
    return false;
}

static bool settings_check(void)
{
    esp_partition_info_t *table = heap_caps_malloc(ESP_PARTITION_TABLE_MAX_LEN, MALLOC_CAP_INTERNAL);
    bool found = false;

    if (table != NULL) {
        found = esp_flash_read(NULL, table, CONFIG_PARTITION_TABLE_OFFSET, ESP_PARTITION_TABLE_MAX_LEN)
                    == ESP_OK
                && settings_in_table(table);
        heap_caps_free(table);
    }
    return found;
}

ESP_SYSTEM_INIT_FN(board_flash, CORE, BIT(0), 130)
{
    esp_flash_t *chip;
    esp_err_t err = esp_flash_init_default_chip();

    if (err != ESP_OK) {
        ESP_EARLY_LOGE(TAG, "The flash driver did not start (0x%x); no store", (unsigned) err);
        return ESP_OK;
    }
    chip = esp_flash_default_chip;
    chip->os_func = &board_flash_os_functions;
    driver_started = true;
    ESP_EARLY_LOGI(TAG, "ID 0x%06" PRIx32 " (%s), %" PRIu32 " MB, %s", chip->chip_id,
                   chip->chip_drv->name, chip->size >> 20, io_mode_name(chip->read_mode));

    settings_found = settings_check();
    if (settings_found) {
        board_flash_os_allow_settings();
    } else {
        ESP_EARLY_LOGE(TAG, "No \"settings\" partition at 0x%x (0x%x bytes) in the partition table "
                       "at 0x%x; the store stays closed", BOARD_SETTINGS_OFFSET, BOARD_SETTINGS_SIZE,
                       CONFIG_PARTITION_TABLE_OFFSET);
    }
    return ESP_OK;
}

bool board_flash_settings_ok(void)
{
    return settings_found;
}

static int to_errno(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return 0;
    case ESP_ERR_INVALID_ARG:
    case ESP_ERR_NOT_ALLOWED:
    case ESP_ERR_NO_MEM:            /* the buffer is not in internal RAM */
        return -EINVAL;
    case ESP_ERR_TIMEOUT:
        return -ETIMEDOUT;
    default:
        return -EIO;
    }
}

int board_flash_read(uint32_t offset, void *buf, uint32_t len)
{
    if (!driver_started) {
        return -ENODEV;
    }
    return to_errno(esp_flash_read(NULL, buf, offset, len));
}

int board_flash_write(uint32_t offset, const void *buf, uint32_t len)
{
    if (!settings_found) {
        return -ENODEV;
    }
    return to_errno(esp_flash_write(NULL, buf, offset, len));
}

int board_flash_erase(uint32_t offset, uint32_t len)
{
    if (!settings_found) {
        return -ENODEV;
    }
    return to_errno(esp_flash_erase_region(NULL, offset, len));
}
