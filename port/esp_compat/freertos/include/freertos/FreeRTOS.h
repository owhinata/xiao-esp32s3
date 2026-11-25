/*
 * "freertos/FreeRTOS.h" for ESP-IDF sources: ThreadX's FreeRTOS
 * compatibility layer (its FreeRTOS.h, with FreeRTOSConfig.h of this
 * directory's parent), plus what ESP-IDF's FreeRTOS adds to the stock API
 * and its sources use.  The additions are implemented here and in
 * port/esp_compat/freertos/freertos_idf.c.
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

/* ---- Ticks -------------------------------------------------------------- */

#define portTICK_PERIOD_MS              ((TickType_t) (1000u / configTICK_RATE_HZ))

/* As FreeRTOS 11 and ESP-IDF compute them, without the 32-bit overflow of the layer's. */
#undef pdMS_TO_TICKS
#define pdMS_TO_TICKS(xTimeInMs)                                               \
    ((TickType_t) (((uint64_t) (xTimeInMs) * (uint64_t) configTICK_RATE_HZ) / (uint64_t) 1000u))
#define pdTICKS_TO_MS(xTicks)                                                  \
    ((TickType_t) (((uint64_t) (xTicks) * (uint64_t) 1000u) / (uint64_t) configTICK_RATE_HZ))

/* ---- Critical sections -------------------------------------------------- */

/*
 * ESP-IDF's critical sections take a spinlock (portMUX_TYPE).  With one core
 * a critical section excludes every other one and the lock itself carries no
 * state: one nesting depth and the PS saved by the outermost entry serve
 * them all (freertos_idf.c).  Entering masks the interrupts up to
 * XCHAL_EXCM_LEVEL (every interrupt with a C handler, what ThreadX's
 * TX_DISABLE masks), never lowering a higher level, and the outermost
 * entry also disables ThreadX's preemption (_tx_thread_preempt_disable),
 * as ESP-IDF's FreeRTOS does no switch inside: a service that readies a
 * more urgent thread inside the section (a semaphore or queue given, also
 * through the _FromISR forms the Wi-Fi driver uses with interrupts masked)
 * leaves the switch pending.  The outermost exit restores the PS, enables
 * preemption again and makes that switch, but only where the caller may
 * switch: in a thread whose restored PS has the interrupts open.  In an
 * interrupt handler ThreadX switches when the outermost handler returns;
 * before the kernel runs and in tx_application_define() there is nothing
 * to switch to; with the interrupts masked by the caller the switch waits
 * for the next interrupt (the tick at the latest).  So a section is never
 * left in another thread, the saved PS is always the entering thread's,
 * and the same code serves threads, interrupt handlers, the start-up and
 * the fault path, as the _ISR and _SAFE forms of ESP-IDF do.  Nothing that
 * can block may be called inside: a ThreadX service that would wait
 * returns its error at once while preemption is disabled.
 */
typedef spinlock_t                      portMUX_TYPE;

#define portMUX_INITIALIZER_UNLOCKED    SPINLOCK_INITIALIZER
#define portMUX_INITIALIZE(mux)         spinlock_initialize(mux)

extern uint32_t esp_compat_critical_saved_ps;
extern uint32_t esp_compat_critical_nesting;

/* ThreadX's (tx_thread.h; freertos_idf.c includes it, which checks these against it). */
extern volatile UINT _tx_thread_preempt_disable;
extern TX_THREAD *_tx_thread_current_ptr;
extern TX_THREAD *_tx_thread_execute_ptr;
VOID _tx_thread_system_preempt_check(VOID);

static inline __attribute__((always_inline)) void esp_compat_critical_enter(void)
{
    uint32_t ps = XTOS_SET_MIN_INTLEVEL(XCHAL_EXCM_LEVEL);

    if (esp_compat_critical_nesting++ == 0u) {
        esp_compat_critical_saved_ps = ps;
        _tx_thread_preempt_disable++;
    }
}

static inline __attribute__((always_inline)) void esp_compat_critical_exit(void)
{
    uint32_t ps;

    if (--esp_compat_critical_nesting != 0u) {
        return;
    }
    ps = esp_compat_critical_saved_ps;
    _tx_thread_preempt_disable--;
    XTOS_RESTORE_INTLEVEL(ps);
    if ((ps & (PS_INTLEVEL_MASK | PS_EXCM)) == 0u && _tx_thread_current_ptr != _tx_thread_execute_ptr) {
        /* Switches only at thread level (no interrupt handler, no initialization). */
        _tx_thread_system_preempt_check();
    }
}

