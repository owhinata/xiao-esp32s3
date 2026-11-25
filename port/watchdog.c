/*
 * Watchdogs: the interrupt watchdog, and the RTC watchdog as the last resort
 * of the fault path.
 *
 * The interrupt watchdog is ESP-IDF's (esp_system/int_wdt.c,
 * CONFIG_ESP_INT_WDT) on a single core: timer group 1's watchdog (MWDT1),
 * fed by the tick interrupt.  When no tick has run for
 * CONFIG_ESP_INT_WDT_TIMEOUT_MS (interrupts masked up to XCHAL_EXCM_LEVEL
 * for that long, or an interrupt handler that does not return), stage 0
 * raises the level-4 CPU interrupt ETS_INT_WDT_INUM.  Level 4 is above what
 * a lockout masks, so it is taken where the code is stuck; Espressif's
 * xt_highint4 (esp_system/port/soc/esp32s3/highint_hdl.S) saves the frame
 * and calls panicHandler() (port/panic.c) with the pseudo cause
 * PANIC_RSN_INTWDT_CPU0.  If that cannot run (the CPU at level 4 or above)
 * stage 1 resets the system twice the stage-0 time later, with no record;
 * board_reset_by_int_wdt() (port/board.c) tells the next start.
 *
 * The tick reaches the kernel through board_tick_interrupt(), the layer's
 * XT_RTOS_TIMER_INT (port/threadx/inc/xtensa_rtos.h); the first tick starts
 * the watchdog, so start-up and tx_application_define(), which run with
 * interrupts masked, are not watched.  The stages are set once and every
 * tick only feeds (ESP-IDF sets them again at every tick because it starts
 * with a longer first timeout).
 *
 * The fault path runs at level 15 for as long as its output takes.  At its
 * entry board_watchdog_fault() does what ESP-IDF's panic handler does
 * (esp_system/port/panic_handler.c): the timer group watchdog off, the RTC
 * watchdog armed so that a fault path that hangs still restarts.
 * board_restart() then stops both just before its reset, so that nothing of
 * this outlives the reset (the RTC watchdog is not reset by it, and the ROM's
 * download mode must not be cut short).
 *
 * This file runs from RAM (see the linker script): the tick wrapper is on
 * the interrupt path, and the fault path calls it before the record is
 * sealed.  The wdt_hal functions are in wdt_hal_iram.c, also in RAM; the LL
 * functions are inline, rtc_clk_slow_freq_get_hz() is in rtc_clk.c (RAM),
 * esp_rom_route_intr_matrix() is in ROM and xt_ints_on() in
 * xtensa_intr_asm.S (RAM).
 */
#include <stdbool.h>
#include <stdint.h>

#include "tx_api.h"

#include "sdkconfig.h"
#include "esp_rom_sys.h"
#include "esp_private/esp_int_wdt.h"
#include "hal/timg_ll.h"
#include "hal/wdt_hal.h"
#include "soc/interrupts.h"
#include "soc/rtc.h"
#include "soc/soc.h"
#include "xtensa_api.h"

#include "board.h"

/* port/threadx/src/tx_timer_interrupt.S; no ThreadX header declares it. */
extern VOID _tx_timer_interrupt(VOID);

/*
 * How long the fault path may take before the RTC watchdog restarts the
 * board.  It takes about 0.5 s at most: the UART's ring and FIFO (0.37 s at
 * 115200 baud), the dump (some 600 bytes, 52 ms), and one
 * CONSOLE_TX_TIMEOUT_MS per transport that does not drain.  The RTC slow
 * clock it counts is the RC oscillator, which is not trimmed.
 */
#define FAULT_RTC_WDT_TIMEOUT_MS    2000u

/* Timer group 1's watchdog; mwdt_dev stays NULL until the first tick. */
static wdt_hal_context_t int_wdt;

