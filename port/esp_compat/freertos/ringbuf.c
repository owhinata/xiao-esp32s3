/*
 * ESP-IDF's ring buffer API (components/esp_ringbuf, its header
 * freertos/ringbuf.h unmodified), the part that esp-usb's usb_host_uac
 * calls: byte buffers only, created, written, read up to a size, returned,
 * queried for the bytes waiting, deleted.  ESP-IDF's ringbuf.c is built on
 * FreeRTOS's internal event lists, which ThreadX's FreeRTOS layer does not
 * have; this one keeps the same semantics for these calls on a critical
 * section (FreeRTOSConfig.h) and a ThreadX event flags group.  Every other
 * function of the header (the item buffers, static and ISR variants, queue
 * sets) is left undefined, so a use of one fails at the link.
 *
 * A byte buffer, as in ESP-IDF: a contiguous array of xBufferSize bytes
 * holding a sequence of bytes, no item boundaries and no headers.
 *
 *   xRingbufferSend()         copies all xItemSize bytes in (split at the
 *                             end of the array when needed), waiting up to
 *                             xTicksToWait for the room; an item larger than
 *                             the buffer fails at once.
 *   xRingbufferReceiveUpTo()  lends the oldest waiting bytes in place, at
 *                             most xMaxSize and never across the end of the
 *                             array (a second call gets the rest), waiting up
 *                             to xTicksToWait for data; NULL for xMaxSize 0
 *                             or on the timeout.  Until the loan is returned
 *                             with vRingbufferReturnItem(), its bytes still
 *                             take room and nothing more can be received.
 *   vRingbufferGetInfo()      the offsets of ESP-IDF's free, read, write and
 *                             acquire pointers, and the bytes waiting (not
 *                             counting a loan).
 *
 * The critical section masks the interrupts and ThreadX's preemption for
 * the bookkeeping and the copy of one item: the callers' items are the
 * isochronous packets of an audio stream (at most wMaxPacketSize, 1023
 * bytes at full speed), a copy of about a microsecond.  For threads only
 * (FreeRTOS tasks or not): the waits are ThreadX ones, and no call comes
 * from an interrupt handler (usb_host_uac calls them from its client task
 * and from its users' tasks).  Deleting a buffer that a thread waits on is
 * not allowed, as in ESP-IDF (usb_host_uac wakes its readers and waits
 * before it deletes).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "tx_api.h"

#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

/* Event flags: data or room may have come. */
#define RB_EVENT_DATA   0x1u
#define RB_EVENT_ROOM   0x2u

typedef struct {
    uint8_t *storage;
    size_t size;
    size_t head;                /* offset of the oldest byte not returned (ESP-IDF's free pointer) */
    size_t lent;                /* bytes from head on that a receiver holds */
    size_t waiting;             /* bytes after the loan, not received yet */
    TX_EVENT_FLAGS_GROUP events;
} ringbuf_t;

static size_t wrap(const ringbuf_t *rb, size_t off)
{
    return off >= rb->size ? off - rb->size : off;
}

/* Inside the critical section: clears the flag before the caller waits on it. */
static void flag_clear(ringbuf_t *rb, ULONG flag)
{
    (void) tx_event_flags_set(&rb->events, ~flag, TX_AND);
}

static void flag_set(ringbuf_t *rb, ULONG flag)
{
    (void) tx_event_flags_set(&rb->events, flag, TX_OR);
}

/*
 * Waits for the flag at most until the deadline (ticks of tx_time_get()).
 * false when the time is up.
 */
static bool flag_wait(ringbuf_t *rb, ULONG flag, TickType_t ticks, ULONG start)
{
    ULONG actual;
    ULONG wait;

    if (ticks == 0u) {
        return false;
    }
    if (ticks == portMAX_DELAY) {
        wait = TX_WAIT_FOREVER;
    } else {
        ULONG elapsed = tx_time_get() - start;

        if (elapsed >= ticks) {
            return false;
        }
        wait = ticks - elapsed;
    }
    return tx_event_flags_get(&rb->events, flag, TX_OR, &actual, wait) == TX_SUCCESS;
}

RingbufHandle_t xRingbufferCreate(size_t xBufferSize, RingbufferType_t xBufferType)
{
    ringbuf_t *rb;

    if (xBufferType != RINGBUF_TYPE_BYTEBUF || xBufferSize == 0u) {
        return NULL;
    }
    rb = calloc(1, sizeof(*rb));
    if (rb == NULL) {
        return NULL;
    }
    rb->storage = malloc(xBufferSize);
    if (rb->storage == NULL) {
        free(rb);
        return NULL;
    }
    rb->size = xBufferSize;
    if (tx_event_flags_create(&rb->events, "ringbuf") != TX_SUCCESS) {
        free(rb->storage);
        free(rb);
        return NULL;
    }
    return rb;
}

