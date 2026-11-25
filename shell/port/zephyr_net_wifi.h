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

#endif /* SHELL_PORT_ZEPHYR_NET_WIFI_H */