/*
 * As esp_int_wdt_init() and esp_int_wdt_cpu_init() do: the timer group's
 * clock on and the group reset (which also clears the flash-boot protection
 * of its watchdog), the prescaler for 500 us ticks from the 80 MHz APB clock
 * (MWDT_LL_DEFAULT_CLK_PRESCALER), the two stages, then the source routed to
 * the level-4 CPU interrupt.  wdt_hal_init() clears the interrupt status.
 */
static void int_wdt_start(void)
{
    int __DECLARE_RCC_RC_ATOMIC_ENV __attribute__((unused)); /* what PERIPH_RCC_ACQUIRE_ATOMIC declares */

    timg_ll_enable_bus_clock(1, true);
    timg_ll_reset_register(1);

    wdt_hal_init(&int_wdt, WDT_MWDT1, MWDT_LL_DEFAULT_CLK_PRESCALER, true);
    wdt_hal_write_protect_disable(&int_wdt);
    wdt_hal_config_stage(&int_wdt, WDT_STAGE0, IWDT_STAGE0_TIMEOUT_US, WDT_STAGE_ACTION_INT);
    wdt_hal_config_stage(&int_wdt, WDT_STAGE1, IWDT_STAGE1_TIMEOUT_US, WDT_STAGE_ACTION_RESET_SYSTEM);
    wdt_hal_enable(&int_wdt);
    wdt_hal_write_protect_enable(&int_wdt);

    esp_rom_route_intr_matrix(0, ETS_TG1_WDT_LEVEL_INTR_SOURCE, ETS_INT_WDT_INUM);
    xt_ints_on(1u << ETS_INT_WDT_INUM);
}

/*
 * The tick interrupt (XT_RTOS_TIMER_INT), called by the layer's dispatcher
 * at the tick's level: feed, then ThreadX's tick.  Fed once per interrupt,
 * also when the interrupt then counts several late ticks.
 */
void board_tick_interrupt(void)
{
    if (int_wdt.mwdt_dev == NULL) {
        int_wdt_start();
    }
    wdt_hal_write_protect_disable(&int_wdt);
    wdt_hal_feed(&int_wdt);
    wdt_hal_write_protect_enable(&int_wdt);

    _tx_timer_interrupt();
}

/*
 * Local contexts, as ESP-IDF's panic and restart code uses: valid whether or
 * not the first tick has run.
 */
static void int_wdt_stop(void)
{
    wdt_hal_context_t mwdt1 = { .inst = WDT_MWDT1, .mwdt_dev = &TIMERG1 };

    wdt_hal_write_protect_disable(&mwdt1);
    wdt_hal_disable(&mwdt1);
    wdt_hal_write_protect_enable(&mwdt1);
}

void board_watchdog_fault(void)
{
    wdt_hal_context_t rwdt = RWDT_HAL_CONTEXT_DEFAULT();
    uint32_t ticks = (uint32_t) ((uint64_t) FAULT_RTC_WDT_TIMEOUT_MS * rtc_clk_slow_freq_get_hz() / 1000u);

    int_wdt_stop();

    /*
     * esp_panic_handler_enable_rtc_wdt(), except that the reset is a system
     * reset (the RAM keeps the record, as after the software reset of
     * board_restart()) rather than an RTC reset.
     */
    wdt_hal_init(&rwdt, WDT_RWDT, 0, false);
    wdt_hal_write_protect_disable(&rwdt);
    wdt_hal_config_stage(&rwdt, WDT_STAGE0, ticks, WDT_STAGE_ACTION_RESET_SYSTEM);
    wdt_hal_enable(&rwdt);
    wdt_hal_write_protect_enable(&rwdt);
}

void board_watchdog_stop(void)
{
    wdt_hal_context_t rwdt = RWDT_HAL_CONTEXT_DEFAULT();

    int_wdt_stop();
    wdt_hal_write_protect_disable(&rwdt);
    wdt_hal_disable(&rwdt);
    wdt_hal_write_protect_enable(&rwdt);
}
