/* SPDX-License-Identifier: MIT */
/*
 * lwIP's sockets as the offloaded sockets of the shell's socket layer
 * (zephyr_net_socket.c): the resolver's registration.
 */
#ifndef SHELL_PORT_ZEPHYR_NET_SOCKET_H
#define SHELL_PORT_ZEPHYR_NET_SOCKET_H

/*
 * Registers lwIP's name resolution as the layer's offloaded resolver
 * (zsock_getaddrinfo()), as Zephyr's offloaded interfaces do in their
 * init (rp2040-connect's nina_net.c: nina_dns_init()).  Once.
 */
void zephyr_net_socket_dns_register(void);

#endif /* SHELL_PORT_ZEPHYR_NET_SOCKET_H */
