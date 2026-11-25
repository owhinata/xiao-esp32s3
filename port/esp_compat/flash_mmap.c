/*
 * Stand-in for the one function of ESP-IDF's flash_mmap.c (spi_flash; not
 * built: it maps flash on request through esp_mm's MMU driver, which this
 * firmware does not use) that the compiled sources call:
 * memspi_host_flush_cache(), the flash driver's host, after every write
 * and erase, with the cache still suspended.
 *
 * Code or data cached from the flash range that was just changed would be
 * stale.  As ESP-IDF's does, this looks for MMU entries that map a 64 KiB
 * page of the range, and invalidates the cache for the virtual addresses of
 * each: on this chip an entry maps the same page into the instruction and
 * the data window.  The only mapping here is the application image's (from
 * 0x10000; the second-stage bootloader drops its own before it starts the
 * application), and the store's partition lies below it, so normally
 * nothing is found and nothing is invalidated.
 *
 * Runs with the cache suspended: from RAM (see the linker script), calling
 * only the MMU's inline accessors and the cache HAL (RAM, ROM).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sdkconfig.h"
#include "esp_rom_spiflash.h"
#include "hal/cache_hal.h"
#include "hal/mmu_ll.h"
#include "hal/mmu_types.h"
#include "soc/soc_caps.h"
#include "esp_private/cache_utils.h"

bool spi_flash_check_and_flush_cache(size_t start_addr, size_t length)
{
    const uint32_t page = CONFIG_MMU_PAGE_SIZE;
    uint32_t first = (uint32_t) start_addr & ~(page - 1u);
    uint32_t end = (uint32_t) start_addr + (uint32_t) length;
    bool flushed = false;

    if (length == 0 || end < start_addr || end > g_rom_flashchip.chip_size) {
        return false;
    }
    for (uint32_t id = 0; id < SOC_MMU_ENTRY_NUM; id++) {
        uint32_t paddr;

        if (!mmu_ll_check_entry_valid(0, id) || mmu_ll_get_entry_target(0, id) != MMU_TARGET_FLASH0) {
            continue;
        }
        paddr = mmu_ll_entry_id_to_paddr_base(0, id);
        if (paddr < first || paddr >= end) {
            continue;
        }
        (void) cache_hal_invalidate_addr(mmu_ll_entry_id_to_vaddr_base(0, id, MMU_VADDR_INSTRUCTION), page);
        (void) cache_hal_invalidate_addr(mmu_ll_entry_id_to_vaddr_base(0, id, MMU_VADDR_DATA), page);
        flushed = true;
    }
    return flushed;
}
