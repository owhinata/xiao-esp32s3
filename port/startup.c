/*
 * Application entry.
 *
 * ESP-IDF's second-stage bootloader (bootloader/) has verified and loaded
 * the RAM segments of this image, mapped its code and read-only data from
 * flash through the cache and MMU, and jumped to _start on the stack it was
 * itself running on (the ROM's start-up stack), with the windowed ABI live
 * and interrupts off.  Clocks run from the PLL with the CPU at
 * CONFIG_BOOTLOADER_CPU_CLK_FREQ_MHZ, the flash interface is configured from
 * the image header, the super watchdog feeds itself, and the RTC watchdog
 * runs (CONFIG_BOOTLOADER_WDT_ENABLE).
 *
 * What is left is what ESP-IDF's application start-up does on the ESP32-S3
 * after the bootloader (call_start_cpu0() in esp_system/port/cpu_start.c and
 * start_cpu0_default() in esp_system/startup.c), in the same order, for the
 * parts this firmware uses:
 *
 *   init_cpu()            vectors
 *   init_bss()            .bss (there is no RTC .bss)
 *   ext_mem_init()        the cache HAL's and the MMU HAL's state
 *                         (cache_hal_init(), mmu_hal_ctx_init()), cache
 *                         geometry, code/data split of the MMU table
 *   sys_rtc_init()        esp_rtc_init(): rtc_init(), the regulators and the
 *                         SAR peripherals (o-code calibration after power-on);
 *                         the CPU clock kept running in WAITI (see _start)
 *   mspi_init()           esp_mmu_map_init(): the MMU driver's free pages,
 *                         all but the ones the bootloader mapped for this
 *                         image (spi_flash_mmap(), the OTA code)
 *   system_early_init()   esp_clk_init(): RTC fast and slow clocks, the slow
 *                         clock's calibration, the final CPU clock; the
 *                         peripheral clock gates (esp_perip_clk_init()); the
 *                         cache error interrupt (esp_cache_err_int_init())
 *   init_disable_rtc_wdt  RTC watchdog off (esp_system/startup_funcs.c)
 *   do_core_init()        the ESP_SYSTEM_INIT_FN(..., CORE, ...) hooks: the
 *                         heap (heap_caps_init()), esp_timer's counter, the
 *                         application information on the console, the
 *                         flash driver (port/board_flash.c)
 *   __libc_init_array()   global constructors
 *
 * The rest of ESP-IDF's start-up hooks run later, where ESP-IDF runs them
 * relative to its scheduler (board_sys_init_secondary(), board_sys_pre_
 * scheduler(), board_sys_pre_app_main(); see board.h): the scheduler here is
 * ThreadX, whose objects can only be made from tx_application_define() on.
 *
 * Left out, with the reason:
 *   - the rest of mspi_init(): timing tuning, which the flash at 80 MHz in
 *     DIO mode does not need, and the PSRAM, which is not used.
 *   - system_early_init(): the memory region protection the bootloader has
 *     already set (bootloader_init_mem()); stopping the second core and
 *     Cache_Set_IDROM_MMU_Info(), which only the PSRAM code reads; the
 *     interrupt matrix clear of a full ESP-IDF system.
 *   - CONFIG_ESP_SYSTEM_BBPLL_RECALIB: a second PLL calibration that only
 *     bootloaders older than ESP-IDF v5.2 need (bootloader/ is the same
 *     ESP-IDF as the application).
 *
 * This file runs from RAM (see the linker script): it reconfigures the cache
 * that the code in flash is fetched through.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_clk_internal.h"
#include "esp_cpu.h"
#include "esp_err.h"
#include "esp_intr_alloc.h"
#include "esp_private/esp_mmu_map_private.h"
#include "esp_rom_sys.h"
#include "esp_private/cache_err_int.h"
#include "esp_private/esp_sys_event.h"
#include "esp32s3/rom/cache.h"
#include "hal/cache_hal.h"
#include "hal/mmu_hal.h"
#include "hal/wdt_hal.h"
#include "soc/ext_mem_defs.h"
#include "soc/soc.h"
#include "soc/system_reg.h"
#include "xtensa/config/core.h"
#include "xtensa/corebits.h"
#include "xtensa/xtruntime.h"

#include "board.h"
#include "freertos/FreeRTOS.h"

_Static_assert(BOARD_CPU_FREQ_MHZ == CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
               "board.h and sdkconfig.h name different CPU clocks");

extern char _vector_table;
extern char _bss_start, _bss_end;
extern char _flash_text_start, _flash_text_end;
extern int  main(void);
extern void __libc_init_array(void);

/* The tables of ESP-IDF's start-up hooks, one per event (linker script). */
extern const esp_sys_event_handler_t _esysev_initc_start[], _esysev_initc_end[];
extern const esp_sys_event_handler_t _esysev_inits_start[], _esysev_inits_end[];
extern const esp_sys_event_handler_t _esysev_presc_start[], _esysev_presc_end[];
extern const esp_sys_event_handler_t _esysev_pream_start[], _esysev_pream_end[];

