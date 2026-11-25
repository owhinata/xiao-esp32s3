/*
 * newlib's character and string output on stdout and stderr, without the
 * stream's lock: fwrite(), fputs(), puts(), fputc(), putc() and putchar(),
 * linked with -Wl,--wrap=<name> (CMakeLists.txt).  On stdout and stderr
 * they write straight to _write_r() (port/syscalls.c), which is what newlib
 * does for these unbuffered streams too, less the lock; on any other stream
 * they are newlib's.
 *
 * stdout and stderr are one pair of streams for every thread (newlib's
 * __sf[]), and newlib holds a stream's lock for the whole write of these
 * functions.  A write ends in the calling thread's console
 * (shell/port/shell_serial_os_impl.h), and the Telnet console waits up to
 * 10 s for a client that does not read (shell_telnet.c, SEND_WAIT_MS): with
 * the lock held for that long, every other thread's printf() and putchar(),
 * the serial console's among them, would wait too.  The consoles serialize
 * their writers themselves (shell_console.c), so the lock is not needed.
 * printf() and the rest of its family are left as they are: newlib takes
 * the lock only to look at the stream and then formats into a stream of
 * the call's own (__sbprintf() for an unbuffered stream), whose output goes
 * to _write_r() without the shared lock.
 */
#include <reent.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

size_t __real_fwrite(const void *buf, size_t size, size_t n, FILE *fp);
int __real_fputs(const char *s, FILE *fp);
int __real_fputc(int c, FILE *fp);
int __real_putc(int c, FILE *fp);

size_t __wrap_fwrite(const void *buf, size_t size, size_t n, FILE *fp);
int __wrap_fputs(const char *s, FILE *fp);
int __wrap_puts(const char *s);
int __wrap_fputc(int c, FILE *fp);
int __wrap_putc(int c, FILE *fp);
int __wrap_putchar(int c);

/* STDOUT_FILENO or STDERR_FILENO for the two streams, else -1. */
static int console_fd(FILE *fp)
{
    if (fp == stdout) {
        return STDOUT_FILENO;
    }
    if (fp == stderr) {
        return STDERR_FILENO;
    }
    return -1;
}

static void put(int fd, const void *buf, size_t len)
{
    if (len > 0u) {
        (void) _write_r(_REENT, fd, buf, len);
    }
}

static int put_char(int fd, int c)
{
    unsigned char ch = (unsigned char) c;

    put(fd, &ch, 1u);
    return ch;
}

size_t __wrap_fwrite(const void *buf, size_t size, size_t n, FILE *fp)
{
    int fd = console_fd(fp);

    if (fd < 0) {
        return __real_fwrite(buf, size, n, fp);
    }
    if (size == 0u) {
        return 0u;
    }
    put(fd, buf, size * n);
    return n;
}

int __wrap_fputs(const char *s, FILE *fp)
{
    int fd = console_fd(fp);

    if (fd < 0) {
        return __real_fputs(s, fp);
    }
    put(fd, s, strlen(s));
    return 0;
}

int __wrap_puts(const char *s)
{
    put(STDOUT_FILENO, s, strlen(s));
    put(STDOUT_FILENO, "\n", 1u);
    return 0;
}

int __wrap_fputc(int c, FILE *fp)
{
    int fd = console_fd(fp);

    return fd < 0 ? __real_fputc(c, fp) : put_char(fd, c);
}

int __wrap_putc(int c, FILE *fp)
{
    int fd = console_fd(fp);

    return fd < 0 ? __real_putc(c, fp) : put_char(fd, c);
}

int __wrap_putchar(int c)
{
    return put_char(STDOUT_FILENO, c);
}
