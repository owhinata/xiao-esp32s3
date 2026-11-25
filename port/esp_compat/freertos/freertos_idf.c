/*
 * The parts of ESP-IDF's FreeRTOS API that ThreadX's FreeRTOS compatibility
 * layer (tx_freertos.c) does not have, on top of it.  Declared in
 * include/freertos/FreeRTOS.h, which says what each one assumes.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "tx_api.h"
#include "tx_initialize.h"
#include "tx_thread.h"
#include "tx_byte_pool.h"

#include "freertos/FreeRTOS.h"
#include "../pthread_local.h"

/* State of the critical sections: the interrupt level to return to and the nesting depth. */
uint32_t esp_compat_critical_saved_ps;
uint32_t esp_compat_critical_nesting;

BaseType_t xPortInIsrContext(void)
{
    ULONG state = TX_THREAD_GET_SYSTEM_STATE();

    return (state != 0u && state < TX_INITIALIZE_IN_PROGRESS) ? pdTRUE : pdFALSE;
}

/* Bytes to StackType_t, rounded up. */
static configSTACK_DEPTH_TYPE stack_words(uint32_t bytes)
{
    return (configSTACK_DEPTH_TYPE) ((bytes + sizeof(StackType_t) - 1u) / sizeof(StackType_t));
}

/*
 * The layer's functions, called by name in parentheses: the names are
 * macros for these wrappers in freertos/FreeRTOS.h.  The layer insists on a
 * place for the handle; ESP-IDF allows NULL.
 */
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t pxTaskCode,
                                   const char *const pcName,
                                   const uint32_t ulStackDepth,
                                   void *const pvParameters,
                                   UBaseType_t uxPriority,
                                   TaskHandle_t *const pxCreatedTask,
                                   const BaseType_t xCoreID)
{
    TaskHandle_t handle;

    (void) xCoreID;
    return (xTaskCreate)(pxTaskCode, pcName, stack_words(ulStackDepth), pvParameters,
                         uxPriority, pxCreatedTask != NULL ? pxCreatedTask : &handle);
}

TaskHandle_t xTaskCreateStaticPinnedToCore(TaskFunction_t pxTaskCode,
                                           const char *const pcName,
                                           const uint32_t ulStackDepth,
                                           void *const pvParameters,
                                           UBaseType_t uxPriority,
                                           StackType_t *const puxStackBuffer,
                                           StaticTask_t *const pxTaskBuffer,
                                           const BaseType_t xCoreID)
{
    (void) xCoreID;
    return (xTaskCreateStatic)(pxTaskCode, pcName, stack_words(ulStackDepth), pvParameters,
                               uxPriority, puxStackBuffer, pxTaskBuffer);
}

/* The layer's functions; freertos/FreeRTOS.h redirects their names here. */
#undef xTaskGetCurrentTaskHandle
#undef vTaskDelete

TaskHandle_t esp_compat_task_get_current(void)
{
    TX_THREAD *thread = tx_thread_identify();

    return thread != TX_NULL ? (TaskHandle_t) thread->txfr_thread_ptr : NULL;
}

void esp_compat_task_delete(TaskHandle_t xTask)
{
    if (xTask == NULL || xTask == esp_compat_task_get_current()) {
        esp_compat_pthread_exit_destructors();
    }
    vTaskDelete(xTask);
}

BaseType_t xQueueGenericSend(QueueHandle_t xQueue,
                             const void *const pvItemToQueue,
                             TickType_t xTicksToWait,
                             const BaseType_t xCopyPosition)
{
    switch (xCopyPosition) {
    case queueSEND_TO_FRONT:
        return xQueueSendToFront(xQueue, pvItemToQueue, xTicksToWait);
    case queueOVERWRITE:
        return xQueueOverwrite(xQueue, pvItemToQueue);
    default:
        return xQueueSendToBack(xQueue, pvItemToQueue, xTicksToWait);
    }
}

/*
 * A static queue's control block is the caller's StaticQueue_t itself and
 * its storage the buffer it was given (xQueueCreateStatic()).
 */
BaseType_t xQueueGetStaticBuffers(QueueHandle_t xQueue,
                                  uint8_t **ppucQueueStorage,
                                  StaticQueue_t **ppxStaticQueue)
{
    if (xQueue == NULL || xQueue->allocated != 0u) {
        return pdFALSE;
    }
    if (ppucQueueStorage != NULL) {
        *ppucQueueStorage = xQueue->p_mem;
    }
    if (ppxStaticQueue != NULL) {
        *ppxStaticQueue = xQueue;
    }
    return pdTRUE;
}

/*
 * The layer's byte pool is static in tx_freertos.c; it is found by its
 * name among the kernel's byte pools.
 */
size_t esp_compat_freertos_pool_free(void)
{
    TX_BYTE_POOL *pool = _tx_byte_pool_created_ptr;
    ULONG n;
    ULONG available = 0;

    for (n = _tx_byte_pool_created_count; n > 0u && pool != TX_NULL; n--) {
        if (pool->tx_byte_pool_name != TX_NULL
            && strcmp(pool->tx_byte_pool_name, "txfr_byte_pool") == 0) {
            (void) tx_byte_pool_info_get(pool, TX_NULL, &available, TX_NULL, TX_NULL, TX_NULL, TX_NULL);
            break;
        }
        pool = pool->tx_byte_pool_created_next;
    }
    return (size_t) available;
}
