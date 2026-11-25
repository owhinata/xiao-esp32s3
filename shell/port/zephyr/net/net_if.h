/* SPDX-License-Identifier: MIT */
/*
 * This board's <zephyr/net/net_if.h>: the network interface as the Wi-Fi
 * management layer, the "wifi" command (shell/common/compat/
 * zephyr_wifi_mgmt.c, zephyr_wifi_shell.c) and the board's Wi-Fi interface
 * (zephyr_net_wifi.c) use it: the one interface, the Wi-Fi station, with
 * the operations of its driver, whether it is up and on a network
 * (dormant), its IPv4 address, netmask and default gateway, its link
 * address and MTU.  The names and meanings are rp2040-connect's (its
 * common/compat/net_offload/include/zephyr/net/net_if.h, written for an
 * offloaded network stack, whose functions these are); its address type
 * comes from the copied socket headers there, and here it is defined
 * below with the same layout.  Implemented in zephyr_net_if.c.  Any
 * thread may call the functions (not interrupt handlers).
 */
#ifndef SHELL_PORT_ZEPHYR_NET_NET_IF_H
#define SHELL_PORT_ZEPHYR_NET_NET_IF_H

#include <stdbool.h>
#include <stdint.h>

/* An IPv4 address, s_addr in network byte order (<netinet/in.h>'s struct in_addr). */
struct net_in_addr {
	uint32_t s_addr;
};

struct net_if {
	const void *api; /* struct net_wifi_mgmt_offload */
};

/* How an address was obtained, and its state (Zephyr's). */
enum net_addr_type {
	NET_ADDR_ANY = 0,
	NET_ADDR_AUTOCONF,
	NET_ADDR_DHCP,
	NET_ADDR_MANUAL,
	NET_ADDR_OVERRIDABLE,
};

enum net_addr_state {
	NET_ADDR_ANY_STATE = -1,
	NET_ADDR_TENTATIVE = 0,
	NET_ADDR_PREFERRED,
	NET_ADDR_DEPRECATED,
};

enum net_link_type {
	NET_LINK_UNKNOWN = 0,
	NET_LINK_ETHERNET,
};

struct net_if_addr {
	struct net_in_addr address;
	enum net_addr_type addr_type;
	enum net_addr_state addr_state;
};

#define NET_LINK_ADDR_MAX_LENGTH 6

struct net_linkaddr {
	uint8_t type; /* enum net_link_type */
	uint8_t len;
	uint8_t addr[NET_LINK_ADDR_MAX_LENGTH];
};

/* Makes iface the interface (the only one), administratively up, dormant, without an address. */
void z_net_if_register(struct net_if *iface);

/* The interface, NULL before z_net_if_register(). */
struct net_if *net_if_get_default(void);
struct net_if *net_if_get_wifi_sta(void);

/* The one IPv4 address: adding replaces it, removing clears it if it is addr.  NULL / false for 0.0.0.0. */
struct net_if_addr *net_if_ipv4_addr_add(struct net_if *iface, const struct net_in_addr *addr,
					 enum net_addr_type addr_type, uint32_t vlifetime);
bool net_if_ipv4_addr_rm(struct net_if *iface, const struct net_in_addr *addr);

/* The address if there is one in state (or any state, NET_ADDR_ANY_STATE), else NULL. */
struct net_in_addr *net_if_ipv4_get_global_addr(struct net_if *iface, enum net_addr_state state);

/* The netmask of addr, when addr is the interface's address. */
bool net_if_ipv4_set_netmask_by_addr(struct net_if *iface, const struct net_in_addr *addr,
				     const struct net_in_addr *netmask);
struct net_in_addr net_if_ipv4_get_netmask_by_addr(struct net_if *iface,
						   const struct net_in_addr *addr);

/* The default gateway (0.0.0.0 when there is none). */
void net_if_ipv4_set_gw(struct net_if *iface, const struct net_in_addr *gw);
struct net_in_addr net_if_ipv4_get_gw(struct net_if *iface);

/* Dormant: not on a network. */
void net_if_dormant_on(struct net_if *iface);
void net_if_dormant_off(struct net_if *iface);
bool net_if_is_dormant(struct net_if *iface);

bool net_if_is_admin_up(struct net_if *iface);

/* The link (MAC) address: NET_LINK_ETHERNET and 6 bytes only (else -EINVAL). */
int net_if_set_link_addr(struct net_if *iface, uint8_t *addr, uint8_t len, enum net_link_type type);
struct net_linkaddr *net_if_get_link_addr(struct net_if *iface);

/* The MTU, 0 until set. */
void net_if_set_mtu(struct net_if *iface, uint16_t mtu);
uint16_t net_if_get_mtu(struct net_if *iface);

#endif /* SHELL_PORT_ZEPHYR_NET_NET_IF_H */
