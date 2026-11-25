/*
 * Minimal stand-in for ESP-IDF's "freertos/FreeRTOS.h".
 *
 * Some esp-hal-3rdparty sources (regi2c_ctrl.c, ...) use FreeRTOS only for
 * short critical sections, reached through esp_private/critical_section.h:
 * on Xtensa that header declares each lock as a spinlock_t and maps
 * esp_os_enter/exit_critical*() to portENTER/EXIT_CRITICAL*(lock).  On one
 * core a critical section is an interrupt lockout, which is all this header
 * provides.  Sources that need tasks, queues or semaphores need a real
 * FreeRTOS API on top of ThreadX, not this file.
 *
 * With a single core every critical section excludes every other one, so
 * the lock argument carries no state: the interrupt level saved by the
 * outermost "enter" and the nesting depth are kept once, in
 * port/esp_compat/esp_stubs.c.
 */
#ifndef ESP_COMPAT_FREERTOS_H
#define ESP_COMPAT_FREERTOS_H

#include <stdbool.h>
#include <stdint.h>
#include <xtensa/config/core.h>
#include <xtensa/xtruntime.h>

#include "spinlock.h"

typedef int             BaseType_t;
typedef unsigned int    UBaseType_t;
typedef uint32_t        TickType_t;

typedef spinlock_t      portMUX_TYPE;

#define portMUX_INITIALIZER_UNLOCKED    SPINLOCK_INITIALIZER

extern uint32_t esp_compat_critical_saved_ps;
extern uint32_t esp_compat_critical_nesting;

static inline __attribute__((always_inline)) void esp_compat_critical_enter(void)
{
    uint32_t ps = XTOS_SET_INTLEVEL(XCHAL_EXCM_LEVEL);

    if (esp_compat_critical_nesting++ == 0) {
        esp_compat_critical_saved_ps = ps;
    }
}

static inline __attribute__((always_inline)) void esp_compat_critical_exit(void)
{
    if (--esp_compat_critical_nesting == 0) {
        XTOS_RESTORE_INTLEVEL(esp_compat_critical_saved_ps);
    }
}

#define portENTER_CRITICAL(mux)         ((void) (mux), esp_compat_critical_enter())
#define portEXIT_CRITICAL(mux)          ((void) (mux), esp_compat_critical_exit())
#define portENTER_CRITICAL_ISR(mux)     ((void) (mux), esp_compat_critical_enter())
#define portEXIT_CRITICAL_ISR(mux)      ((void) (mux), esp_compat_critical_exit())
#define portENTER_CRITICAL_SAFE(mux)    ((void) (mux), esp_compat_critical_enter())
#define portEXIT_CRITICAL_SAFE(mux)     ((void) (mux), esp_compat_critical_exit())

#endif
