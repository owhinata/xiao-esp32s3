/* SPDX-License-Identifier: MIT */
/*
 * The console transport of the shell engine on this board
 * (shell/common/compat/port/shell_serial_os.h reads this file and says what
 * each item must do): the console of port/board.c (USB Serial/JTAG and
 * UART0 together) and newlib's stdout and stderr of port/syscalls.c.
 */
#ifndef SHELL_PORT_SHELL_SERIAL_OS_IMPL_H
#define SHELL_PORT_SHELL_SERIAL_OS_IMPL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "board.h"
#include "shell_console.h"

/* What a thread writes to stdout or stderr goes to its console. */
static void shell_serial_os_stdout(const char *buf, size_t len)
{
    shell_console_write(shell_console_current(), buf, len);
}

/*
 * stdout and stderr are one pair of streams for all threads here, and both
 * end in _write_r().  Unbuffered, every call reaches the console before it
 * returns, so printf(), fwrite() and putchar() stay in order with each other
 * and with shell_print(), which does not go through the stream; fflush() has
 * nothing left to do.  stderr is unbuffered from the start.
 */
static inline void shell_serial_os_stdio_init(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    console_stdout_redirect(shell_serial_os_stdout);
}

static inline void shell_serial_os_rx_init(void)
{
    console_rx_notify(shell_serial_rx_ready);
}

/*
 * USB host present, or a byte received on the UART since the start
 * (console_host_present() says why that is all a UART can tell).
 */
static inline bool shell_serial_os_connected(void)
{
    return console_host_present() != 0;
}

static inline int shell_serial_os_read(char *buf, size_t len)
{
    return console_read_nonblock(buf, (uint32_t) len);
}

/*
 * console_write() gives up on USB after 50 ms when the host stops reading;
 * for the UART it queues the bytes for its transmit interrupt, which sends
 * them at the baud rate, listener or not, and waits only while that queue
 * (4 KiB) is full.
 */
static inline void shell_serial_os_write(const char *buf, size_t len)
{
    console_write(buf, (uint32_t) len);
}

#endif /* SHELL_PORT_SHELL_SERIAL_OS_IMPL_H */
