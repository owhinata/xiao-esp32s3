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
 * no erase is let through (port/board_flash_os.c).  With "settings" found,
 * the otadata partition of the same table is opened too, where the OTA
 * code (app_update) records the slot to boot, once it is checked: two whole
 * sectors of the chip, not below the first 64 KiB (the bootloader, the
 * table and "settings") and not in the application partition the image
 * runs from (the one the MMU maps this code from).  Nothing else is opened
 * here; the OTA slots stay closed.
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
#include "esp_private/flash_mmap.h"
#include "spi_flash_mmap.h"
#include "hal/mmu_hal.h"
#include "hal/mmu_types.h"

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

/*
 * Below this only "settings" is ever opened: the bootloader, the partition
 * table and "settings" fill the first 64 KiB (the first MMU page, where no
 * application partition can start).
 */
#define BOOT_AREA_END   0x10000u
#define OTADATA_SIZE    (2u * SPI_FLASH_SEC_SIZE)

_Static_assert(BOARD_SETTINGS_OFFSET >= CONFIG_PARTITION_TABLE_OFFSET + ESP_PARTITION_TABLE_MAX_LEN
                   && BOARD_SETTINGS_OFFSET + BOARD_SETTINGS_SIZE <= BOOT_AREA_END,
               "\"settings\" lies between the partition table and the first application page");

/* Whether table (count entries, checked) has the build's "settings". */
static bool settings_in_table(const esp_partition_info_t *table, int count)
{
    static const char label[] = "settings";

    _Static_assert(sizeof(label) <= sizeof(table->label), "the label fits");
    for (int i = 0; i < count; i++) {
        const esp_partition_info_t *p = &table[i];

        if (p->magic == ESP_PARTITION_MAGIC && memcmp(p->label, label, sizeof(label)) == 0) {
            return p->type == BOARD_SETTINGS_TYPE && p->subtype == BOARD_SETTINGS_SUBTYPE
                   && p->pos.offset == BOARD_SETTINGS_OFFSET && p->pos.size == BOARD_SETTINGS_SIZE;
        }
    }
    return false;
}

/* The table's entry of the first partition of that type and subtype, or NULL. */
static const esp_partition_info_t *table_find(const esp_partition_info_t *table, int count,
                                              uint8_t type, uint8_t subtype)
{
    for (int i = 0; i < count; i++) {
        if (table[i].magic == ESP_PARTITION_MAGIC && table[i].type == type && table[i].subtype == subtype) {
            return &table[i];
        }
    }
    return NULL;
}

/* The application partition that holds the flash page this code is mapped from, or NULL. */
static const esp_partition_info_t *table_running(const esp_partition_info_t *table, int count)
{
    uint32_t paddr;
    mmu_target_t target;

    if (!mmu_hal_vaddr_to_paddr(0, (uint32_t) &table_running, &paddr, &target) || target != MMU_TARGET_FLASH0) {
        return NULL;
    }
    for (int i = 0; i < count; i++) {
        const esp_partition_info_t *p = &table[i];

        if (p->magic == ESP_PARTITION_MAGIC && p->type == PART_TYPE_APP && paddr >= p->pos.offset
            && paddr - p->pos.offset < p->pos.size) {
            return p;
        }
    }
    return NULL;
}

/* Whether p may be opened for writes: see the top. */
static bool otadata_usable(const esp_partition_info_t *p, const esp_partition_info_t *running, uint32_t chip_size)
{
    uint32_t start = p->pos.offset;
    uint32_t size = p->pos.size;

    return size == OTADATA_SIZE && start % SPI_FLASH_SEC_SIZE == 0 && start >= BOOT_AREA_END
           && start <= chip_size && size <= chip_size - start
           && (start + size <= running->pos.offset || start >= running->pos.offset + running->pos.size);
}

/*
 * Reads and checks the partition table in flash, and opens the regions it
 * finds (see the top).  Returns whether "settings" was found.
 */
static bool table_check(uint32_t chip_size)
{
    esp_partition_info_t *table = heap_caps_malloc(ESP_PARTITION_TABLE_MAX_LEN, MALLOC_CAP_INTERNAL);
    const esp_partition_info_t *otadata;
    const esp_partition_info_t *running;
    int count;
    bool found = false;

    if (table == NULL) {
        return false;
    }
    if (esp_flash_read(NULL, table, CONFIG_PARTITION_TABLE_OFFSET, ESP_PARTITION_TABLE_MAX_LEN) == ESP_OK
        && esp_partition_table_verify(table, false, &count) == ESP_OK && settings_in_table(table, count)) {
        found = true;
        board_flash_os_set_region(BOARD_FLASH_REGION_SETTINGS, BOARD_SETTINGS_OFFSET, BOARD_SETTINGS_SIZE);
        otadata = table_find(table, count, PART_TYPE_DATA, PART_SUBTYPE_DATA_OTA);
        running = table_running(table, count);
        if (otadata != NULL && running != NULL && otadata_usable(otadata, running, chip_size)) {
            board_flash_os_set_region(BOARD_FLASH_REGION_OTADATA, otadata->pos.offset, otadata->pos.size);
        } else {
            ESP_EARLY_LOGE(TAG, "No usable otadata partition in the partition table at 0x%x; "
                           "it stays closed", CONFIG_PARTITION_TABLE_OFFSET);
        }
    }
    heap_caps_free(table);
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

    settings_found = table_check(chip->size);
    if (!settings_found) {
        ESP_EARLY_LOGE(TAG, "No \"settings\" partition at 0x%x (0x%x bytes) in the partition table "
                       "at 0x%x; the store stays closed", BOARD_SETTINGS_OFFSET, BOARD_SETTINGS_SIZE,
                       CONFIG_PARTITION_TABLE_OFFSET);
    }
    return ESP_OK;
}

/*
 * The lock of ESP-IDF's spi_flash_mmap() (flash_mmap.c), which ESP-IDF
 * creates in the flash driver's start-up (esp_flash_app_init_os_functions()).
 * Its semaphores are FreeRTOS objects, which the FreeRTOS layer makes from
 * tx_application_define() on: here one stage later, before any thread can
 * map flash (the OTA code).  Nothing takes the lock's freeze: in ESP-IDF
 * the OS functions of a write or an erase do, to wait for the mappings that
 * block writes; port/board_flash_os.c does not.  Those mappings (the
 * partition table, otadata, an image while it is checked) are read and
 * unmapped by the code that made them, and otadata and the slots are
 * written only by that same code, between its mappings.  A write to a
 * mapped page (the store, in the partition table's page) invalidates the
 * cache of the mapping (spi_flash_check_and_flush_cache(), flash_mmap.c).
 */
ESP_SYSTEM_INIT_FN(board_flash_mmap_lock, SECONDARY, BIT(0), 130)
{
    return flash_mmap_lock_init();
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
