/*
 * Configuration of ThreadX's FreeRTOS compatibility layer (port/threadx/
 * freertos, a modified copy: port/threadx/ORIGIN.md).  The options the
 * layer reads, and the port's part of the stock FreeRTOS API: the critical
 * sections.  Included by the layer's FreeRTOS.h after tx_api.h, so the
 * ThreadX options (tx_user.h) and the Xtensa interrupt macros of tx_port.h
 * are visible.
 *
 * What ESP-IDF code expects on top of stock FreeRTOS is not here but in
 * freertos/FreeRTOS.h, the header ESP-IDF sources include.
 */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <assert.h>
#include <stdint.h>

#include <xtensa/config/core.h>
#include <xtensa/corebits.h>
#include <xtensa/xtruntime.h>

#include "../../pthread_local.h"

/* The ThreadX tick (tx_user.h); pdMS_TO_TICKS() and portTICK_PERIOD_MS use it. */
#define configTICK_RATE_HZ              TX_TIMER_TICKS_PER_SECOND
#define configUSE_16_BIT_TICKS          0

/*
 * FreeRTOS priorities 0 .. 24, ESP-IDF's default range; ESP-IDF derives its
 * task priorities from the top of it (esp_task.h).  The layer maps FreeRTOS
 * priority p to ThreadX priority TX_MAX_PRIORITIES - 1 - p (31 - p), so they
 * occupy ThreadX 31 .. 7, below and among this firmware's own threads.
 */
#define configMAX_PRIORITIES            25

/*
 * Stack depths in the layer's unit, StackType_t (4 bytes).  ESP-IDF passes
 * bytes; freertos/FreeRTOS.h converts.  The minimum is what xTaskCreate()
 * accepts (ThreadX's own minimum for this port is below it).
 */
#define configSTACK_DEPTH_TYPE          uint32_t
#define configMINIMAL_STACK_SIZE        128u

/*
 * A task's name is copied into its task block, at most this many bytes
 * with the terminating NUL (ESP-IDF's default, CONFIG_FREERTOS_MAX_TASK_
 * NAME_LEN); the ThreadX thread's name is that copy.
 */
#define configMAX_TASK_NAME_LEN         16

/*
 * Objects the layer creates dynamically (xTaskCreate(): task block and
 * stack, xQueueCreate(), xSemaphoreCreate*(), xEventGroupCreate(),
 * xTimerCreate()) come from a ThreadX byte pool over a static array of
 * this size, not from the heap: the layer has no hook for another
 * allocator.  Each block costs 8 bytes of the pool's bookkeeping on top; a
 * task is its stack and a task block of about 880 bytes, a semaphore or
 * mutex 96 bytes, a queue 96 bytes and its items, an event group 44 bytes.
 * The tasks: esp_timer (4096-byte stack), the Wi-Fi driver's (6656,
 * WIFI_TASK_STACK_SIZE), the default event loop's "sys_evt" (2816), lwIP's
 * "tcpip" (3584), which stay, and "main" (src/main.c, 4096), whose stack
 * and task block go back to the pool when it ends after the start-up.
 * The objects: the event loop's queue (32 items of 16 bytes) and mutex,
 * the driver's queues, semaphores, mutexes and event groups, lwIP's mutex
 * (sys_arch_protect()), and one semaphore for each thread that calls the
 * driver (the OS adapter's wifi_thread_semphr_get): a few KiB, not
 * documented.  port/net/wifi.c logs how much the start took from the pool,
 * which is the measure to size it by: with Wi-Fi up, about 9 KiB of 36 KiB
 * are free.  The other 12 KiB are for the USB host while it runs
 * (port/usb/host.c): its two
 * tasks, "usb_host" and "usb_client" (4096-byte stacks), and the USB Host
 * Library's semaphores, mutexes, client queue and timer, about 11 KiB
 * together, which go back when it stops.  The queues and semaphores the OS
 * adapter makes with xQueueCreateStatic() (its _queue_create), lwIP's
 * mailboxes and semaphores (static ones in ESP-IDF's heap) and all of
 * ESP-IDF's malloc() and heap_caps_malloc() use ESP-IDF's heap.
 */
#define configTOTAL_HEAP_SIZE           (48u * 1024u)

/*
 * vTaskDelete() and returning from a task: the layer's own thread
 * "txfr_idle_task" (ThreadX priority 31, the lowest) deletes the task and
 * gives its task block and stack back to the pool (a task made with
 * xTaskCreateStatic() keeps its buffers).  Before it deletes the thread it
 * runs the thread's thread-specific data destructors (port/esp_compat/
 * pthread_local.c), for every deletion, as ESP-IDF's FreeRTOS runs the
 * deletion callbacks of a task's local storage when it frees the task:
 * the Wi-Fi driver keeps a semaphore per thread there.  Its stack, in
 * StackType_t.
 */
#define INCLUDE_vTaskDelete             1
#define TX_FREERTOS_IDLE_STACK          (1536u / 4u)
#define TX_FREERTOS_TASK_DELETE_HOOK(p_thread) esp_compat_pthread_destructors(p_thread)

