/*
 * Thread-safe newlib support for the ThreadX Xtensa port, adapted to the
 * newlib that ships with the Espressif GCC toolchain.
 *
 * This replaces ports/xtensa/xcc/src/tx_clib_lock.c from ThreadX (MIT
 * license, Copyright (c) 2024 Microsoft Corporation and Cadence Design
 * Systems), which targets the Xtensa Tools newlib and does not build here:
 *
 *   - Espressif newlib is configured with _REENT_SMALL and __DYNAMIC_REENT__,
 *     so there is no per-reent __sf[] array and no _wrapup_reent().
 *   - It is configured with _RETARGETABLE_LOCKING, so the locks are the
 *     __retarget_lock_*() family instead of __malloc_lock()/__env_lock().
 *     Implementing that family also makes stdio streams thread-safe, which
 *     the original (malloc and env only) did not.
 *
 * The interface towards the rest of the port is unchanged: _tx_clib_init(),
 * _tx_clib_thread_setup(), _tx_clib_reent_init(), _tx_clib_reent_cleanup(),
 * and the per-thread reent pointer that the scheduler copies into
 * _impure_ptr on every context switch.  There is no heap here: malloc() and
 * the rest of the family are ESP-IDF's (esp_libc heap.c on heap_caps), so
 * newlib's allocator, its _sbrk_r() and its malloc lock are not linked.
 *
 * On top of that, the _lock_*() functions of ESP-IDF's
 * esp_libc/platform_include/sys/lock.h, which Espressif sources (esp_phy)
 * use, on the same locks.
 *
 * As in the original, the C library may be used from threads and from
 * tx_application_define(), never from interrupt handlers.
 */
#include <reent.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/lock.h>

#include "tx_api.h"
#include "tx_thread.h"

#ifndef TX_THREAD_SAFE_CLIB
#error "tx_clib_newlib.c expects TX_THREAD_SAFE_CLIB (set by the Xtensa tx_port.h)"
#endif

/* ---- Locks --------------------------------------------------------------- */

struct __lock {
    TX_MUTEX mutex;
};

/* Static locks referenced by name from inside newlib. */
struct __lock __lock___sfp_recursive_mutex;
struct __lock __lock___atexit_recursive_mutex;
struct __lock __lock___at_quick_exit_mutex;
struct __lock __lock___env_recursive_mutex;
struct __lock __lock___tz_mutex;
struct __lock __lock___dd_hash_mutex;
struct __lock __lock___arc4random_mutex;

static struct __lock * const static_locks[] = {
    &__lock___sfp_recursive_mutex,
    &__lock___atexit_recursive_mutex,
    &__lock___at_quick_exit_mutex,
    &__lock___env_recursive_mutex,
    &__lock___tz_mutex,
    &__lock___dd_hash_mutex,
    &__lock___arc4random_mutex,
};

/*
 * Locks only mean something once threads run.  During initialization there
 * is a single flow of control and a mutex cannot be taken, so lock calls made
 * then are skipped -- acquire and release alike, which keeps them balanced.
 */
static int threads_running(void)
{
    return TX_THREAD_GET_SYSTEM_STATE() == 0u && _tx_thread_current_ptr != TX_NULL;
}

static void lock_create(struct __lock *lock)
{
    tx_mutex_create(&lock->mutex, "clib", TX_INHERIT);
}

static void lock_new(_LOCK_T *lock)
{
    struct __lock *l = malloc(sizeof(*l));

    if (l != NULL) {
        lock_create(l);
    }
    /* On allocation failure the stream simply runs unlocked. */
    *lock = l;
}

static void lock_free(_LOCK_T lock)
{
    if (lock != NULL) {
        tx_mutex_delete(&lock->mutex);
        free(lock);
    }
}

static void lock_get(_LOCK_T lock)
{
    if (lock != NULL && threads_running()) {
        tx_mutex_get(&lock->mutex, TX_WAIT_FOREVER);
    }
}

static int lock_try(_LOCK_T lock)
{
    if (lock != NULL && threads_running()) {
        return tx_mutex_get(&lock->mutex, TX_NO_WAIT) == TX_SUCCESS ? 0 : -1;
    }
    return 0;
}

static void lock_put(_LOCK_T lock)
{
    if (lock != NULL && threads_running()) {
        tx_mutex_put(&lock->mutex);
    }
}

/* TX_MUTEX is recursive, so one implementation serves both lock flavours. */
void __retarget_lock_init(_LOCK_T *lock)                    { lock_new(lock); }
void __retarget_lock_init_recursive(_LOCK_T *lock)          { lock_new(lock); }
void __retarget_lock_close(_LOCK_T lock)                    { lock_free(lock); }
void __retarget_lock_close_recursive(_LOCK_T lock)          { lock_free(lock); }
void __retarget_lock_acquire(_LOCK_T lock)                  { lock_get(lock); }
void __retarget_lock_acquire_recursive(_LOCK_T lock)        { lock_get(lock); }
int  __retarget_lock_try_acquire(_LOCK_T lock)              { return lock_try(lock); }
int  __retarget_lock_try_acquire_recursive(_LOCK_T lock)    { return lock_try(lock); }
void __retarget_lock_release(_LOCK_T lock)                  { lock_put(lock); }
void __retarget_lock_release_recursive(_LOCK_T lock)        { lock_put(lock); }