/* The layer's forms take no lock; ESP-IDF's take one. */
#undef portENTER_CRITICAL
#undef portEXIT_CRITICAL
#undef taskENTER_CRITICAL
#undef taskEXIT_CRITICAL

#define portENTER_CRITICAL(mux)         ((void) (mux), esp_compat_critical_enter())
#define portEXIT_CRITICAL(mux)          ((void) (mux), esp_compat_critical_exit())
#define portENTER_CRITICAL_ISR(mux)     ((void) (mux), esp_compat_critical_enter())
#define portEXIT_CRITICAL_ISR(mux)      ((void) (mux), esp_compat_critical_exit())
#define portENTER_CRITICAL_SAFE(mux)    ((void) (mux), esp_compat_critical_enter())
#define portEXIT_CRITICAL_SAFE(mux)     ((void) (mux), esp_compat_critical_exit())
#define taskENTER_CRITICAL(mux)         portENTER_CRITICAL(mux)
#define taskEXIT_CRITICAL(mux)          portEXIT_CRITICAL(mux)
#define taskENTER_CRITICAL_ISR(mux)     portENTER_CRITICAL_ISR(mux)
#define taskEXIT_CRITICAL_ISR(mux)      portEXIT_CRITICAL_ISR(mux)
#define taskENTER_CRITICAL_SAFE(mux)    portENTER_CRITICAL_SAFE(mux)
#define taskEXIT_CRITICAL_SAFE(mux)     portEXIT_CRITICAL_SAFE(mux)

#define portSET_INTERRUPT_MASK_FROM_ISR()       taskENTER_CRITICAL_FROM_ISR()
#define portCLEAR_INTERRUPT_MASK_FROM_ISR(x)    taskEXIT_CRITICAL_FROM_ISR(x)

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
#define traceISR_EXIT_TO_SCHEDULER()

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

/*
 * The calling task, and deleting a task, redirected as the two above.
 * The layer's xTaskGetCurrentTaskHandle() reads the control block of
 * tx_thread_identify() without a check, which is NULL in an interrupt
 * handler that interrupted the scheduler's idle wait and before the
 * kernel runs: this one returns NULL there.  It is also NULL in a thread
 * that is not a FreeRTOS task (made with tx_thread_create(), as the
 * shell's), as with the layer.  vTaskDelete(NULL) first runs the calling
 * thread's thread-specific data destructors (port/esp_compat/
 * pthread_local.c), as a POSIX thread's exit does: the Wi-Fi driver
 * keeps a semaphore per thread there.  Being object-like, the macros
 * also redirect where the functions' addresses are taken (the Wi-Fi OS
 * adapter's table).
 */
TaskHandle_t esp_compat_task_get_current(void);
void esp_compat_task_delete(TaskHandle_t xTask);

#define xTaskGetCurrentTaskHandle       esp_compat_task_get_current
#define vTaskDelete                     esp_compat_task_delete

/* ---- Memory ------------------------------------------------------------- */

/*
 * Bytes free in the layer's byte pool (configTOTAL_HEAP_SIZE), where its
 * tasks, queues, semaphores, mutexes and event groups are allocated.
 */
size_t esp_compat_freertos_pool_free(void);

/* ---- Mutexes ------------------------------------------------------------ */

/*
 * FreeRTOS's mutexes inherit the priority of the most urgent waiter; the
 * layer's plain mutexes do not (TX_NO_INHERIT), its recursive ones do
 * (TX_INHERIT), and that is all that differs between the two: both are the
 * same ThreadX mutex, and xSemaphoreTake() refuses a second take by the
 * owner whichever made it.  So a plain mutex is made as a recursive one,
 * for lwIP's sys_mutex_new() (sys_arch_protect() among them) and the Wi-Fi
 * OS adapter's mutex_create_wrapper().
 */
#define xSemaphoreCreateMutex()         xSemaphoreCreateRecursiveMutex()
#define xSemaphoreCreateMutexStatic(pxMutexBuffer) xSemaphoreCreateRecursiveMutexStatic(pxMutexBuffer)

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
