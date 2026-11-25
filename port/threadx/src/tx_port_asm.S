/*
 * Assembly pieces that Espressif's Xtensa RTOS layer expects from the RTOS
 * port and that ThreadX's own port sources do not cover.
 */
#include "xtensa_rtos.h"
#include "tx_api_asm.h"

    .text

/*
 * XT_RTOS_CP_STATE: return in a15 the coprocessor state save area of the
 * current thread, or 0 when there is none (no thread, or interrupt context).
 * call0 only; may use a15 alone.
 *
 *   return (_tx_thread_system_state == 0 && _tx_thread_current_ptr != 0)
 *          ? &_tx_thread_current_ptr->tx_thread_cp_state : 0;
 */
    .global _tx_thread_coproc_state
    .type   _tx_thread_coproc_state,@function
    .align  4
    .literal_position
    .align  4
_tx_thread_coproc_state:
    movi    a15, _tx_thread_system_state
    l32i    a15, a15, 0
    bnez    a15, 1f
    movi    a15, _tx_thread_current_ptr
    l32i    a15, a15, 0
    beqz    a15, 2f
    addi    a15, a15, tx_thread_cp_state
    ret
1:
    movi    a15, 0
2:
    ret

/*
 * XT_RTOS_CP_EXC_HOOK: called (call0) at the start of the coprocessor
 * exception, where FreeRTOS pins the task to the current core.  Nothing to
 * do on one core.  Must preserve everything but a0.
 */
    .global _tx_thread_coproc_exc_hook
    .type   _tx_thread_coproc_exc_hook,@function
    .align  4
_tx_thread_coproc_exc_hook:
    ret

/*
 * System (interrupt) stack.  _tx_thread_context_save switches to it on the
 * first level of interrupt nesting; _tx_thread_schedule idles on it.
 */
    .section .bss
    .global _xt_interrupt_stack
    .global _xt_interrupt_stack_top
    .align  16
_xt_interrupt_stack:
    .space  TX_SYSTEM_STACK_SIZE
_xt_interrupt_stack_top:
