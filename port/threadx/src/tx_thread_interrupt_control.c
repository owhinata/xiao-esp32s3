/*
 * _tx_thread_interrupt_control for the Xtensa port (XEA2).
 * Adapted from ports/xtensa/xcc/src/tx_thread_interrupt_control.c of ThreadX
 * (MIT, Microsoft Corporation / Cadence Design Systems / Eclipse ThreadX
 * contributors); the XCC intrinsics are replaced by GCC inline assembly.
 */
#define TX_SOURCE_CODE

#include "tx_api.h"
#include "tx_thread.h"

/*
 * Set PS.INTLEVEL to new_posture (TX_INT_ENABLE, TX_INT_DISABLE, or a value
 * returned earlier) and return the previous level.
 */
UINT _tx_thread_interrupt_control(UINT new_posture)
{
    UINT ps;

    __asm__ volatile ("rsil %0, 15" : "=a" (ps) : : "memory");
    __asm__ volatile ("wsr.ps %0\n rsync"
                      : : "a" ((ps & ~0xFu) | (new_posture & 0xFu)) : "memory");
    return ps & 0xFu;
}
