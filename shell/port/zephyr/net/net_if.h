/* SPDX-License-Identifier: MIT */
/*
 * This board's <zephyr/net/net_if.h>: the network interface as far as the
 * Wi-Fi management layer and the "wifi" command use it (shell/common/
 * compat/zephyr_wifi_mgmt.c, zephyr_wifi_shell.c): the one interface, the
 * Wi-Fi station, with the operations of its driver, and whether it is up.
 * The names and meanings are rp2040-connect's (its common/compat/
 * net_offload/include/zephyr/net/net_if.h, which also carries the IPv4
 * address and the link address of an offloaded network stack); here the
 * network stack is not in the build yet, so there is no address.
 * Implemented in zephyr_net_if.c.
 */
#ifndef SHELL_PORT_ZEPHYR_NET_NET_IF_H
#define SHELL_PORT_ZEPHYR_NET_NET_IF_H

#include <stdbool.h>

struct net_if {
	const void *api; /* struct net_wifi_mgmt_offload */
};

/* Makes iface the interface (the only one), administratively up. */
void z_net_if_register(struct net_if *iface);

/* The interface, NULL before z_net_if_register(). */
struct net_if *net_if_get_default(void);
struct net_if *net_if_get_wifi_sta(void);

bool net_if_is_admin_up(struct net_if *iface);

#endif /* SHELL_PORT_ZEPHYR_NET_NET_IF_H */
