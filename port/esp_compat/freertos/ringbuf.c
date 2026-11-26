/*
 * ESP-IDF's ring buffer API (components/esp_ringbuf, its header
 * freertos/ringbuf.h unmodified), the part that esp-usb's usb_host_uac
 * calls: byte buffers only, created, written, read up to a size, returned,
 * queried for the bytes waiting, deleted.  ESP-IDF's ringbuf.c is built on
 * FreeRTOS's internal event lists, which ThreadX's FreeRTOS layer does not
 * have; this one keeps the same semantics for these calls on a critical
 * section (FreeRTOSConfig.h) and lists of waiting threads.  Every other
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
 * A thread that has to wait checks its request and puts itself on the list
 * of senders or of receivers in one critical section, then waits on a
 * ThreadX semaphore of its own.  A change that can satisfy a request (data
 * sent; a loan returned, which makes room and may end a receiver's wait)
 * takes every waiter of that kind off its list and wakes it, in the
 * critical section of the change: each re-checks its own request, and one
 * whose request still does not fit puts itself back.  So no wake-up is lost
 * between a check and the wait, and no waiter takes one that another
 * needed.  The timeout is a deadline from the call across the re-waits.
 *
 * The critical section masks the interrupts and ThreadX's preemption for
 * the bookkeeping, the wake-ups (one semaphore put per waiter) and the copy
 * of one item: the callers' items are the isochronous packets of an audio
 * stream (at most wMaxPacketSize, 1023 bytes at full speed), a copy of about
 * a microsecond.  For threads only
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

/* A thread waiting for room (sender) or data (receiver). */
typedef struct waiter {
    struct waiter *next;
    TX_SEMAPHORE wake;
} waiter_t;

typedef struct {
    uint8_t *storage;
    size_t size;
    size_t head;                /* offset of the oldest byte not returned (ESP-IDF's free pointer) */
    size_t lent;                /* bytes from head on that a receiver holds */
    size_t waiting;             /* bytes after the loan, not received yet */
    waiter_t *senders;          /* waiting for room */
    waiter_t *receivers;        /* waiting for data, or for the loan to end */
} ringbuf_t;

static size_t wrap(const ringbuf_t *rb, size_t off)
{
    return off >= rb->size ? off - rb->size : off;
}

/* Inside the critical section: wakes every waiter of the list and empties it. */
static void wake_all(waiter_t **list)
{
    waiter_t *w = *list;

    *list = NULL;
    while (w != NULL) {
        waiter_t *next = w->next;

        (void) tx_semaphore_put(&w->wake);
        w = next;
    }
}

/* Inside the critical section: takes the waiter off the list if it is still on it. */
static void unlist(waiter_t **list, waiter_t *w)
{
    for (; *list != NULL; list = &(*list)->next) {
        if (*list == w) {
            *list = w->next;
            return;
        }
    }
}

/*
 * The ticks left until the deadline (start + ticks of tx_time_get()), 0 when
 * the time is up.
 */
static ULONG ticks_left(TickType_t ticks, ULONG start)
{
    ULONG elapsed;

    if (ticks == portMAX_DELAY) {
        return TX_WAIT_FOREVER;
    }
    elapsed = tx_time_get() - start;
    return elapsed >= ticks ? 0u : ticks - elapsed;
}

/*
 * Inside the critical section, after a failed check: puts the waiter on the
 * list and leaves the critical section, waits at most left ticks, and takes
 * the waiter off the list again.  The caller re-checks its request.
 */
static void wait_on(waiter_t **list, waiter_t *w, ULONG left)
{
    w->next = *list;
    *list = w;
    portEXIT_CRITICAL();
    (void) tx_semaphore_get(&w->wake, left);
    portENTER_CRITICAL();
    unlist(list, w);
}

/*
 * The waiter's semaphore, created on the first wait of a call: false when it
 * cannot be.  A wake-up given after the waiter left the list on its timeout
 * stays in the count and only makes a later wait re-check early.
 */
