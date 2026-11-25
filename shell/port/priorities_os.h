/* SPDX-License-Identifier: MIT */
/*
 * The board's part of the layer's thread priorities and stack sizes
 * (shell/common/compat/include/priorities.h, which includes this header
 * first).  The mapping and the time slices are those of the other ThreadX
 * build of the shared sources:
 *
 *   layer priority   = 15 - Zephyr priority     (larger = more urgent)
 *   ThreadX priority = 31 - layer priority = Zephyr priority + 16
 *
 * so the shell's thread (Zephyr's lowest application priority, 14) runs at
 * ThreadX priority 30, which is what "ps" shows.
 */
#ifndef PRIORITIES_OS_H
#define PRIORITIES_OS_H

#define APP_PRIO_FROM_ZEPHYR(p) (15 - (p))

/*
 * 20 ms time slices among threads of the same priority, for Zephyr
 * priorities 0 and below in urgency (z_os_thread_start() hands them to
 * ThreadX).
 */
#define APP_TIMESLICE_TICKS     20
#define APP_TIMESLICE_PRIO_MAX  APP_PRIO_FROM_ZEPHYR(0)

/*
 * The shell's thread.  The shared default (3072 bytes, the Zephyr build's
 * CONFIG_SHELL_STACK_SIZE) serves builds on a Cortex-M0+, two of them with
 * the Pico SDK's small printf.  Here every command prints through newlib's
 * printf, which on an unbuffered stream formats into a BUFSIZ (1024-byte)
 * buffer on the stack, and the windowed ABI adds a register save area to
 * every call frame.  "ps" shows what is left.
 */
#define APP_STACK_SHELL         8192

/*
 * The network event thread "net_mgmt" (shell/common/compat/
 * zephyr_net_mgmt.c, the shared default 2048 bytes, Zephyr's
 * CONFIG_NET_MGMT_EVENT_STACK_SIZE): its callbacks print the "wifi"
 * command's results with newlib's printf, as the shell's thread does.
 */
#define APP_STACK_NET_MGMT      6144

#endif /* PRIORITIES_OS_H */
