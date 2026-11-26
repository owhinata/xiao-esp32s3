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
 * every call frame.  The deepest so far, over the commands on both
 * consoles and a "zperf tcp upload", is 2.9 KiB.  "ps" shows what is left.
 */
#define APP_STACK_SHELL         5120

/*
 * The network event thread "net_mgmt" (shell/common/compat/
 * zephyr_net_mgmt.c, the shared default 2048 bytes, Zephyr's
 * CONFIG_NET_MGMT_EVENT_STACK_SIZE): its callbacks print the "wifi"
 * command's results with newlib's printf, as the shell's thread does
 * (2.0 KiB at its deepest so far, a scan's results).
 */
#define APP_STACK_NET_MGMT      4096

/*
 * The thread that raises the Wi-Fi events of the network layer, "wifi_evt"
 * (zephyr_net_wifi.c), at the priority of rp2040-connect's NINA Wi-Fi
 * thread (APP_PRIO_NINA_WIFI): it takes a scan's records from the driver
 * (its OS adapter's calls) and logs with newlib's printf (1.0 KiB at its
 * deepest so far).
 */
#define APP_STACK_WIFI_EVENTS   2560

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
#define APP_STACK_WIFI_RECONNECT 3072

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
 * prints the "wifi" command's results (2.1 KiB at its deepest so far, a
 * test and one the client gave up).  "ps" shows what is left.
 */
#define APP_STACK_ZPERF_SERVER  4096

/*
 * The OTA service's thread "ota_service" (shell/port/ota_service.c), at
 * the priority of "telnet_service" and the zperf server (Zephyr's 14,
 * ThreadX 30).  Its deepest path is the end of an update in
 * port/ota_slots.c: esp_ota_end() and esp_ota_set_boot_partition() each
 * check the image with esp_image_verify(), about 1.3 KiB of frames down
 * to its SHA-256 or its flash reads (GCC's call graph; PSA's hash
 * operation is on the heap), and an ESP_LOGx of the check goes on through
 * the RAM log's vprintf (0.75 KiB) into newlib's vsnprintf() (about
 * 1.1 KiB): about 3.2 KiB with an interrupt's frame.  "ps" shows what is
 * left.
 */
#define APP_PRIO_OTA_SERVICE    APP_PRIO_FROM_ZEPHYR(14)
#define APP_STACK_OTA_SERVICE   4096

/*
 * The time synchronization's thread "time_sync_tid" (shell/common/imu/
 * time_sync.c, the shared default 2048 bytes): a UDP send and receive
 * through the socket layer to lwIP (each lwIP call waits for "tcpip") and
 * the 64-bit arithmetic of the estimate; it prints nothing (1.4 KiB at its
 * deepest so far).  "ps" shows what is left.
 */
#define APP_STACK_TIME_SYNC     3072

/*
 * The USB host's FreeRTOS tasks (port/usb/host.c): "usb_host", which runs
 * the USB Host Library's events (the hub driver, the enumeration), and
 * "usb_client", which runs its client's.  The FreeRTOS layer's priority p
 * is ThreadX priority 31 - p, the same as this layer's priority, so
 * APP_PRIO_FROM_ZEPHYR() gives the FreeRTOS priority as well: Zephyr's 8,
 * ThreadX 24, with the threads that take the Wi-Fi driver's events, below
 * "net_mgmt".  ESP-IDF's USB host examples give each 4096 bytes;
 * "usb_client" has used 1.9 KiB of them (a device's descriptors for "usb
 * desc") and keeps them, "usb_host" 0.9 KiB.  Both log with newlib's
 * vsnprintf().  "ps" shows what is left.
 */
#define APP_PRIO_USB_HOST       APP_PRIO_FROM_ZEPHYR(8)
#define APP_STACK_USB_HOST      2560
#define APP_STACK_USB_CLIENT    4096

/*
 * The USB audio input's FreeRTOS tasks (port/usb/audio.c,
 * port/usb/audio_uac2.c), above lwIP's "tcpip" (ThreadX 13, which can run
 * for many milliseconds on end while TCP data flows, as under "zperf") and
 * the network event thread "net_mgmt" (15), and below the Wi-Fi driver's
 * task (8) and esp_timer's (9), whose bursts are short and which have
 * deadlines of their own.
 *
 * "USB UAC Host", the UAC 1.0 driver's event task, and "uac2", the UAC 2.0
 * client's, ThreadX 10 (FreeRTOS 21, Zephyr -6), above "sys_evt" (11) too:
 * an isochronous IN transfer that completes is copied to a ring buffer and
 * resubmitted in it, which has to happen before the two other transfers in
 * flight are done (6 ms; the packets of a missed frame are lost,
 * isochronous transfers have no retry).  The work is a copy of about 300
 * bytes and a resubmission every 3 ms, so it takes little from the threads
 * below.  Only one microphone streams at a time, so the two never compete.
 * The USB Host Library's own tasks (APP_PRIO_USB_HOST) are not on that
 * path: a transfer's completion goes from the controller's interrupt to the
 * client's task.  They serve the enumeration and the control transfers,
 * which may wait.
 *
 * "uac", which reads the samples from the buffer, decimates them and serves
 * the requests, ThreadX 12 (FreeRTOS 19, Zephyr -4), below "sys_evt" and
 * above "tcpip": the buffer holds 40 to 80 ms (port/usb/audio.c), and it is
 * woken every 10 to 20 ms.
 *
 * 4096 bytes for the client tasks, as in esp-usb's UAC examples ("USB UAC
 * Host" had 1448 left after a device went during a stream, its deepest
 * path: the driver's stop and close in its callback, with ESP_LOGx).
 * 5120 for "uac": 1392 were left of 4096 with UAC 1.0 alone, and it now
 * also parses a UAC 2.0 device's descriptors and sends its class requests.
 * All log with newlib's vsnprintf().  "ps" shows what is left.
 */
#define APP_PRIO_UAC_DRIVER     APP_PRIO_FROM_ZEPHYR(-6)
#define APP_STACK_UAC_DRIVER    4096
#define APP_STACK_UAC2_CLIENT   4096
#define APP_PRIO_UAC_READER     APP_PRIO_FROM_ZEPHYR(-4)
#define APP_STACK_UAC_READER    5120

#endif /* PRIORITIES_OS_H */