/*
 * ESP-IDF's _lock_*() (the declarations are in esp_libc's
 * platform_include/sys/lock.h, with _lock_t being _LOCK_T): a lock that is
 * still NULL, as a static _lock_t starts, is created on first use.  Two
 * threads may get there at once; the loser frees its lock again.  A lock
 * that could not be allocated stays NULL and does not lock.
 */
static void lock_create_once(_LOCK_T *plock)
{
    TX_INTERRUPT_SAVE_AREA
    _LOCK_T lock;

    if (*plock != NULL) {
        return;
    }
    lock_new(&lock);
    if (lock == NULL) {
        return;
    }
    TX_DISABLE
    if (*plock == NULL) {
        *plock = lock;
        lock = NULL;
    }
    TX_RESTORE
    lock_free(lock);
}

void _lock_init(_LOCK_T *plock)                             { *plock = NULL; lock_create_once(plock); }
void _lock_init_recursive(_LOCK_T *plock)                   { *plock = NULL; lock_create_once(plock); }
void _lock_close(_LOCK_T *plock)                            { lock_free(*plock); *plock = NULL; }
void _lock_close_recursive(_LOCK_T *plock)                  { lock_free(*plock); *plock = NULL; }
void _lock_acquire(_LOCK_T *plock)                          { lock_create_once(plock); lock_get(*plock); }
void _lock_acquire_recursive(_LOCK_T *plock)                { lock_create_once(plock); lock_get(*plock); }
int  _lock_try_acquire(_LOCK_T *plock)                      { lock_create_once(plock); return lock_try(*plock); }
int  _lock_try_acquire_recursive(_LOCK_T *plock)            { lock_create_once(plock); return lock_try(*plock); }
void _lock_release(_LOCK_T *plock)                          { lock_put(*plock); }
void _lock_release_recursive(_LOCK_T *plock)                { lock_put(*plock); }

/* ---- Per-thread reentrancy state ----------------------------------------- */

/* The reent structure used before threads run and from idle. */
static struct _reent *global_reent;

/*
 * newlib asks for the current reent through this function (__DYNAMIC_REENT__).
 * A thread gets its own once its wrapper has initialized it.
 */
struct _reent *__getreent(void)
{
    TX_THREAD *thread_ptr = _tx_thread_current_ptr;

    if (thread_ptr != TX_NULL && thread_ptr->tx_thread_clib_ptr != NULL) {
        return thread_ptr->tx_thread_clib_ptr;
    }
    return global_reent != NULL ? global_reent : _impure_ptr;
}

void _tx_clib_reent_init(TX_THREAD *thread_ptr)
{
    struct _reent *reent;

    if (thread_ptr == TX_NULL) {
        return;
    }

    reent = &thread_ptr->tx_thread_clib_reent;
    memset(reent, 0, sizeof(*reent));
    _REENT_INIT_PTR(reent);
    thread_ptr->tx_thread_clib_ptr = reent;
}

/*
 * partial != 0: the thread has finished but may be restarted.
 * partial == 0: release everything the C library allocated for the thread.
 * stdin/stdout/stderr are global streams here, so there is nothing per-thread
 * to flush or close in the partial case.
 */
void _tx_clib_reent_cleanup(TX_THREAD *thread_ptr, int partial)
{
    if (partial == 0) {
        _reclaim_reent(&thread_ptr->tx_thread_clib_reent);
    }
}

/* Every thread is entered through this wrapper (see _tx_clib_thread_setup). */
static void thread_wrapper(ULONG arg)
{
    TX_INTERRUPT_SAVE_AREA
    TX_THREAD *thread_ptr = _tx_thread_current_ptr;

    (void) arg;

    _tx_clib_reent_init(thread_ptr);

    TX_DISABLE
    _impure_ptr = thread_ptr->tx_thread_clib_ptr;
    TX_RESTORE

    (thread_ptr->tx_real_thread_entry)(thread_ptr->tx_thread_entry_parameter);

    _tx_clib_reent_cleanup(thread_ptr, 1);
    _tx_clib_reent_cleanup(thread_ptr, 0);

    /*
     * The reent is gone; fall back to the global one until the thread is
     * reset and runs the wrapper again.  The scheduler copies this pointer to
     * _impure_ptr on the next switch to this thread.
     */
    TX_DISABLE
    thread_ptr->tx_thread_clib_ptr = global_reent;
    _impure_ptr = global_reent;
    TX_RESTORE
}

void _tx_clib_thread_setup(TX_THREAD *thread_ptr)
{
    thread_ptr->tx_real_thread_entry = thread_ptr->tx_thread_entry;
    thread_ptr->tx_thread_entry      = thread_wrapper;
    /* Valid from the first dispatch, before the wrapper has run. */
    thread_ptr->tx_thread_clib_ptr   = global_reent != NULL ? global_reent : _impure_ptr;
}

/* Called by _tx_initialize_low_level(). */
void _tx_clib_init(void)
{
    unsigned int i;

    global_reent = _impure_ptr;

    for (i = 0; i < sizeof(static_locks) / sizeof(static_locks[0]); i++) {
        lock_create(static_locks[i]);
    }
}
