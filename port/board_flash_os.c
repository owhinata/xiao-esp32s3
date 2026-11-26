/*
 * The flash driver's OS functions (esp_flash_os_functions_t of ESP-IDF's
 * esp_flash), and what the fault path needs of them.
 *
 * esp_flash brackets every access to the flash chip through SPI1 with
 * start() and end(): a read chunk, one program chunk
 * (CONFIG_SPI_FLASH_WRITE_CHUNK_SIZE), one sector erase
 * (CONFIG_SPI_FLASH_BYPASS_BLOCK_ERASE), a register access.  In between,
 * SPI0, through which the cache fetches the code and read-only data in
 * flash, must not touch the chip.  ESP-IDF's own table for that case
 * (esp_flash_noos_functions, spi_flash_os_func_noos.c) suspends and resumes
 * the cache; ESP-IDF's application table adds a scheduler lock, masks the
 * interrupts whose handlers are not in RAM and stops the other core.  Here
 * the window masks every interrupt up to XCHAL_EXCM_LEVEL, everything
 * ThreadX and the drivers handle in C, and then suspends the cache with
 * ESP-IDF's no-OS start(): no interrupt handler and no other thread can run
 * until end(), so none of them has to be in RAM.  The levels above stay
 * open: the interrupt watchdog and the cache error interrupt (level 4),
 * both handled in RAM.
 *
 * The longest window is one sector erase (typically some tens of ms, the
 * driver's timeout 600 ms).  The tick, which feeds the interrupt watchdog,
 * is masked meanwhile, so the window feeds it: once at start() and in
 * every delay_us(), which the driver calls in each of its polling loops
 * (every 1 or 20 us).  A chip that does not finish therefore ends in the
 * driver's ESP_ERR_TIMEOUT, not in the watchdog; a controller that hangs in
 * one of the HAL's tight loops, which do not delay, still ends in the
 * watchdog.  After end() the tick catches up the ticks it missed.
 *
 * region_protected() lets a write or erase through only inside one of the
 * regions that port/board_flash.c has opened (board_flash_os.h): the
 * "settings" partition, otadata and, during an update, the OTA slot that
 * is not running.  Nothing is open until the partition
 * table in flash has been found to agree with the build; port/board_flash.c
 * never opens a region in the bootloader, the partition table or the
 * running application.
 *
 * board_flash_fault(): a fault inside a window (an exception, an assertion
 * of the driver, the cache error interrupt, the interrupt watchdog) enters
 * the fault path with the cache still suspended, and the chip perhaps
 * still erasing.  Once the first record is sealed (port/panic.c), the
 * fault path waits for SPI1 and the chip to be idle and resumes the cache
 * before it touches anything in flash.
 *
 * This file and its read-only data run from RAM (see the linker script),
 * and call only RAM and ROM: no logging, no C library beyond the ROM's, no
 * ThreadX service.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_flash_chips/esp_flash_types.h"
#include "esp_rom_spiflash.h"
#include "esp_rom_sys.h"
#include "hal/cache_hal.h"
#include "hal/cache_ll.h"
#include "hal/spimem_flash_ll.h"
#include "soc/spi_mem_struct.h"
#include "xtensa/config/core.h"
#include "xtensa/xtruntime.h"

#include "board.h"
#include "board_flash_os.h"

/* ESP-IDF's no-OS table (spi_flash_os_func_noos.c): the cache suspend and resume. */
extern const esp_flash_os_functions_t esp_flash_noos_functions;

/* Inside a window: the PS to restore at its end.  Written with interrupts masked. */
static volatile bool window_open;
static uint32_t window_ps;

/* The open regions, [start, end); start == end is closed.  Written with interrupts masked. */
static struct {
    uint32_t start;
    uint32_t end;
} regions[BOARD_FLASH_REGION_COUNT];

static esp_err_t window_start(void *arg, uint32_t flags)
{
    uint32_t ps = XTOS_SET_MIN_INTLEVEL(XCHAL_EXCM_LEVEL);

    /* The driver never nests a window ("Does not need to be recursive"). */
    if (window_open) {
        XTOS_RESTORE_INTLEVEL(ps);
        return ESP_ERR_INVALID_STATE;
    }
    window_ps = ps;
    window_open = true;
    board_watchdog_feed();
    return esp_flash_noos_functions.start(arg, flags);
}

static esp_err_t window_end(void *arg)
{
    esp_err_t err = esp_flash_noos_functions.end(arg);
    uint32_t ps = window_ps;

    window_open = false;
    XTOS_RESTORE_INTLEVEL(ps);
    return err;
}

static esp_err_t window_delay_us(void *arg, uint32_t us)
{
    (void) arg;
    esp_rom_delay_us(us);
    board_watchdog_feed();
    return ESP_OK;
}

/*
 * ESP_OK if [start, start + size) lies inside one open region.  No sum can
 * wrap; an empty request inside a region is let through, as the driver
 * does nothing for it.  Called by the driver before it opens a window.
 */
static esp_err_t window_region_protected(void *arg, size_t start_addr, size_t size)
{
    (void) arg;
    for (unsigned int i = 0; i < BOARD_FLASH_REGION_COUNT; i++) {
        uint32_t start = regions[i].start;
        uint32_t end = regions[i].end;

        if (start_addr >= start && start_addr < end && size <= end - start_addr) {
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_ALLOWED;
}

/*
 * No temporary buffers (buffers must be in internal RAM), no yield (an
 * erase is one sector per window, and the caller's thread is preempted
 * between windows), no operation status (no flash brownout handling).
 */
const esp_flash_os_functions_t board_flash_os_functions = {
    .start = window_start,
    .end = window_end,
    .region_protected = window_region_protected,
    .delay_us = window_delay_us,
};

void board_flash_os_set_region(enum board_flash_region region, uint32_t offset, uint32_t size)
{
    uint32_t ps = XTOS_SET_MIN_INTLEVEL(XCHAL_EXCM_LEVEL);

    regions[region].start = offset;
    regions[region].end = offset + size;
    XTOS_RESTORE_INTLEVEL(ps);
}

void board_flash_fault(void)
{
    if (!window_open) {
        return;
    }
    while (!spimem_flash_ll_host_idle(&SPIMEM1)) {
    }
    (void) esp_rom_spiflash_wait_idle(&g_rom_flashchip);
    cache_hal_resume(CACHE_LL_LEVEL_EXT_MEM, CACHE_TYPE_ALL);
    window_open = false;
}
