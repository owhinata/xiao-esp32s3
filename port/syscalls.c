/*
 * newlib system call layer: file descriptors 0-2 are the board console.
 */
#include <errno.h>
#include <reent.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "tx_api.h"
#include "tx_initialize.h"
#include "tx_thread.h"

#include "board.h"

static void (*stdout_sink)(const char *buf, size_t len);

void console_stdout_redirect(void (*sink)(const char *buf, size_t len))
{
    stdout_sink = sink;
}

/* An interrupt handler is running (nesting count; larger values: start-up). */
static int in_interrupt(void)
{
    ULONG state = TX_THREAD_GET_SYSTEM_STATE();

    return state != 0u && state < TX_INITIALIZE_IN_PROGRESS;
}

_ssize_t _write_r(struct _reent *r, int fd, const void *buf, size_t len)
{
    const char *p = buf;
    size_t i;
    size_t start = 0;

    if (fd != STDOUT_FILENO && fd != STDERR_FILENO) {
        r->_errno = EBADF;
        return -1;
    }

    /*
     * The sink serializes writers with a mutex, so it cannot serve an
     * interrupt handler; what is printed there goes out directly.
     */
    if (stdout_sink != NULL && !in_interrupt()) {
        stdout_sink(p, len);
        return (_ssize_t) len;
    }

    /* Terminals expect CR LF. */
    for (i = 0; i < len; i++) {
        if (p[i] == '\n') {
            console_write(p + start, (uint32_t) (i - start));
            console_write("\r\n", 2);
            start = i + 1;
        }
    }
    console_write(p + start, (uint32_t) (len - start));
    return (_ssize_t) len;
}

/* Blocks until at least one byte arrives.  Polled: one tick between tries. */
_ssize_t _read_r(struct _reent *r, int fd, void *buf, size_t len)
{
    int n;

    if (fd != STDIN_FILENO) {
        r->_errno = EBADF;
        return -1;
    }
    if (len == 0) {
        return 0;
    }

    for (;;) {
        n = console_read_nonblock(buf, (uint32_t) len);
        if (n > 0) {
            return n;
        }
        if (TX_THREAD_GET_SYSTEM_STATE() != 0u || _tx_thread_current_ptr == TX_NULL) {
            /* Not in a thread: cannot sleep, so report "nothing yet". */
            r->_errno = EAGAIN;
            return -1;
        }
        tx_thread_sleep(1);
    }
}

int _close_r(struct _reent *r, int fd)
{
    (void) fd;
    r->_errno = EBADF;
    return -1;
}

_off_t _lseek_r(struct _reent *r, int fd, _off_t off, int whence)
{
    (void) fd; (void) off; (void) whence;
    r->_errno = ESPIPE;
    return -1;
}

int _fstat_r(struct _reent *r, int fd, struct stat *st)
{
    if (fd < 0 || fd > STDERR_FILENO) {
        r->_errno = EBADF;
        return -1;
    }
    st->st_mode = S_IFCHR;
    return 0;
}

int _isatty_r(struct _reent *r, int fd)
{
    (void) r;
    return fd >= 0 && fd <= STDERR_FILENO;
}

/*
 * POSIX sleeps, which newlib leaves to the system (the Wi-Fi supplicant's
 * os_sleep() calls them): the calling thread sleeps for at least the time
 * asked, in whole ticks, rounded up.  Only from a thread.
 */
int usleep(useconds_t us)
{
    const uint64_t us_per_tick = 1000000u / TX_TIMER_TICKS_PER_SECOND;
    uint64_t ticks = ((uint64_t) us + us_per_tick - 1u) / us_per_tick;

    while (ticks > 0u) {
        ULONG n = ticks > 0x7FFFFFFFu ? 0x7FFFFFFFu : (ULONG) ticks;

        tx_thread_sleep(n);
        ticks -= n;
    }
    return 0;
}

unsigned int sleep(unsigned int seconds)
{
    while (seconds > 0u) {
        unsigned int n = seconds > 1000u ? 1000u : seconds;

        (void) usleep((useconds_t) n * 1000000u);
        seconds -= n;
    }
    return 0;
}

int _getpid_r(struct _reent *r)
{
    (void) r;
    return 1;
}

int _kill_r(struct _reent *r, int pid, int sig)
{
    (void) pid; (void) sig;
    r->_errno = EINVAL;
    return -1;
}
