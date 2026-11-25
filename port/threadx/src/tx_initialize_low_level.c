/*
 * Low-level ThreadX initialization on top of Espressif's Xtensa RTOS layer.
 * Derived from ports/xtensa/xcc/src/tx_initialize_low_level.c and
 * xtensa_init.c of ThreadX (MIT, Microsoft Corporation / Cadence Design
 * Systems / Eclipse ThreadX contributors).
 */
#define TX_SOURCE_CODE

#include "tx_api.h"
#include "tx_initialize.h"
#include "tx_thread.h"
#include "xtensa_api.h"
#include "xtensa_rtos.h"
#include "esp_rom_sys.h"

#ifndef TX_NO_TIMER

/* CPU cycles per tick; read by _tx_timer_interrupt. */
unsigned _xt_tick_divisor;

/*
 * The CPU clock is the one ESP-IDF code keeps in the ROM's "ticks per
 * microsecond" variable (esp_rom_set_cpu_ticks_per_us).  Whoever changes the
 * CPU frequency must update that variable and call this again.
 */
void _xt_tick_divisor_init(void)
{
    _xt_tick_divisor = (esp_rom_get_cpu_ticks_per_us() * 1000000u) / XT_TICK_PER_SEC;
}

#endif /* TX_NO_TIMER */

VOID _tx_initialize_low_level(VOID)
{
    extern char _xt_interrupt_stack;
    extern char _xt_interrupt_stack_top;
    extern void _tx_clib_init(void);
    extern void _xt_coproc_init(void);
    uint32_t ccount;

    TX_INTERRUPT_SAVE_AREA

    /* Interrupts stay masked until the first thread is dispatched. */
    TX_DISABLE
    (void) interrupt_save;

    /*
     * Nothing runs on the system stack yet (this is still the ROM's stack).
     * Fill it the way ThreadX fills thread stacks (the bytes of
     * TX_STACK_FILL), so that its high-water mark can be read the same way.
     */
    memset(&_xt_interrupt_stack, 0xEF, (size_t) (&_xt_interrupt_stack_top - &_xt_interrupt_stack));
    _tx_thread_system_stack_ptr = &_xt_interrupt_stack_top;

    /*
     * No free RAM to hand to tx_application_define(): everything the image
     * does not occupy already belongs to ESP-IDF's heap (heap_caps_init()
     * in the start-up code), which malloc() and heap_caps_malloc() serve.
     */
    _tx_initialize_unused_memory = TX_NULL;

#if XCHAL_CP_NUM > 0
    /*
     * No coprocessor has an owner yet.  CPENABLE is left as the startup code
     * set it (all enabled) so that tx_application_define() can use them; each
     * thread starts with all of them disabled and acquires them on first use.
     */
    _xt_coproc_init();
#endif

#ifndef TX_NO_TIMER
    _xt_tick_divisor_init();
    __asm__ volatile ("rsr.ccount %0" : "=a" (ccount));
    ccount += _xt_tick_divisor;
#if XT_TIMER_INDEX == 0
    __asm__ volatile ("wsr.ccompare0 %0\n esync" : : "a" (ccount));
#elif XT_TIMER_INDEX == 1
    __asm__ volatile ("wsr.ccompare1 %0\n esync" : : "a" (ccount));
#elif XT_TIMER_INDEX == 2
    __asm__ volatile ("wsr.ccompare2 %0\n esync" : : "a" (ccount));
#else
#error "Unsupported XT_TIMER_INDEX"
#endif
    /*
     * The layer dispatches this interrupt straight to XT_RTOS_TIMER_INT,
     * which ends in _tx_timer_interrupt (xtensa_rtos.h).
     */
    xt_ints_on(XT_TIMER_INTEN);
#else
    (void) ccount;
#endif

    _tx_clib_init();
}
