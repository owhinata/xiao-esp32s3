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
 * deletion frees through the pool.  Each block costs 8 bytes of the pool's
 * bookkeeping on top; a task is its stack and an 856-byte task block, a
 * semaphore or mutex 96 bytes, a queue 96 bytes and its items, an event
 * group 44 bytes.  The tasks: esp_timer (4096-byte stack), "main"
 * (src/main.c, 4096, ends after the start-up), the Wi-Fi driver's
 * (6656, WIFI_TASK_STACK_SIZE), the default event loop's "sys_evt"
 * (2816), lwIP's "tcpip" (3584); 26.8 KiB with their task blocks.  When
 * "main" ends, only its task block goes back: the layer's task deletion
 * frees the block and not the stack xTaskCreate() allocated, so about 26 KiB
 * stay taken.  The objects: the event loop's queue (32
 * items of 16 bytes) and mutex, the driver's queues, semaphores, mutexes
 * and event groups, lwIP's mutex (sys_arch_protect()), and one semaphore
 * for each thread that calls the driver (the OS adapter's
 * wifi_thread_semphr_get): a few KiB, not documented (the Wi-Fi start
 * alone took 23.4 KiB, lwIP adds 4.6 KiB).  36 KiB leaves about 8 KiB for
 * those and the threads that call the driver later; port/net/wifi.c logs
 * how much the start took from the pool, which is the measure to size it
 * by.  The queues and semaphores the OS adapter makes with
 * xQueueCreateStatic() (its _queue_create), lwIP's mailboxes and
 * semaphores (static ones in ESP-IDF's heap) and all of ESP-IDF's
 * malloc() and heap_caps_malloc() use ESP-IDF's heap.
 */
#define configTOTAL_HEAP_SIZE           (36u * 1024u)

/*
 * vTaskDelete() and returning from a task: the layer's own thread
 * "txfr_idle_task" (ThreadX priority 31) deletes the task and frees its
 * task block (not its stack, see above).  Its stack, in StackType_t.
 */
#define INCLUDE_vTaskDelete             1
#define TX_FREERTOS_IDLE_STACK          (1536u / 4u)

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
 * ThreadX's TX_DISABLE), and its end.  Only the layer's vPortEnterCritical()
 * and vPortExitCritical() use the first two; ESP-IDF code reaches the
 * critical sections of freertos/FreeRTOS.h instead.
 */
#define portDISABLE_INTERRUPTS()        ((void) XTOS_SET_INTLEVEL(XCHAL_EXCM_LEVEL))
#define portENABLE_INTERRUPTS()         ((void) XTOS_SET_INTLEVEL(0))
#define taskENTER_CRITICAL_FROM_ISR()   XTOS_SET_MIN_INTLEVEL(XCHAL_EXCM_LEVEL)
#define taskEXIT_CRITICAL_FROM_ISR(x)   XTOS_RESTORE_INTLEVEL(x)

#endif
