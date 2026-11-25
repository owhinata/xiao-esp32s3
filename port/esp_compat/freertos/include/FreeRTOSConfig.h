/*
 * Configuration of ThreadX's FreeRTOS compatibility layer
 * (lib/threadx/utility/rtos_compatibility_layers/FreeRTOS, built
 * unmodified).  Only the options the layer reads are here; the layer's
 * readme.md lists them.  Included by the layer's FreeRTOS.h after tx_api.h,
 * so the ThreadX options (tx_user.h) and the Xtensa interrupt macros of
 * tx_port.h are visible.
 *
 * What ESP-IDF code expects on top of stock FreeRTOS is not here but in
 * freertos/FreeRTOS.h, the header ESP-IDF sources include.
 */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <assert.h>

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
 * Objects the layer creates dynamically (xTaskCreate(): TCB and stack,
 * xQueueCreate(), xSemaphoreCreate*(), xEventGroupCreate(), xTimerCreate())
 * come from a ThreadX byte pool over a static array of this size, not from
 * the heap: the layer has no hook for another allocator, and its task
 * deletion frees through the pool.  Sized for the two tasks there are, each
 * a 4096-byte stack, an 840-byte task block and 8 bytes of the pool's
 * bookkeeping for each block: the esp_timer task, and "main"
 * (src/main.c), which ends after the start-up and leaves its room to
 * whatever comes later; about 300 bytes spare.  Each further FreeRTOS
 * object needs room here.  ESP-IDF code that calls malloc() or
 * heap_caps_malloc() itself uses ESP-IDF's heap.
 */
#define configTOTAL_HEAP_SIZE           (10u * 1024u)

/*
 * vTaskDelete() and returning from a task: the layer's own thread
 * "txfr_idle_task" (ThreadX priority 31) deletes the task and frees what it
 * allocated.  Its stack, in StackType_t.
 */
#define INCLUDE_vTaskDelete             1
#define TX_FREERTOS_IDLE_STACK          (1536u / 4u)

/* Started from tx_application_define() (tx_freertos_init()). */
#define TX_FREERTOS_AUTO_INIT           0

/* Argument checks and internal failures, like ESP-IDF's configASSERT(). */
#define configASSERT(x)                 assert(x)
#define TX_FREERTOS_ASSERT_FAIL()       assert(0)

/*
 * The port macros the layer leaves to the configuration: an interrupt
 * lockout of everything with a C handler (levels 1 .. XCHAL_EXCM_LEVEL, as
 * ThreadX's TX_DISABLE), and its end.  Only the layer's vPortEnterCritical()
 * and vPortExitCritical() use the first two; ESP-IDF code reaches the
 * critical sections of freertos/FreeRTOS.h instead.
 */
#define portDISABLE_INTERRUPTS()        ((void) XTOS_SET_INTLEVEL(XCHAL_EXCM_LEVEL))
#define portENABLE_INTERRUPTS()         ((void) XTOS_SET_INTLEVEL(0))
#define taskENTER_CRITICAL_FROM_ISR()   XTOS_SET_MIN_INTLEVEL(XCHAL_EXCM_LEVEL)
#define taskEXIT_CRITICAL_FROM_ISR(x)   XTOS_RESTORE_INTLEVEL(x)

#endif