static bool waiter_open(waiter_t *w, bool *open)
{
    if (!*open) {
        if (tx_semaphore_create(&w->wake, "ringbuf", 0u) != TX_SUCCESS) {
            return false;
        }
        *open = true;
    }
    return true;
}

static void waiter_close(waiter_t *w, bool open)
{
    if (open) {
        (void) tx_semaphore_delete(&w->wake);
    }
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
    return rb;
}

void vRingbufferDelete(RingbufHandle_t xRingbuffer)
{
    ringbuf_t *rb = xRingbuffer;

    if (rb == NULL) {
        return;
    }
    free(rb->storage);
    free(rb);
}

BaseType_t xRingbufferSend(RingbufHandle_t xRingbuffer, const void *pvItem, size_t xItemSize,
                           TickType_t xTicksToWait)
{
    ringbuf_t *rb = xRingbuffer;
    ULONG start = tx_time_get();
    waiter_t self;
    bool open = false;
    BaseType_t ret;

    if (xItemSize > rb->size) {
        return pdFALSE;
    }
    if (xItemSize == 0u) {
        return pdTRUE;
    }
    for (;;) {
        ULONG left;

        portENTER_CRITICAL();
        if (rb->size - rb->lent - rb->waiting >= xItemSize) {
            size_t at = wrap(rb, rb->head + rb->lent + rb->waiting);
            size_t first = rb->size - at < xItemSize ? rb->size - at : xItemSize;

            memcpy(&rb->storage[at], pvItem, first);
            memcpy(rb->storage, (const uint8_t *) pvItem + first, xItemSize - first);
            rb->waiting += xItemSize;
            wake_all(&rb->receivers);
            portEXIT_CRITICAL();
            ret = pdTRUE;
            break;
        }
        left = ticks_left(xTicksToWait, start);
        if (left == 0u || !open) {
            portEXIT_CRITICAL();
            if (left == 0u || !waiter_open(&self, &open)) {
                ret = pdFALSE;
                break;
            }
            continue;
        }
        wait_on(&rb->senders, &self, left);
        portEXIT_CRITICAL();
    }
    waiter_close(&self, open);
    return ret;
}

void *xRingbufferReceiveUpTo(RingbufHandle_t xRingbuffer, size_t *pxItemSize, TickType_t xTicksToWait,
                             size_t xMaxSize)
{
    ringbuf_t *rb = xRingbuffer;
    ULONG start = tx_time_get();
    waiter_t self;
    bool open = false;
    void *item = NULL;

    if (xMaxSize == 0u) {
        return NULL;
    }
    for (;;) {
        ULONG left;

        portENTER_CRITICAL();
        if (rb->lent == 0u && rb->waiting > 0u) {
            size_t to_end = rb->size - rb->head;
            size_t n = rb->waiting < xMaxSize ? rb->waiting : xMaxSize;

            item = &rb->storage[rb->head];
            if (n > to_end) {
                n = to_end;
            }
            rb->lent = n;
            rb->waiting -= n;
            portEXIT_CRITICAL();
            *pxItemSize = n;
            break;
        }
        left = ticks_left(xTicksToWait, start);
        if (left == 0u || !open) {
            portEXIT_CRITICAL();
            if (left == 0u || !waiter_open(&self, &open)) {
                break;
            }
            continue;
        }
        wait_on(&rb->receivers, &self, left);
        portEXIT_CRITICAL();
    }
    waiter_close(&self, open);
    return item;
}

void vRingbufferReturnItem(RingbufHandle_t xRingbuffer, void *pvItem)
{
    ringbuf_t *rb = xRingbuffer;

    portENTER_CRITICAL();
    if (rb->lent != 0u && pvItem == &rb->storage[rb->head]) {
        rb->head = wrap(rb, rb->head + rb->lent);
        rb->lent = 0;
        wake_all(&rb->senders);
        /* A receiver may wait for the loan to end. */
        if (rb->waiting > 0u) {
            wake_all(&rb->receivers);
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
