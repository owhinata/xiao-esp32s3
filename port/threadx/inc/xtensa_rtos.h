/*
 * Glue between Espressif's Xtensa RTOS porting layer
 * (esp-hal-3rdparty/components/xtensa) and ThreadX.
 *
 * The porting layer -- vectors, interrupt dispatch, context save/restore,
 * coprocessor exception -- is RTOS-agnostic and reaches the RTOS only through
 * the names defined here.  This is the ThreadX counterpart of the header
 * FreeRTOS supplies in ESP-IDF and of baremetal/xtensa_rtos.h in the layer.
 *
 * Included by the layer's own sources (C and assembly), so it has to stay
 * free of ThreadX C declarations.
 */
#ifndef XTENSA_RTOS_H
#define XTENSA_RTOS_H

#ifdef __ASSEMBLER__
#include <xtensa/coreasm.h>
#else
#include <xtensa/config/core.h>
#endif

#include <xtensa/corebits.h>
#include <xtensa/config/system.h>

#ifdef TX_INCLUDE_USER_DEFINE_FILE
#include "tx_user.h"
#endif

#define XT_RTOS_NAME            ThreadX

#if !XCHAL_HAVE_XEA2
#error "This ThreadX port requires Xtensa Exception Architecture 2 (XEA2)."
#endif

/*
 * The layer sizes its per-CPU tables with this FreeRTOS-named macro.
 * ThreadX (non-SMP) runs on core 0 only.
 */
#ifndef portNUM_PROCESSORS
#define portNUM_PROCESSORS      1
#endif
#if portNUM_PROCESSORS != 1
#error "ThreadX (non-SMP) supports one core"
#endif

/*
 * "Board mode": the CPU clock is not a compile-time constant.  The tick
 * divisor is the variable _xt_tick_divisor, set by _xt_tick_divisor_init()
 * (port/threadx/src/tx_initialize_low_level.c).
 */
#ifndef XT_BOARD
#define XT_BOARD                1
#endif

/* Entry to / exit from an interrupt that may cause a context switch (call0). */
#define XT_RTOS_INT_ENTER       _tx_thread_context_save
#define XT_RTOS_INT_EXIT        _tx_thread_context_restore

/* Tick interrupt; C calling convention. */
#ifndef TX_NO_TIMER
#define XT_RTOS_TIMER_INT       _tx_timer_interrupt
#endif
#ifndef TX_TIMER_TICKS_PER_SECOND
#define TX_TIMER_TICKS_PER_SECOND 100
#endif
#define XT_TICK_PER_SEC         TX_TIMER_TICKS_PER_SECOND

/*
 * Coprocessor exception support (call0, see port/threadx/src/tx_port_asm.S):
 *   XT_RTOS_CP_STATE     a15 = save area of the current thread, 0 if none.
 *   XT_RTOS_CP_EXC_HOOK  called first in the exception; FreeRTOS pins the
 *                        task to its core there, ThreadX has nothing to do.
 */
#define XT_RTOS_CP_STATE        _tx_thread_coproc_state
#define XT_RTOS_CP_EXC_HOOK     _tx_thread_coproc_exc_hook

/* Convenience macros used by the ThreadX assembly sources. */
#define XT_INTS_DISABLE(reg)    rsil reg, XCHAL_EXCM_LEVEL
#define XT_INTS_ENABLE(reg)     wsr  reg, PS; rsync

#define XT_INTEXC_HOOK_NUM      (1 + XCHAL_NUM_INTLEVELS + XCHAL_HAVE_NMI)

#ifndef __ASSEMBLER__
typedef unsigned (*XT_INTEXC_HOOK)(unsigned cause);
extern volatile XT_INTEXC_HOOK _xt_intexc_hooks[XT_INTEXC_HOOK_NUM];
#endif

#include "xtensa_context.h"

#ifdef XT_RTOS_TIMER_INT
#include "xtensa_timer.h"
#endif

#endif /* XTENSA_RTOS_H */