/* ROM functions without a declaration in the esp_rom headers (cpu_start.c declares them the same way). */
extern void rom_config_instruction_cache_mode(uint32_t cfg_cache_size, uint8_t cfg_cache_ways,
                                              uint8_t cfg_cache_line_size);
extern void rom_config_data_cache_mode(uint32_t cfg_cache_size, uint8_t cfg_cache_ways,
                                       uint8_t cfg_cache_line_size);

/*
 * Cache geometry (CONFIG_ESP32S3_INSTRUCTION_CACHE_SIZE and friends):
 * ESP-IDF's defaults for the ESP32-S3, but an instruction cache of 32 KiB
 * (ESP-IDF: 16 KiB), which the code left in flash of the path of a packet
 * needs.  The caches live in internal SRAM: the instruction cache in SRAM0,
 * which the linker script therefore leaves empty (ICACHE_SIZE there is the
 * single source of the size), the data cache at the top of SRAM2.
 */
extern char _icache_size;       /* absolute symbol, value = size in bytes */
#define BOARD_ICACHE_SIZE       ((uint32_t) &_icache_size)
#define BOARD_DCACHE_SIZE       0x8000u     /* 32 KiB */
#define BOARD_CACHE_WAYS        8u
#define BOARD_CACHE_LINE_SIZE   32u

/*
 * ext_mem_init(): what executing from flash needs on top of the bootloader's
 * mapping, which only ever read flash as data.  Not inlined into _start: it
 * runs before the code in flash may be fetched, and ldscript/check_ram_refs.py
 * checks it on its own.
 */
static void __attribute__((noinline)) cache_init(void)
{
    /*
     * The cache HAL's state: whether autoload is on, which
     * cache_hal_resume() restores after the flash driver's windows
     * (port/board_flash_os.c); the caches and their buses enabled, as the
     * bootloader left them.
     */
    const cache_hal_config_t config = { .core_nums = 1 };
    const mmu_hal_config_t mmu_config = { .core_nums = 1 };

    cache_hal_init(&config);

    /* The MMU HAL's state, which ESP-IDF's MMU driver (esp_mm) works through. */
    mmu_hal_ctx_init(&mmu_config);

    /* Cache geometry. */
    rom_config_instruction_cache_mode(BOARD_ICACHE_SIZE, BOARD_CACHE_WAYS, BOARD_CACHE_LINE_SIZE);
    Cache_Suspend_DCache();
    rom_config_data_cache_mode(BOARD_DCACHE_SIZE, BOARD_CACHE_WAYS, BOARD_CACHE_LINE_SIZE);
    Cache_Resume_DCache(0);

    /*
     * The instruction and data windows share one MMU table, and the ROM's
     * cache code has to be told how many of its entries are used for code
     * (ESP_ROM_NEEDS_SET_CACHE_MMU_SIZE).  The code starts on the first page
     * of the instruction window; the linker script puts the read-only data on
     * the pages after it.
     */
    {
        const uint32_t text_size = (uint32_t) (&_flash_text_end - &_flash_text_start);
        const uint32_t irom_entries_size =
            (text_size + CONFIG_MMU_PAGE_SIZE - 1u) / CONFIG_MMU_PAGE_SIZE * sizeof(uint32_t);

        Cache_Set_IDROM_MMU_Size(irom_entries_size, CACHE_DROM_MMU_MAX_END - irom_entries_size);
    }
}

/*
 * The bootloader leaves the RTC watchdog running to cover the boot (and has
 * already cleared the flash-boot protection of the RTC and timer group 0
 * watchdogs and put the super watchdog in auto-feed mode).  Nothing here
 * feeds it: switch it off, as ESP-IDF's init_disable_rtc_wdt does.
 */
static void rtc_watchdog_disable(void)
{
    wdt_hal_context_t rwdt = RWDT_HAL_CONTEXT_DEFAULT();

    wdt_hal_write_protect_disable(&rwdt);
    wdt_hal_disable(&rwdt);
    wdt_hal_write_protect_enable(&rwdt);
}

/*
 * Runs the hooks of one event in the order of their priority, as ESP-IDF's
 * do_system_init_event() does, which also stops at the first failure
 * (abort(): a fault record and a restart here).
 */
static void sys_event_run(const esp_sys_event_handler_t *h, const esp_sys_event_handler_t *end)
{
    for (; h < end; h++) {
        if (h->handler(NULL, NULL) != ESP_OK) {
            esp_rom_printf("Start-up hook %p failed\n", (void *) h->handler);
            abort();
        }
    }
}

void board_sys_init_secondary(void)
{
    /* The hooks create FreeRTOS tasks (esp_timer's). */
    if (tx_freertos_init() != TX_SUCCESS) {
        esp_rom_printf("The FreeRTOS layer did not start\n");
        abort();
    }
    sys_event_run(_esysev_inits_start, _esysev_inits_end);
}

void board_sys_pre_scheduler(void)
{
    sys_event_run(_esysev_presc_start, _esysev_presc_end);
}

void board_sys_pre_app_main(void)
{
    sys_event_run(_esysev_pream_start, _esysev_pream_end);
}

/*
 * CPU interrupts that this firmware routes and serves itself, outside
 * ESP-IDF's interrupt allocator, which must therefore never hand them out
 * (esp_intr_reserve()).  The allocator's table already keeps it off the
 * tick's (6, a "special" timer interrupt), the interrupt watchdog's (24),
 * the cache error interrupt's (25) and the other reserved ones; on one
 * core it leaves 0 free, which the Wi-Fi driver takes for its MAC interrupt
 * (ETS_WMAC_INUM) with xt_set_interrupt_handler() as it does under ESP-IDF,
 * and which would otherwise be the first level-1 interrupt the allocator
 * hands out.
 */
static void intr_reserve_own(void)
{
    static const int own[] = {
        ETS_WMAC_INUM,              /* Wi-Fi MAC */
        BOARD_CONSOLE_RX_CPU_INTR,  /* USB Serial/JTAG, port/board.c */
        BOARD_CONSOLE_UART_CPU_INTR /* UART0, port/board.c */
    };
    unsigned int i;

    for (i = 0; i < sizeof(own) / sizeof(own[0]); i++) {
        if (esp_intr_reserve(own[i], 0) != ESP_OK) {
            esp_rom_printf("CPU interrupt %d could not be reserved\n", own[i]);
            abort();
        }
    }
}

void __attribute__((noreturn, section(".entry.text"))) _start(void)
{
    uint32_t ps;

    /*
     * Mask everything the kernel manages and drop whatever interrupt enables
     * the ROM and the bootloader left behind, then take over the vectors.
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

    cache_init();

    /*
     * The RTC domain, then the clocks.  esp_clk_init() ends with the CPU at
     * CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, the ROM's ticks-per-microsecond
     * variable (delays, the ThreadX tick) and CCOUNT scaled to it.
     */
    esp_rtc_init();

    /*
     * ESP-IDF's MMU driver: the linear address space minus the pages that
     * hold this image's code and read-only data (_instruction_reserved_*,
     * _rodata_reserved_*; see the linker script).  From flash, as in
     * ESP-IDF.
     */
    esp_mmu_map_init();

    esp_clk_init();

    /*
     * Peripheral clock gates as ESP-IDF leaves them: the clocks of unused
     * peripherals off, and the modem clock bits, its low-power clock source
     * and the RNG clock set the way esp_phy and the Wi-Fi driver expect
     * (esp_phy_enable() asserts on them).  The USB Serial/JTAG controller
     * keeps its clock because CONFIG_USJ_ENABLE_USB_SERIAL_JTAG is set, and
     * UART0 because it is CONFIG_ESP_CONSOLE_UART_NUM.
     */
    esp_perip_clk_init();

    /*
     * The cache error interrupt (level 4, ETS_CACHEERR_INUM), as ESP-IDF's
     * system_early_init(): an access to the flash windows while the cache
     * is off, or an illegal cache operation, ends in panicHandler()
     * (port/panic.c) through xt_highint4 instead of a stall or wrong data.
     * It also makes the cache refuse writes to the flash windows.
     */
    esp_cache_err_int_init();

    /*
     * rtc_init() lets the CPU clock stop while the CPU waits in WAITI
     * (RTC_CONFIG_DEFAULT()'s cpu_waiti_clk_gate), which ESP-IDF can do
     * because its tick on this chip comes from SYSTIMER.  Here the tick is
     * CCOMPARE0, which counts CPU cycles (CCOUNT), as do the run times of
     * "ps": the clock keeps running while the scheduler idles in WAITI, as
     * it does after reset.
     */
    SET_PERI_REG_MASK(SYSTEM_CPU_PER_CONF_REG, SYSTEM_CPU_WAIT_MODE_FORCE_ON);

    rtc_watchdog_disable();

    console_init();

    /* do_core_init(), then the allocator's bookkeeping (it needs the heap). */
    sys_event_run(_esysev_initc_start, _esysev_initc_end);
    intr_reserve_own();

    __libc_init_array();

    main();

    for (;;) {
    }
}
