/*
 * ThreadX build options for this port.
 *
 * Also included by assembly sources and by Espressif's Xtensa layer (through
 * xtensa_rtos.h), so it may hold preprocessor definitions only, and values
 * must be plain numbers without C suffixes.
 */
#ifndef TX_USER_H
#define TX_USER_H

/* Tick rate.  The CCOMPARE reload value is derived from it at start-up. */
#define TX_TIMER_TICKS_PER_SECOND       100

/* Detect thread stack overflow at context switches. */
#define TX_ENABLE_STACK_CHECKING

#endif
