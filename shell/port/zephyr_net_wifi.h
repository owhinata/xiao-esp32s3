/* SPDX-License-Identifier: MIT */
/*
 * Start-up of the network layer of the shell (zephyr_net_wifi.c): what
 * rp2040-connect's zephyr_net_initialize() does on its FreeRTOS builds
 * (common/compat/net_offload/zephyr_net_init.c), then the "wifi" command's
 * initialization (wifi_shell_init()).
 */
#ifndef SHELL_PORT_ZEPHYR_NET_WIFI_H
#define SHELL_PORT_ZEPHYR_NET_WIFI_H

/*
 * The network event thread ("net_mgmt"), the Wi-Fi station interface
 * with its operations (given to port/net/wifi.c, net_wifi_set_ops()) and
 * the thread that raises its events ("wifi_evt"), the "wifi" command's
 * event callbacks.  Once, from
 * tx_application_define() or a thread, before the shell takes commands;
 * the operations answer -ENETDOWN until the driver runs
 * (net_wifi_start()).  0, or a negative errno value.
 */
int zephyr_net_initialize(void);

/*
 * The stored networks ("wifi cred"): the initialization of the copy of
 * Zephyr's Wi-Fi credentials (wifi_credentials_init()), which reads their
 * SSIDs from the store, at the point of rp2040-connect's start where
 * Zephyr runs its SYS_INIT().  Once, from a thread, after kv_store_init().
 * A failure (the store closed) is logged; the commands then fail.
 */
void zephyr_net_wifi_credentials_init(void);

/*
 * The automatic connection: starts the Wi-Fi profile's thread
 * (wifi_reconnect_tid, as rp2040-connect's app_threads_start()), then
 * wifi_profile_init(), which registers its hooks with the driver and, with
 * a network stored, has the thread connect to it 4 s later.  Once, from a
 * thread, after zephyr_net_wifi_credentials_init() and net_wifi_start().
 * 0, or a negative errno value (logged).
 */
int zephyr_net_wifi_profile_start(void);

#endif /* SHELL_PORT_ZEPHYR_NET_WIFI_H */