/* Started from tx_application_define() (tx_freertos_init()). */
#define TX_FREERTOS_AUTO_INIT           0

/*
 * The layer's argument checks (configASSERT()) follow the build type, and
 * the build type of this firmware turns them off (NDEBUG): some of them
 * are stricter than FreeRTOS, among them a queue item of at least 4 bytes,
 * which the layer's queues (items copied with memcpy()) do not need.  Its
 * internal failures (TX_FREERTOS_ASSERT_FAIL(): a ThreadX service that
 * failed where it cannot, such as freeing to the byte pool) always end in
 * the fault path, as a failed assertion (port/panic.c).
 */
#define configASSERT(x)                 assert(x)
#define TX_FREERTOS_ASSERT_FAIL()       __assert_func(__FILE__, __LINE__, __func__, "TX_FREERTOS_ASSERT_FAIL()")

/*
 * The port macros the layer leaves to the configuration: an interrupt
 * lockout of everything with a C handler (levels 1 .. XCHAL_EXCM_LEVEL, as
 * ThreadX's TX_DISABLE), and its end.
 */
#define portDISABLE_INTERRUPTS()        ((void) XTOS_SET_INTLEVEL(XCHAL_EXCM_LEVEL))
#define portENABLE_INTERRUPTS()         ((void) XTOS_SET_INTLEVEL(0))
#define taskENTER_CRITICAL_FROM_ISR()   XTOS_SET_MIN_INTLEVEL(XCHAL_EXCM_LEVEL)
#define taskEXIT_CRITICAL_FROM_ISR(x)   XTOS_RESTORE_INTLEVEL(x)

/*
 * Critical sections, the one implementation behind stock FreeRTOS's
 * taskENTER_CRITICAL() and ESP-IDF's portENTER_CRITICAL(&mux) and their
 * _ISR and _SAFE forms (freertos/FreeRTOS.h).  The macros take the stock
 * form (no argument) and ESP-IDF's (a spinlock, portMUX_TYPE, evaluated
 * and otherwise ignored).
 *
 * With one core a critical section excludes every other one and the lock
 * itself carries no state: one nesting depth and the PS saved by the
 * outermost entry serve them all (freertos_idf.c).  Entering masks the
 * interrupts up to XCHAL_EXCM_LEVEL (every interrupt with a C handler, what
 * ThreadX's TX_DISABLE masks), never lowering a higher level, and the
 * outermost entry also disables ThreadX's preemption
 * (_tx_thread_preempt_disable), as FreeRTOS does no switch inside: a
 * service that readies a more urgent thread inside the section (a
 * semaphore or queue given, also through the _FromISR forms the Wi-Fi
 * driver uses with interrupts masked) leaves the switch pending.  The
 * outermost exit restores the PS, enables preemption again and makes that
 * switch, but only where the caller may switch: in a thread whose restored
 * PS has the interrupts open.  In an interrupt handler ThreadX switches
 * when the outermost handler returns; before the kernel runs and in
 * tx_application_define() there is nothing to switch to; with the
 * interrupts masked by the caller the switch waits for the next interrupt
 * (the tick at the latest).  So a section is never left in another thread,
 * the saved PS is always the entering thread's, and the same code serves
 * threads, interrupt handlers, the start-up and the fault path, as the
 * _ISR and _SAFE forms of ESP-IDF do.  Nothing that can block may be
 * called inside: a ThreadX service that would wait returns its error at
 * once while preemption is disabled.  Inline, for the code that runs from
 * RAM while the flash is busy.
 */
extern uint32_t esp_compat_critical_saved_ps;
extern uint32_t esp_compat_critical_nesting;

/* ThreadX's (tx_thread.h; freertos_idf.c includes it, which checks these against it). */
extern volatile UINT _tx_thread_preempt_disable;
extern TX_THREAD *_tx_thread_current_ptr;
extern TX_THREAD *_tx_thread_execute_ptr;
VOID _tx_thread_system_preempt_check(VOID);

/* The argument is the optional spinlock, which only has to be evaluated. */
static inline __attribute__((always_inline)) void esp_compat_critical_enter(int unused, ...)
{
    uint32_t ps = XTOS_SET_MIN_INTLEVEL(XCHAL_EXCM_LEVEL);

    (void) unused;
    if (esp_compat_critical_nesting++ == 0u) {
        esp_compat_critical_saved_ps = ps;
        _tx_thread_preempt_disable++;
    }
}

static inline __attribute__((always_inline)) void esp_compat_critical_exit(int unused, ...)
{
    uint32_t ps;

    (void) unused;
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

#define portENTER_CRITICAL(...)         esp_compat_critical_enter(0, ##__VA_ARGS__)
#define portEXIT_CRITICAL(...)          esp_compat_critical_exit(0, ##__VA_ARGS__)
#define taskENTER_CRITICAL(...)         portENTER_CRITICAL(__VA_ARGS__)
#define taskEXIT_CRITICAL(...)          portEXIT_CRITICAL(__VA_ARGS__)

#endif