void vRingbufferDelete(RingbufHandle_t xRingbuffer)
{
    ringbuf_t *rb = xRingbuffer;

    if (rb == NULL) {
        return;
    }
    (void) tx_event_flags_delete(&rb->events);
    free(rb->storage);
    free(rb);
}

BaseType_t xRingbufferSend(RingbufHandle_t xRingbuffer, const void *pvItem, size_t xItemSize,
                           TickType_t xTicksToWait)
{
    ringbuf_t *rb = xRingbuffer;
    ULONG start = tx_time_get();

    if (xItemSize > rb->size) {
        return pdFALSE;
    }
    if (xItemSize == 0u) {
        return pdTRUE;
    }
    for (;;) {
        portENTER_CRITICAL();
        if (rb->size - rb->lent - rb->waiting >= xItemSize) {
            size_t at = wrap(rb, rb->head + rb->lent + rb->waiting);
            size_t first = rb->size - at < xItemSize ? rb->size - at : xItemSize;

            memcpy(&rb->storage[at], pvItem, first);
            memcpy(rb->storage, (const uint8_t *) pvItem + first, xItemSize - first);
            rb->waiting += xItemSize;
            flag_set(rb, RB_EVENT_DATA);
            portEXIT_CRITICAL();
            return pdTRUE;
        }
        flag_clear(rb, RB_EVENT_ROOM);
        portEXIT_CRITICAL();
        if (!flag_wait(rb, RB_EVENT_ROOM, xTicksToWait, start)) {
            return pdFALSE;
        }
    }
}

void *xRingbufferReceiveUpTo(RingbufHandle_t xRingbuffer, size_t *pxItemSize, TickType_t xTicksToWait,
                             size_t xMaxSize)
{
    ringbuf_t *rb = xRingbuffer;
    ULONG start = tx_time_get();

    if (xMaxSize == 0u) {
        return NULL;
    }
    for (;;) {
        portENTER_CRITICAL();
        if (rb->lent == 0u && rb->waiting > 0u) {
            size_t to_end = rb->size - rb->head;
            size_t n = rb->waiting < xMaxSize ? rb->waiting : xMaxSize;
            void *item = &rb->storage[rb->head];

            if (n > to_end) {
                n = to_end;
            }
            rb->lent = n;
            rb->waiting -= n;
            portEXIT_CRITICAL();
            *pxItemSize = n;
            return item;
        }
        flag_clear(rb, RB_EVENT_DATA);
        portEXIT_CRITICAL();
        if (!flag_wait(rb, RB_EVENT_DATA, xTicksToWait, start)) {
            return NULL;
        }
    }
}

void vRingbufferReturnItem(RingbufHandle_t xRingbuffer, void *pvItem)
{
    ringbuf_t *rb = xRingbuffer;

    portENTER_CRITICAL();
    if (rb->lent != 0u && pvItem == &rb->storage[rb->head]) {
        rb->head = wrap(rb, rb->head + rb->lent);
        rb->lent = 0;
        flag_set(rb, RB_EVENT_ROOM);
        if (rb->waiting > 0u) {
            /* A receiver may wait for the loan to end. */
            flag_set(rb, RB_EVENT_DATA);
        }
    }
    portEXIT_CRITICAL();
}

void vRingbufferGetInfo(RingbufHandle_t xRingbuffer, UBaseType_t *uxFree, UBaseType_t *uxRead,
                        UBaseType_t *uxWrite, UBaseType_t *uxAcquire, UBaseType_t *uxItemsWaiting)
{
    ringbuf_t *rb = xRingbuffer;
    size_t write;

    portENTER_CRITICAL();
    write = wrap(rb, rb->head + rb->lent + rb->waiting);
    if (uxFree != NULL) {
        *uxFree = rb->head;
    }
    if (uxRead != NULL) {
        *uxRead = wrap(rb, rb->head + rb->lent);
    }
    if (uxWrite != NULL) {
        *uxWrite = write;
    }
    if (uxAcquire != NULL) {
        *uxAcquire = write;
    }
    if (uxItemsWaiting != NULL) {
        *uxItemsWaiting = rb->waiting;
    }
    portEXIT_CRITICAL();
}
