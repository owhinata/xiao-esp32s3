/*
 * Stand-in for the allocators of ESP-IDF's heap component that the compiled
 * Espressif sources call (esp_timer.c, intr_alloc.c).  ESP-IDF's heap keeps
 * one region per memory type and picks one by capability; here the only
 * heap is newlib's (port/threadx/src/tx_clib_newlib.c), in internal SRAM on
 * the data bus.  That memory is internal, byte- and word-addressable, usable
 * by DMA and what plain malloc() returns, so a request for exactly those
 * capabilities is served by malloc(); any other (executable, PSRAM, RTC
 * memory, ...) fails as it would with no such region.  Like malloc(): from
 * threads and tx_application_define() only, never from interrupt handlers.
 */
#include <stdint.h>
#include <stdlib.h>

#include "esp_heap_caps.h"

#define CAPS_OF_NEWLIB_HEAP \
    (MALLOC_CAP_8BIT | MALLOC_CAP_32BIT | MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT)

void *heap_caps_malloc(size_t size, uint32_t caps)
{
    if ((caps & ~CAPS_OF_NEWLIB_HEAP) != 0u) {
        return NULL;
    }
    return malloc(size);
}

void *heap_caps_calloc(size_t n, size_t size, uint32_t caps)
{
    if ((caps & ~CAPS_OF_NEWLIB_HEAP) != 0u) {
        return NULL;
    }
    return calloc(n, size);
}
