/*
 * newlib system call layer: file descriptors 0-2 are the board console.
 */
#include <errno.h>
#include <reent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "tx_api.h"
#include "tx_thread.h"

#include "board.h"

_ssize_t _write_r(struct _reent *r, int fd, const void *buf, size_t len)
{
    const char *p = buf;
    size_t i;
    size_t start = 0;

    if (fd != STDOUT_FILENO && fd != STDERR_FILENO) {
        r->_errno = EBADF;
        return -1;
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
