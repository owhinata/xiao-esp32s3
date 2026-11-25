/*
 * Stack overflow notification, registered by _tx_initialize_low_level() when
 * TX_ENABLE_STACK_CHECKING is defined.  Derived from the ThreadX Xtensa port
 * (MIT, Microsoft Corporation / Cadence Design Systems / Eclipse ThreadX
 * contributors), minus the simulator and evaluation-board output paths.
 */
#include "tx_api.h"

#ifdef TX_ENABLE_STACK_CHECKING

#include "esp_rom_sys.h"

VOID _tx_xtensa_stack_error_handler(TX_THREAD *thread)
{
    /* ROM printf: no heap, no locks, little stack. */
    esp_rom_printf("\n*** Stack overflow in thread \"%s\"\n",
                   thread->tx_thread_name != TX_NULL ? thread->tx_thread_name : "?");

    __asm__ volatile ("break 1, 15");       /* debugger, or the panic handler */
}

#endif /* TX_ENABLE_STACK_CHECKING */
