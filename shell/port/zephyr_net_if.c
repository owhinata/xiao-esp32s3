/* SPDX-License-Identifier: MIT */
/*
 * The network interface of <zephyr/net/net_if.h> (shell/port/zephyr/net/
 * net_if.h): the one interface, registered at the start
 * (zephyr_net_initialize(), zephyr_net_wifi.c), up from then on.
 */
#include <stdbool.h>
#include <stddef.h>

#include <zephyr/net/net_if.h>

static struct net_if *default_iface;

void z_net_if_register(struct net_if *iface)
{
	default_iface = iface;
}

struct net_if *net_if_get_default(void)
{
	return default_iface;
}

struct net_if *net_if_get_wifi_sta(void)
{
	return default_iface;
}

bool net_if_is_admin_up(struct net_if *iface)
{
	return iface != NULL && iface == default_iface;
}
