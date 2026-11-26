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

/*
 * The iperf3 server of "zperf tcp download", "zperf_server_tid"
 * (shell/common/compat/zperf_iperf3.c, the shared default 2048 bytes): the
 * same poll path as "telnet_service", and after it the server's JSON
 * (snprintf()) and the command's report of each test, which it prints to
 * the session that started the server with newlib's printf, as "net_mgmt"
 * prints the "wifi" command's results.  "ps" shows what is left.
 */
#define APP_STACK_ZPERF_SERVER  6144

/*
 * The time synchronization's thread "time_sync_tid" (shell/common/imu/
 * time_sync.c, the shared default 2048 bytes): a UDP send and receive
 * through the socket layer to lwIP (each lwIP call waits for "tcpip") and
 * the 64-bit arithmetic of the estimate; it prints nothing.  "ps" shows
 * what is left.
 */
#define APP_STACK_TIME_SYNC     4096

/*
 * The USB host's FreeRTOS tasks (port/usb/host.c): "usb_host", which runs
 * the USB Host Library's events (the hub driver, the enumeration), and
 * "usb_client", which runs its client's.  The FreeRTOS layer's priority p
 * is ThreadX priority 31 - p, the same as this layer's priority, so
 * APP_PRIO_FROM_ZEPHYR() gives the FreeRTOS priority as well: Zephyr's 8,
 * ThreadX 24, with the threads that take the Wi-Fi driver's events, below
 * "net_mgmt".  4096 bytes each, as in ESP-IDF's USB host examples; both
 * log with newlib's vsnprintf().  "ps" shows what is left.
 */
#define APP_PRIO_USB_HOST       APP_PRIO_FROM_ZEPHYR(8)
#define APP_STACK_USB_HOST      4096
#define APP_STACK_USB_CLIENT    4096

/*
 * The USB audio input's FreeRTOS tasks (port/usb/audio.c), above lwIP's
 * "tcpip" (ThreadX 13, which can run for many milliseconds on end while
 * TCP data flows, as under "zperf") and the network event thread
 * "net_mgmt" (15), and below the Wi-Fi driver's task (8) and esp_timer's
 * (9), whose bursts are short and which have deadlines of their own.
 *
 * "USB UAC Host", the UAC driver's event task, ThreadX 10 (FreeRTOS 21,
 * Zephyr -6), above "sys_evt" (11) too: an isochronous IN transfer that
 * completes is copied to the driver's buffer and resubmitted in it, which
 * has to happen before the two other transfers in flight are done (6 ms;
 * the packets of a missed frame are lost, isochronous transfers have no
 * retry).  Its work is a copy of about 300 bytes and a resubmission every
 * 3 ms, so it takes little from the threads below it.  The USB Host
 * Library's own tasks (APP_PRIO_USB_HOST) are not on that path: a
 * transfer's completion goes from the controller's interrupt to the
 * client's task.  They serve the enumeration and the control transfers,
 * which may wait.
 *
 * "uac", which reads the samples from the driver's buffer, decimates them
 * and serves the requests, ThreadX 12 (FreeRTOS 19, Zephyr -4), below
 * "sys_evt" and above "tcpip": the driver's buffer holds 40 to 80 ms
 * (port/usb/audio.c), and an RX_DONE wakes it every 10 to 20 ms.
 *
 * 4096 bytes each, as in esp-usb's UAC examples; both log with newlib's
 * vsnprintf(), "uac" also through the driver's calls (ESP_LOGx).  "ps"
 * shows what is left.
 */
#define APP_PRIO_UAC_DRIVER     APP_PRIO_FROM_ZEPHYR(-6)
#define APP_STACK_UAC_DRIVER    4096
#define APP_PRIO_UAC_READER     APP_PRIO_FROM_ZEPHYR(-4)
#define APP_STACK_UAC_READER    4096

#endif /* PRIORITIES_OS_H */
