/*
 * Thread-specific data on ThreadX threads (pthread_local.c).
 */
#ifndef ESP_COMPAT_PTHREAD_LOCAL_H
#define ESP_COMPAT_PTHREAD_LOCAL_H

#include "tx_api.h"

/*
 * Runs the destructors of a thread's values, as when a POSIX thread exits;
 * for a thread that has ended and is not deleted yet, from another thread.
 */
void esp_compat_pthread_destructors(TX_THREAD *thread);

#endif
