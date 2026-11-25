/*
 * Thread-specific data (pthread_key_create(), pthread_getspecific(),
 * pthread_setspecific(), pthread_key_delete()) on ThreadX threads, in
 * place of ESP-IDF's pthread component (pthread_local_storage.c), which
 * keeps the values in FreeRTOS's thread-local storage pointers; the
 * FreeRTOS layer has none.  The Wi-Fi driver's OS adapter keeps a
 * semaphore per calling thread this way (wifi_thread_semphr_get_wrapper()
 * in esp_wifi/esp32s3/esp_adapter.c).
 *
 * Every ThreadX thread has one value per key in its control block
 * (tx_thread_pthread_local, port/threadx/inc/tx_user.h), cleared when the
 * thread is created, so there is nothing to allocate.  At most
 * TX_PTHREAD_KEYS_MAX keys exist at a time.  Only threads have values:
 * outside a thread getspecific returns NULL and setspecific fails.
 *
 * Destructors run when a FreeRTOS task deletes itself (vTaskDelete(NULL),
 * port/esp_compat/freertos/freertos_idf.c), in the task, before it ends,
 * as POSIX runs them at a thread's exit: for every key whose value is not
 * NULL, the value is cleared and the destructor called with it, repeated
 * while destructors set new values (PTHREAD_DESTRUCTOR_ITERATIONS times at
 * most).  A thread that is deleted by another, or that ends otherwise,
 * does not run them; that is the case of no thread here that uses a key.
 */
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

#include "tx_api.h"
#include "tx_thread.h"

#include "pthread_local.h"

#ifndef PTHREAD_DESTRUCTOR_ITERATIONS
#define PTHREAD_DESTRUCTOR_ITERATIONS   4
#endif

/* Keys are 1 .. TX_PTHREAD_KEYS_MAX: a key of 0 is never valid. */
static struct {
    bool used;
    void (*destructor)(void *);
} keys[TX_PTHREAD_KEYS_MAX];

static bool key_valid(pthread_key_t key)
{
    return key >= 1u && key <= TX_PTHREAD_KEYS_MAX && keys[key - 1u].used;
}

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *))
{
    TX_INTERRUPT_SAVE_AREA
    unsigned int i;

    TX_DISABLE
    for (i = 0; i < TX_PTHREAD_KEYS_MAX; i++) {
        if (!keys[i].used) {
            keys[i].used = true;
            keys[i].destructor = destructor;
            break;
        }
    }
    TX_RESTORE
    if (i == TX_PTHREAD_KEYS_MAX) {
        return EAGAIN;
    }
    *key = (pthread_key_t) (i + 1u);
    return 0;
}

/*
 * The values the key had in the threads stay where they are, unseen, and
 * are cleared when the key is handed out again (POSIX leaves them to the
 * application).
 */
int pthread_key_delete(pthread_key_t key)
{
    TX_INTERRUPT_SAVE_AREA
    TX_THREAD *thread;
    ULONG n;

    if (!key_valid(key)) {
        return EINVAL;
    }
    TX_DISABLE
    keys[key - 1u].used = false;
    keys[key - 1u].destructor = NULL;
    thread = _tx_thread_created_ptr;
    for (n = _tx_thread_created_count; n > 0u && thread != TX_NULL; n--) {
        thread->tx_thread_pthread_local[key - 1u] = NULL;
        thread = thread->tx_thread_created_next;
    }
    TX_RESTORE
    return 0;
}

void *pthread_getspecific(pthread_key_t key)
{
    TX_THREAD *self = tx_thread_identify();

    if (self == TX_NULL || TX_THREAD_GET_SYSTEM_STATE() != 0u || !key_valid(key)) {
        return NULL;
    }
    return self->tx_thread_pthread_local[key - 1u];
}

int pthread_setspecific(pthread_key_t key, const void *value)
{
    TX_THREAD *self = tx_thread_identify();

    if (self == TX_NULL || TX_THREAD_GET_SYSTEM_STATE() != 0u || !key_valid(key)) {
        return EINVAL;
    }
    self->tx_thread_pthread_local[key - 1u] = (void *) value;
    return 0;
}

void esp_compat_pthread_exit_destructors(void)
{
    TX_THREAD *self = tx_thread_identify();
    int round;
    bool again = true;

    if (self == TX_NULL) {
        return;
    }
    for (round = 0; again && round < PTHREAD_DESTRUCTOR_ITERATIONS; round++) {
        again = false;
        for (unsigned int i = 0; i < TX_PTHREAD_KEYS_MAX; i++) {
            void *value = self->tx_thread_pthread_local[i];

            if (value == NULL) {
                continue;
            }
            self->tx_thread_pthread_local[i] = NULL;
            if (keys[i].used && keys[i].destructor != NULL) {
                keys[i].destructor(value);
                again = true;
            }
        }
    }
}
