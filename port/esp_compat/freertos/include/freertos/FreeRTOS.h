/*
 * "freertos/FreeRTOS.h" for ESP-IDF sources: ThreadX's FreeRTOS
 * compatibility layer (its FreeRTOS.h, port/threadx/freertos, with
 * FreeRTOSConfig.h of this directory's parent), plus what ESP-IDF's
 * FreeRTOS adds to the stock API and its sources use.  The additions are
 * implemented here and in port/esp_compat/freertos/freertos_idf.c.
 *
 * One core: ThreadX (non-SMP) runs on core 0 only, so the core arguments
 * are ignored and every spinlock is an interrupt lockout.
 *
 * Kept off the include path of Espressif's Xtensa layer (CMakeLists.txt):
 * xtensa_intr.c and xtensa_intr_asm.S switch to their FreeRTOS code as soon
 * as they can see "freertos/FreeRTOS.h" or "freertos/FreeRTOSConfig.h".
 */
#ifndef ESP_COMPAT_FREERTOS_FREERTOS_H
#define ESP_COMPAT_FREERTOS_FREERTOS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <FreeRTOS.h>                   /* the layer's */

#include <xtensa/config/core.h>
#include <xtensa/corebits.h>
#include <xtensa/xtruntime.h>

#include "spinlock.h"

/*
 * ESP-IDF's FreeRTOS.h brings these in (its idf_additions.h, and
 * portmacro.h of its Xtensa port).
 */
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_system.h"

#ifndef portNUM_PROCESSORS
#define portNUM_PROCESSORS              1
#endif

/* ---- Critical sections -------------------------------------------------- */

/*
 * ESP-IDF's critical sections take a spinlock (portMUX_TYPE), which with
 * one core carries no state.  portENTER_CRITICAL(mux) and
 * taskENTER_CRITICAL(mux) are the configuration's (FreeRTOSConfig.h),
 * which says what a critical section does here; the _ISR and _SAFE forms
 * of portENTER_CRITICAL() are the same section, which serves threads,
 * interrupt handlers, the start-up and the fault path alike.
 */
typedef spinlock_t                      portMUX_TYPE;

#define portMUX_INITIALIZER_UNLOCKED    SPINLOCK_INITIALIZER
#define portMUX_INITIALIZE(mux)         spinlock_initialize(mux)

#define portENTER_CRITICAL_ISR(mux)     portENTER_CRITICAL(mux)
#define portEXIT_CRITICAL_ISR(mux)      portEXIT_CRITICAL(mux)
#define portENTER_CRITICAL_SAFE(mux)    portENTER_CRITICAL(mux)
#define portEXIT_CRITICAL_SAFE(mux)     portEXIT_CRITICAL(mux)

/* ---- Context ------------------------------------------------------------ */

/*
 * In an interrupt handler: inside the bracket of _tx_thread_context_save()
 * and _tx_thread_context_restore(), which every interrupt with a C handler
 * goes through (freertos_idf.c).  Not during initialization.
 */
BaseType_t xPortInIsrContext(void);

/*
 * Whether the caller may block: PS.INTLEVEL is 0.  It is not in interrupt
 * handlers (the dispatcher runs them at their level), in critical sections,
 * in tx_application_define() and before the kernel runs.  ESP-IDF's Xtensa
 * port asks the same.
 */
static inline __attribute__((always_inline)) bool xPortCanYield(void)
{
    uint32_t ps;

    __asm__ volatile ("rsr.ps %0" : "=a" (ps));
    return (ps & PS_INTLEVEL_MASK) == 0u;
}

static inline __attribute__((always_inline)) BaseType_t xPortGetCoreID(void)
{
    return 0;
}

/*
 * ThreadX switches to the most urgent ready thread on its own when the
 * outermost interrupt handler returns (_tx_thread_context_restore()), so a
 * yield requested from a handler has nothing left to do.  With or without
 * the "higher priority task woken" argument, which is evaluated.
 */
static inline __attribute__((always_inline)) void esp_compat_yield_from_isr(int unused, ...)
{
    (void) unused;
}

#define portYIELD()                     tx_thread_relinquish()
#define portYIELD_FROM_ISR(...)         esp_compat_yield_from_isr(0, ##__VA_ARGS__)

