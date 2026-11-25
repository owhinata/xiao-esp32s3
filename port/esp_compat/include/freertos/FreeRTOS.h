/*
 * Minimal stand-in for ESP-IDF's "freertos/FreeRTOS.h".
 *
 * Several esp-hal-components sources (regi2c_ctrl.c, periph_ctrl.c, ...) use
 * FreeRTOS only for short critical sections guarded by a spinlock.  On one
 * core a critical section is an interrupt lockout, which is all this header
 * provides.  Sources that need tasks, queues or semaphores need a real
 * FreeRTOS API on top of ThreadX, not this file.
 */
#ifndef ESP_COMPAT_FREERTOS_H
#define ESP_COMPAT_FREERTOS_H

#include <stdbool.h>
#include <stdint.h>
#include <xtensa/config/core.h>
#include <xtensa/xtruntime.h>

typedef int             BaseType_t;
typedef unsigned int    UBaseType_t;
typedef uint32_t        TickType_t;

/* Spinlock: holds the interrupt state saved by the outermost "enter". */
typedef struct {
    uint32_t saved_ps;
    uint32_t nesting;
} portMUX_TYPE;

#define portMUX_INITIALIZER_UNLOCKED    { 0, 0 }

static inline __attribute__((always_inline)) void esp_compat_critical_enter(portMUX_TYPE *mux)
{
    uint32_t ps = XTOS_SET_INTLEVEL(XCHAL_EXCM_LEVEL);

    if (mux->nesting++ == 0) {
        mux->saved_ps = ps;
    }
}

static inline __attribute__((always_inline)) void esp_compat_critical_exit(portMUX_TYPE *mux)
{
    if (--mux->nesting == 0) {
        XTOS_RESTORE_INTLEVEL(mux->saved_ps);
    }
}

#define portENTER_CRITICAL(mux)         esp_compat_critical_enter(mux)
#define portEXIT_CRITICAL(mux)          esp_compat_critical_exit(mux)
#define portENTER_CRITICAL_ISR(mux)     esp_compat_critical_enter(mux)
#define portEXIT_CRITICAL_ISR(mux)      esp_compat_critical_exit(mux)
#define portENTER_CRITICAL_SAFE(mux)    esp_compat_critical_enter(mux)
#define portEXIT_CRITICAL_SAFE(mux)     esp_compat_critical_exit(mux)

#endif
