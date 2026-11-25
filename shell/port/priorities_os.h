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

/*
 * The thread that raises the Wi-Fi events of the network layer, "wifi_evt"
 * (zephyr_net_wifi.c), at the priority of rp2040-connect's NINA Wi-Fi
 * thread (APP_PRIO_NINA_WIFI): it takes a scan's records from the driver
 * (its OS adapter's calls) and logs with newlib's printf.
 */
#define APP_STACK_WIFI_EVENTS   4096

/*
 * The Wi-Fi profile's reconnection thread "wifi_reconnect_tid"
 * (shell/common/imu/wifi_profile.c, the shared default 2048 bytes, measured
 * on a Cortex-M0+): a stored network's connection request runs in it, from
 * the credentials' settings and NVS down to the flash driver
 * (net_mgmt(NET_REQUEST_WIFI_CONNECT_STORED), about 1.3 KiB of frames here,
 * from GCC's call graph), and a log line on that path adds newlib's
 * vsnprintf() (about 1.1 KiB), which 2048 bytes do not hold.  "ps" shows
 * what is left.
 */
#define APP_STACK_WIFI_RECONNECT 4096

/*
 * The Telnet shell's service thread "telnet_service" (shell/common/compat/
 * shell_engine/shell_telnet.c, the shared default 2048 bytes): its poll
 * goes through the socket layer's zvfs_poll() (an event array of 384
 * bytes), the binding's two descriptor arrays and lwip_poll(), and its log
 * lines are formatted with newlib's vsnprintf().  Its session
 * "shell_telnet" runs the commands with the shell's stack, APP_STACK_SHELL.
 */
#define APP_STACK_TELNET_SERVICE 4096

#endif /* PRIORITIES_OS_H */