/* Read by the shared-interrupt dispatcher of intr_alloc.c: see above. */
#define os_task_switch_is_pended(cpu)   ((void) (cpu), false)

/* Stock FreeRTOS's trace hooks of interrupt handlers; the layer has no tracing. */
#define traceISR_ENTER(irq)
#define traceISR_EXIT()

/* ---- Tasks -------------------------------------------------------------- */

/*
 * ESP-IDF gives stack depths in bytes; the layer counts StackType_t
 * (4 bytes).  These take bytes and round up.  xTaskCreate() and
 * xTaskCreateStatic() are redirected to them, as ESP-IDF's task.h does;
 * a buffer for xTaskCreateStatic() still has to be StackType_t-aligned, and
 * an array declared as "StackType_t stack[size in bytes]" is four times as
 * large as it needs to be here (ESP-IDF's StackType_t is one byte).
 */
#define tskNO_AFFINITY                  ((BaseType_t) 0x7FFFFFFF)

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t pxTaskCode,
                                   const char *const pcName,
                                   const uint32_t ulStackDepth,
                                   void *const pvParameters,
                                   UBaseType_t uxPriority,
                                   TaskHandle_t *const pxCreatedTask,
                                   const BaseType_t xCoreID);

TaskHandle_t xTaskCreateStaticPinnedToCore(TaskFunction_t pxTaskCode,
                                           const char *const pcName,
                                           const uint32_t ulStackDepth,
                                           void *const pvParameters,
                                           UBaseType_t uxPriority,
                                           StackType_t *const puxStackBuffer,
                                           StaticTask_t *const pxTaskBuffer,
                                           const BaseType_t xCoreID);

#define xTaskCreate(pxTaskCode, pcName, ulStackDepth, pvParameters, uxPriority, pxCreatedTask) \
    xTaskCreatePinnedToCore((pxTaskCode), (pcName), (ulStackDepth), (pvParameters),            \
                            (uxPriority), (pxCreatedTask), tskNO_AFFINITY)
#define xTaskCreateStatic(pxTaskCode, pcName, ulStackDepth, pvParameters, uxPriority,          \
                          puxStackBuffer, pxTaskBuffer)                                        \
    xTaskCreateStaticPinnedToCore((pxTaskCode), (pcName), (ulStackDepth), (pvParameters),      \
                                  (uxPriority), (puxStackBuffer), (pxTaskBuffer), tskNO_AFFINITY)

/* ---- Memory ------------------------------------------------------------- */

/*
 * Bytes free in the layer's byte pool (configTOTAL_HEAP_SIZE), where its
 * tasks, queues, semaphores, mutexes and event groups are allocated.
 */
size_t esp_compat_freertos_pool_free(void);

/* ---- Lists -------------------------------------------------------------- */

/*
 * Stock FreeRTOS's opaque list type, as its FreeRTOS.h defines it.  Only
 * ESP-IDF's freertos/ringbuf.h names it, in StaticRingbuffer_t, the buffer
 * of the static ring buffers, which port/esp_compat/freertos/ringbuf.c does
 * not implement.
 */
typedef struct xSTATIC_LIST {
    UBaseType_t uxDummy1;
    void *pvDummy2;
    struct {
        TickType_t xDummy2;
        void *pvDummy3[2];
    } xDummy3;
} StaticList_t;

/* ---- Queues ------------------------------------------------------------- */

#define queueSEND_TO_BACK               ((BaseType_t) 0)
#define queueSEND_TO_FRONT              ((BaseType_t) 1)
#define queueOVERWRITE                  ((BaseType_t) 2)

BaseType_t xQueueGenericSend(QueueHandle_t xQueue,
                             const void *const pvItemToQueue,
                             TickType_t xTicksToWait,
                             const BaseType_t xCopyPosition);

/* Only for a queue made with xQueueCreateStatic(); pdFALSE otherwise. */
BaseType_t xQueueGetStaticBuffers(QueueHandle_t xQueue,
                                  uint8_t **ppucQueueStorage,
                                  StaticQueue_t **ppxStaticQueue);

#endif
