/*
 * Thread-specific data on ThreadX threads (pthread_local.c).
 */
#ifndef ESP_COMPAT_PTHREAD_LOCAL_H
#define ESP_COMPAT_PTHREAD_LOCAL_H

/*
 * Runs the destructors of the calling thread's values, as at a POSIX
 * thread's exit; from the thread, before it ends.
 */
void esp_compat_pthread_exit_destructors(void);

#endif
