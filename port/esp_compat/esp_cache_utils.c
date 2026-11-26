/*
 * Stand-in for the cache freeze of ESP-IDF's esp_mm (esp_cache_utils.c, not
 * built), which its MMU driver (esp_mmu_map.c) brackets every change of the
 * MMU table with: the cache is frozen (an access that misses waits) while
 * an entry is written and the range invalidated, which takes a few
 * microseconds.
 *
 * ESP-IDF's version masks only the interrupts whose handlers the interrupt
 * allocator knows to be outside RAM (esp_intr_noniram_disable()).  Here the
 * tick, the console's interrupts and the Wi-Fi MAC's are routed outside the
 * allocator, with their handlers in flash, so this masks every interrupt up
 * to XCHAL_EXCM_LEVEL, as the flash driver's windows do
 * (port/board_flash_os.c): no handler and no other thread runs until the
 * cache is unfrozen.  The levels above stay open: the interrupt watchdog
 * and the cache error interrupt, both handled in RAM.  The calls do not
 * nest (the MMU driver never nests them), and are made from threads only.
 *
 * This file runs from RAM (see the linker script) and calls only the cache
 * HAL, which is in RAM too.
 */
#include <stdint.h>

#include "hal/cache_hal.h"
#include "hal/cache_ll.h"
#include "esp_private/esp_cache_private.h"
#include "xtensa/config/core.h"
#include "xtensa/xtruntime.h"

/* The PS to restore at the unfreeze.  Written with interrupts masked. */
static uint32_t freeze_ps;

void esp_cache_freeze_caches_disable_interrupts(void)
{
    freeze_ps = XTOS_SET_MIN_INTLEVEL(XCHAL_EXCM_LEVEL);
    cache_hal_freeze(CACHE_LL_LEVEL_EXT_MEM, CACHE_TYPE_ALL);
}

void esp_cache_unfreeze_caches_enable_interrupts(void)
{
    cache_hal_unfreeze(CACHE_LL_LEVEL_EXT_MEM, CACHE_TYPE_ALL);
    XTOS_RESTORE_INTLEVEL(freeze_ps);
}
