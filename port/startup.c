/*
 * Reset entry for the RAM-only image.
 *
 * The ESP32-S3 ROM bootloader loads the image segments into SRAM and calls
 * _start on its own stack with the windowed ABI already live.  Everything a
 * second-stage bootloader would normally have done before the application
 * runs, and that this image depends on, has to happen here.
 */
#include <stdint.h>
#include <string.h>

#include "esp_cpu.h"
#include "esp_rom_sys.h"
#include "hal/wdt_hal.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "xtensa/config/core.h"
#include "xtensa/corebits.h"
#include "xtensa/xtruntime.h"

#include "board.h"

extern char _vector_table;
extern char _bss_start, _bss_end;
extern int  main(void);

/*
 * The ROM arms watchdogs before it starts the image and expects the next
 * stage to take them over.  Nothing here feeds them, so switch them off:
 *
 *   - RTC watchdog and timer group 0 watchdog: "flash boot" protection.
 *   - Super watchdog: cannot be disabled, only put in auto-feed mode.
 */
static void watchdog_disable(wdt_hal_context_t *wdt)
{
    wdt_hal_write_protect_disable(wdt);
    wdt_hal_set_flashboot_en(wdt, false);
    wdt_hal_disable(wdt);
    wdt_hal_write_protect_enable(wdt);
}

static void watchdogs_disable(void)
{
    wdt_hal_context_t rwdt  = RWDT_HAL_CONTEXT_DEFAULT();
    wdt_hal_context_t mwdt0 = { .inst = WDT_MWDT0, .mwdt_dev = &TIMERG0 };
    wdt_hal_context_t mwdt1 = { .inst = WDT_MWDT1, .mwdt_dev = &TIMERG1 };

    watchdog_disable(&rwdt);
    watchdog_disable(&mwdt0);
    watchdog_disable(&mwdt1);

    REG_WRITE(RTC_CNTL_SWD_WPROTECT_REG, RTC_CNTL_SWD_WKEY_VALUE);
    REG_SET_BIT(RTC_CNTL_SWD_CONF_REG, RTC_CNTL_SWD_AUTO_FEED_EN);
    REG_WRITE(RTC_CNTL_SWD_WPROTECT_REG, 0);
}

void __attribute__((noreturn, section(".entry.text"))) _start(void)
{
    uint32_t ps;

    /*
     * Mask everything the kernel manages and drop whatever interrupt enables
     * the ROM left behind, then take over the vectors.
     */
    (void) XTOS_SET_INTLEVEL(XCHAL_EXCM_LEVEL);
    esp_cpu_intr_disable(0xFFFFFFFFu);
    esp_cpu_intr_set_ivt_addr(&_vector_table);

    /*
     * PS.UM: level-1 interrupts and exceptions go to the User vector, the one
     * the Xtensa RTOS layer dispatches from.  CPENABLE: coprocessors (FPU and
     * the vector extension) usable during initialization; threads acquire
     * them on first use.
     */
    __asm__ volatile ("rsr.ps %0" : "=a" (ps));
    ps |= PS_UM;
    __asm__ volatile ("wsr.ps %0\n rsync" : : "a" (ps));
    __asm__ volatile ("wsr.cpenable %0\n rsync" : : "a" ((uint32_t) XCHAL_CP_MASK));

    memset(&_bss_start, 0, (size_t) (&_bss_end - &_bss_start));

    watchdogs_disable();

    /*
     * ESP-IDF code takes the CPU clock from the ROM's ticks-per-microsecond
     * variable (delays, and here the ThreadX tick).  Make it match the clock
     * registers before anything relies on it.
     */
    esp_rom_set_cpu_ticks_per_us(board_cpu_freq_hz() / 1000000u);

    console_init();

    main();

    for (;;) {
    }
}
