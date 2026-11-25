/* SPDX-License-Identifier: MIT */
/*
 * lwIP's sockets (port/net/lwip_bridge.h) as the offloaded sockets of the
 * shell's socket layer (shell/common/compat/net_offload: zsock_*() on the
 * descriptor table and poll of Zephyr's zvfs), with the POSIX types and
 * values of shell/common/compat/libc_net: the counterpart of
 * rp2040-connect's binding of its Wi-Fi module's sockets
 * (common/drivers/nina/port/zephyr/nina_socket.c).  lwIP's side, with
 * lwIP's values, is port/net/lwip_bridge.c; each side translates to and
 * from the bridge's.
 *
 * - Descriptors: the layer's (0 .. 15, the numbers "Telnet socket <n>"
 *   names); each one's object is the entry of lwIP's socket in socks[],
 *   whose index is the bridge's socket number.
 * - Blocking: lwIP's own (its sockets wait on a semaphore of the calling
 *   thread); O_NONBLOCK and the timeouts are lwIP's, set through the bridge.
 * - poll(): the whole call goes to lwip_poll() (POLL_PREPARE answers
 *   -EXDEV, then POLL_OFFLOAD), which sleeps until lwIP reports an event
 *   on one of the sockets or the timeout ends.  A descriptor that is not
 *   open is POLLNVAL, and the call then does not wait.  lwIP reports a
 *   connection the peer closed as POLLIN (the receive gives 0), not
 *   POLLHUP.
 * - Options: SO_REUSEADDR, SO_ERROR (get), SO_RCVTIMEO, SO_SNDTIMEO and
 *   TCP_NODELAY; anything else (SO_BINDTODEVICE: there is one interface)
 *   is ENOPROTOOPT.  IPv4 only (EAFNOSUPPORT otherwise).
 * - Name resolution: lwIP's DNS client with the servers DHCP gave, the
 *   first IPv4 address, in one malloc() block per result.
 * - Connections accepted are logged when they open and when they close
 *   (with their peer and how long they lasted).
 */
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/socket_offload.h>
#include <zephyr/sys/fdtable.h>
#include <zephyr/sys/util.h>

#include "app_log.h"
#include "net/lwip_bridge.h"
#include "zephyr_net_socket.h"

LOG_MODULE_REGISTER(net_socket);

_Static_assert(sizeof(struct lwip_bridge_iovec) == sizeof(struct iovec)
	       && offsetof(struct lwip_bridge_iovec, base) == offsetof(struct iovec, iov_base)
	       && offsetof(struct lwip_bridge_iovec, len) == offsetof(struct iovec, iov_len),
	       "the send vectors are the layer's struct iovec");
_Static_assert(sizeof(((struct sockaddr_in *)0)->sin_addr.s_addr) == sizeof(uint32_t)
	       && sizeof(((struct sockaddr_in *)0)->sin_port) == sizeof(uint16_t),
	       "address and port in network order, 4 and 2 bytes");
_Static_assert(LWIP_BRIDGE_POLL_MAX >= 16, "the layer polls up to CONFIG_ZVFS_POLL_MAX (16)");

/* What the binding keeps of each of lwIP's sockets. */
struct lwip_socket {
	bool accepted;
	struct lwip_bridge_addr peer;
	uint16_t local_port; /* network order */
	int64_t since_ms;
};

static struct lwip_socket socks[LWIP_BRIDGE_SOCKETS];
static const struct socket_op_vtable socket_ops;

static int sock_of(const void *obj)
{
	return (int)((const struct lwip_socket *)obj - socks);
}

static int fail(int err)
{
	errno = err;
	return -1;
}

/* An IPv4 socket address of the layer into the bridge's. */
static int addr_in(const struct net_sockaddr *addr, net_socklen_t addrlen,
		   struct lwip_bridge_addr *out)
{
	const struct net_sockaddr_in *sin = (const struct net_sockaddr_in *)addr;

	if (addr == NULL || addrlen < sizeof(*sin)) {
		return fail(EINVAL);
	}
	if (sin->sin_family != NET_AF_INET) {
		return fail(EAFNOSUPPORT);
	}
	out->addr = sin->sin_addr.s_addr;
	out->port = sin->sin_port;
	return 0;
}

/* The bridge's address into ADDR (cut to *ADDRLEN), *ADDRLEN its full size; nothing for NULL. */
static void addr_out(const struct lwip_bridge_addr *a, struct net_sockaddr *addr,
		     net_socklen_t *addrlen)
{
	struct net_sockaddr_in sin = {
		.sin_family = NET_AF_INET,
		.sin_port = a->port,
		.sin_addr = {.s_addr = a->addr},
	};

	if (addr == NULL || addrlen == NULL) {
		return;
	}
	memcpy(addr, &sin, MIN(*addrlen, sizeof(sin)));
	*addrlen = sizeof(sin);
}

static int flags_of(int flags)
{
	return ((flags & ZSOCK_MSG_PEEK) ? LWIP_BRIDGE_MSG_PEEK : 0) |
	       ((flags & ZSOCK_MSG_DONTWAIT) ? LWIP_BRIDGE_MSG_DONTWAIT : 0) |
	       ((flags & ZSOCK_MSG_WAITALL) ? LWIP_BRIDGE_MSG_WAITALL : 0);
}

static void addr_text(const struct lwip_bridge_addr *a, char *buf, size_t size)
{
	char ip[NET_INET_ADDRSTRLEN];
	struct net_in_addr in = {.s_addr = a->addr};

	(void)inet_ntop(NET_AF_INET, &in, ip, sizeof(ip));
	snprintf(buf, size, "%s:%u", ip, (unsigned int)ntohs(a->port));
}

/* The operations ------------------------------------------------------------*/

static int do_bind(void *obj, const struct net_sockaddr *addr, net_socklen_t addrlen)
{
	struct lwip_bridge_addr a;

	if (addr_in(addr, addrlen, &a) != 0) {
		return -1;
	}
	return lwip_bridge_bind(sock_of(obj), &a);
}

static int do_connect(void *obj, const struct net_sockaddr *addr, net_socklen_t addrlen)
{
	struct lwip_bridge_addr a;

	if (addr_in(addr, addrlen, &a) != 0) {
		return -1;
	}
	return lwip_bridge_connect(sock_of(obj), &a);
}

static int do_listen(void *obj, int backlog)
{
	return lwip_bridge_listen(sock_of(obj), backlog);
}

static int do_accept(void *obj, struct net_sockaddr *addr, net_socklen_t *addrlen)
{
	struct lwip_bridge_addr peer;
	struct lwip_bridge_addr local;
	char text[NET_INET_ADDRSTRLEN + 6];
	int fd;
	int sock;

	if ((addr == NULL) != (addrlen == NULL)) {
		return fail(EINVAL);
	}
	fd = zvfs_reserve_fd();
	if (fd < 0) {
		return -1;
	}
	sock = lwip_bridge_accept(sock_of(obj), &peer, &local);
	if (sock < 0) {
		int err = errno;

		zvfs_free_fd(fd);
		return fail(err);
	}
	socks[sock] = (struct lwip_socket){
		.accepted = true,
		.peer = peer,
		.local_port = local.port,
		.since_ms = k_uptime_get(),
	};
	addr_out(&peer, addr, addrlen);
	zvfs_finalize_typed_fd(fd, &socks[sock], (const struct fd_op_vtable *)&socket_ops,
			       ZVFS_MODE_IFSOCK);
	addr_text(&peer, text, sizeof(text));
	LOG_INF("socket %d: %s connected to port %u", fd, text, (unsigned int)ntohs(local.port));
	return fd;
}

static ssize_t do_sendto(void *obj, const void *buf, size_t len, int flags,
			 const struct net_sockaddr *dest_addr, net_socklen_t addrlen)
{
	struct lwip_bridge_addr to;

	if (dest_addr == NULL) {
		return lwip_bridge_send(sock_of(obj), buf, len, flags_of(flags), NULL);
	}
	if (addr_in(dest_addr, addrlen, &to) != 0) {
		return -1;
	}
	return lwip_bridge_send(sock_of(obj), buf, len, flags_of(flags), &to);
}

static ssize_t do_sendmsg(void *obj, const struct net_msghdr *msg, int flags)
{
	struct lwip_bridge_addr to;

	if (msg == NULL || msg->msg_controllen != 0) {
		return fail(msg == NULL ? EINVAL : EOPNOTSUPP);
	}
	if (msg->msg_name != NULL &&
	    addr_in(msg->msg_name, msg->msg_namelen, &to) != 0) {
		return -1;
	}
	return lwip_bridge_sendmsg(sock_of(obj), (const struct lwip_bridge_iovec *)msg->msg_iov,
				   msg->msg_iovlen, flags_of(flags),
				   msg->msg_name != NULL ? &to : NULL);
}

static ssize_t do_recvfrom(void *obj, void *buf, size_t max_len, int flags,
			   struct net_sockaddr *src_addr, net_socklen_t *addrlen)
{
	struct lwip_bridge_addr from;
	ssize_t n;

	if ((src_addr == NULL) != (addrlen == NULL)) {
		return fail(EINVAL);
	}
	if (src_addr == NULL) {
		return lwip_bridge_recv(sock_of(obj), buf, max_len, flags_of(flags), NULL);
	}
	n = lwip_bridge_recv(sock_of(obj), buf, max_len, flags_of(flags), &from);
	if (n >= 0) {
		addr_out(&from, src_addr, addrlen);
	}
	return n;
}

static int do_shutdown(void *obj, int how)
{
	switch (how) {
	case ZSOCK_SHUT_RD:
		return lwip_bridge_shutdown(sock_of(obj), LWIP_BRIDGE_SHUT_RD);
	case ZSOCK_SHUT_WR:
		return lwip_bridge_shutdown(sock_of(obj), LWIP_BRIDGE_SHUT_WR);
	case ZSOCK_SHUT_RDWR:
		return lwip_bridge_shutdown(sock_of(obj), LWIP_BRIDGE_SHUT_RDWR);
	default:
		return fail(EINVAL);
	}
}

static int do_getpeername(void *obj, struct net_sockaddr *addr, net_socklen_t *addrlen)
{
	struct lwip_bridge_addr a;

	if (addr == NULL || addrlen == NULL) {
		return fail(EINVAL);
	}
	if (lwip_bridge_getpeername(sock_of(obj), &a) != 0) {
		return -1;
	}
	addr_out(&a, addr, addrlen);
	return 0;
}

static int do_getsockname(void *obj, struct net_sockaddr *addr, net_socklen_t *addrlen)
{
	struct lwip_bridge_addr a;

	if (addr == NULL || addrlen == NULL) {
		return fail(EINVAL);
	}
	if (lwip_bridge_getsockname(sock_of(obj), &a) != 0) {
		return -1;
	}
	addr_out(&a, addr, addrlen);
	return 0;
}

/* The int options: their bridge name, or false (ENOPROTOOPT). */
static bool int_opt(int level, int optname, enum lwip_bridge_opt *opt)
{
	if (level == ZSOCK_SOL_SOCKET && optname == ZSOCK_SO_REUSEADDR) {
		*opt = LWIP_BRIDGE_OPT_REUSEADDR;
	} else if (level == ZSOCK_SOL_SOCKET && optname == ZSOCK_SO_ERROR) {
		*opt = LWIP_BRIDGE_OPT_ERROR;
	} else if (level == NET_IPPROTO_TCP && optname == ZSOCK_TCP_NODELAY) {
		*opt = LWIP_BRIDGE_OPT_TCP_NODELAY;
	} else {
		return false;
	}
	return true;
}

static bool timeout_opt(int level, int optname, bool *send)
{
	if (level != ZSOCK_SOL_SOCKET ||
	    (optname != ZSOCK_SO_SNDTIMEO && optname != ZSOCK_SO_RCVTIMEO)) {
		return false;
	}
	*send = optname == ZSOCK_SO_SNDTIMEO;
	return true;
}

static int do_setsockopt(void *obj, int level, int optname, const void *optval,
			 net_socklen_t optlen)
{
	enum lwip_bridge_opt opt;
	bool send;

	if (int_opt(level, optname, &opt) && opt != LWIP_BRIDGE_OPT_ERROR) {
		if (optval == NULL || optlen < sizeof(int)) {
			return fail(EINVAL);
		}
		return lwip_bridge_set_int(sock_of(obj), opt, *(const int *)optval);
	}
	if (timeout_opt(level, optname, &send)) {
		if (optval == NULL || optlen < sizeof(struct zsock_timeval)) {
			return fail(EINVAL);
		}
		return lwip_bridge_set_timeout(sock_of(obj), send, optval);
	}
	return fail(ENOPROTOOPT);
}

static int do_getsockopt(void *obj, int level, int optname, void *optval, net_socklen_t *optlen)
{
	enum lwip_bridge_opt opt;
	bool send;

	if (optval == NULL || optlen == NULL) {
		return fail(EINVAL);
	}
	if (int_opt(level, optname, &opt)) {
		if (*optlen < sizeof(int)) {
			return fail(EINVAL);
		}
		*optlen = sizeof(int);
		return lwip_bridge_get_int(sock_of(obj), opt, optval);
	}
	if (timeout_opt(level, optname, &send)) {
		if (*optlen < sizeof(struct zsock_timeval)) {
			return fail(EINVAL);
		}
		*optlen = sizeof(struct zsock_timeval);
		return lwip_bridge_get_timeout(sock_of(obj), send, optval);
	}
	return fail(ENOPROTOOPT);
}

static int do_close2(void *obj, int fd)
{
	struct lwip_socket *s = obj;

	if (s->accepted) {
		char text[NET_INET_ADDRSTRLEN + 6];
		int64_t ms = k_uptime_get() - s->since_ms;

		addr_text(&s->peer, text, sizeof(text));
		LOG_INF("socket %d: %s on port %u closed after %lld.%03d s", fd, text,
			(unsigned int)ntohs(s->local_port), (long long)(ms / 1000), (int)(ms % 1000));
	}
	*s = (struct lwip_socket){0};
	return lwip_bridge_close(sock_of(obj));
}

static ssize_t do_read(void *obj, void *buf, size_t sz)
{
	return lwip_bridge_recv(sock_of(obj), buf, sz, 0, NULL);
}

static ssize_t do_write(void *obj, const void *buf, size_t sz)
{
	return lwip_bridge_send(sock_of(obj), buf, sz, 0, NULL);
}

static uint16_t events_of(short events)
{
	return (uint16_t)(((events & ZSOCK_POLLIN) ? LWIP_BRIDGE_POLLIN : 0) |
			  ((events & ZSOCK_POLLOUT) ? LWIP_BRIDGE_POLLOUT : 0));
}

static short revents_of(uint16_t revents)
{
	return (short)(((revents & LWIP_BRIDGE_POLLIN) ? ZSOCK_POLLIN : 0) |
		       ((revents & LWIP_BRIDGE_POLLOUT) ? ZSOCK_POLLOUT : 0) |
		       ((revents & LWIP_BRIDGE_POLLERR) ? ZSOCK_POLLERR : 0) |
		       ((revents & LWIP_BRIDGE_POLLHUP) ? ZSOCK_POLLHUP : 0) |
		       ((revents & LWIP_BRIDGE_POLLNVAL) ? ZSOCK_POLLNVAL : 0));
}

/* ZFD_IOCTL_POLL_OFFLOAD: every descriptor of the call (zvfs_poll() has released their locks). */
static int do_poll(struct zsock_pollfd *fds, int nfds, int timeout)
{
	struct lwip_bridge_pollfd bfds[LWIP_BRIDGE_POLL_MAX];
	bool invalid = false;
	int ready = 0;
	int ret;

	if (nfds <= 0 || nfds > LWIP_BRIDGE_POLL_MAX) {
		return fail(EINVAL);
	}
	for (int i = 0; i < nfds; i++) {
		const struct fd_op_vtable *vtable;
		void *obj;

		fds[i].revents = 0;
		bfds[i] = (struct lwip_bridge_pollfd){.sock = -1};
		if (fds[i].fd < 0) {
			continue;
		}
		obj = zvfs_get_fd_obj_and_vtable(fds[i].fd, &vtable, NULL);
		if (obj == NULL || vtable != (const struct fd_op_vtable *)&socket_ops) {
			fds[i].revents = ZSOCK_POLLNVAL;
			invalid = true;
			continue;
		}
		bfds[i].sock = sock_of(obj);
		bfds[i].events = events_of(fds[i].events);
	}
	ret = lwip_bridge_poll(bfds, (size_t)nfds, invalid ? 0 : timeout);
	if (ret < 0) {
		return -1;
	}
	for (int i = 0; i < nfds; i++) {
		fds[i].revents |= revents_of(bfds[i].revents);
		if (fds[i].revents != 0) {
			ready++;
		}
	}
	return ready;
}

static int do_ioctl(void *obj, unsigned int request, va_list args)
{
	switch (request) {
	case ZVFS_F_GETFL: {
		bool on;

		if (lwip_bridge_get_nonblocking(sock_of(obj), &on) != 0) {
			return -1;
		}
		return on ? ZVFS_O_NONBLOCK : 0;
	}
	case ZVFS_F_SETFL: {
		int flags = va_arg(args, int);

		if (flags & ~ZVFS_O_NONBLOCK) {
			return fail(EOPNOTSUPP);
		}
		return lwip_bridge_set_nonblocking(sock_of(obj), (flags & ZVFS_O_NONBLOCK) != 0);
	}
	case ZFD_IOCTL_SET_LOCK:
		/* The descriptor's mutex: lwIP's sockets wait on their own. */
		return 0;
	case ZFD_IOCTL_POLL_PREPARE:
		return -EXDEV;
	case ZFD_IOCTL_POLL_OFFLOAD: {
		struct zsock_pollfd *fds = va_arg(args, struct zsock_pollfd *);
		int nfds = va_arg(args, int);
		int timeout = va_arg(args, int);

		return do_poll(fds, nfds, timeout);
	}
	default:
		return fail(ENOTSUP);
	}
}

static const struct socket_op_vtable socket_ops = {
	.fd_vtable = {.read = do_read, .write = do_write, .close2 = do_close2, .ioctl = do_ioctl},
	.shutdown = do_shutdown,
	.bind = do_bind,
	.connect = do_connect,
	.listen = do_listen,
	.accept = do_accept,
	.sendto = do_sendto,
	.recvfrom = do_recvfrom,
	.getsockopt = do_getsockopt,
	.setsockopt = do_setsockopt,
	.sendmsg = do_sendmsg,
	.getpeername = do_getpeername,
	.getsockname = do_getsockname,
};

/* Registration --------------------------------------------------------------*/

static bool is_supported(int family, int type, int proto)
{
	return family == NET_AF_INET &&
	       ((type == NET_SOCK_STREAM && (proto == 0 || proto == NET_IPPROTO_TCP)) ||
		(type == NET_SOCK_DGRAM && (proto == 0 || proto == NET_IPPROTO_UDP)));
}

static int socket_create(int family, int type, int proto)
{
	int fd;
	int sock;

	if (!is_supported(family, type, proto)) {
		return fail(EAFNOSUPPORT);
	}
	fd = zvfs_reserve_fd();
	if (fd < 0) {
		return -1;
	}
	sock = lwip_bridge_socket(type == NET_SOCK_STREAM ? LWIP_BRIDGE_STREAM : LWIP_BRIDGE_DGRAM);
	if (sock < 0) {
		int err = errno;

		zvfs_free_fd(fd);
		return fail(err);
	}
	socks[sock] = (struct lwip_socket){0};
	zvfs_finalize_typed_fd(fd, &socks[sock], (const struct fd_op_vtable *)&socket_ops,
			       ZVFS_MODE_IFSOCK);
	return fd;
}

NET_SOCKET_OFFLOAD_REGISTER(lwip, CONFIG_NET_SOCKETS_OFFLOAD_PRIORITY, NET_AF_INET, is_supported,
			    socket_create);

/* Name resolution -----------------------------------------------------------*/

/* One result: the addrinfo and its address in one block, freed with the addrinfo. */
struct addrinfo_block {
	struct zsock_addrinfo ai;
	struct net_sockaddr_in sin;
};

static int dns_getaddrinfo(const char *node, const char *service,
			   const struct zsock_addrinfo *hints, struct zsock_addrinfo **res)
{
	int family = hints != NULL ? hints->ai_family : NET_AF_UNSPEC;
	int socktype = hints != NULL ? hints->ai_socktype : 0;
	int protocol = hints != NULL ? hints->ai_protocol : 0;
	int flags = 0;
	struct lwip_bridge_addr addr;
	struct addrinfo_block *block;

	if (res == NULL || (node == NULL && service == NULL)) {
		return EAI_NONAME;
	}
	if (family != NET_AF_UNSPEC && family != NET_AF_INET) {
		return EAI_FAMILY;
	}
	if (socktype != 0 && socktype != NET_SOCK_STREAM && socktype != NET_SOCK_DGRAM) {
		return EAI_SOCKTYPE;
	}
	if (hints != NULL && (hints->ai_flags & ZSOCK_AI_PASSIVE)) {
		flags |= LWIP_BRIDGE_AI_PASSIVE;
	}
	if (hints != NULL && (hints->ai_flags & ZSOCK_AI_NUMERICHOST)) {
		flags |= LWIP_BRIDGE_AI_NUMERICHOST;
	}

	switch (lwip_bridge_getaddrinfo(node, service, flags, &addr)) {
	case LWIP_BRIDGE_EAI_OK:
		break;
	case LWIP_BRIDGE_EAI_NONAME:
		return EAI_NONAME;
	case LWIP_BRIDGE_EAI_SERVICE:
		return EAI_SERVICE;
	case LWIP_BRIDGE_EAI_MEMORY:
		return EAI_MEMORY;
	case LWIP_BRIDGE_EAI_FAMILY:
		return EAI_FAMILY;
	default:
		return EAI_FAIL;
	}

	block = calloc(1, sizeof(*block));
	if (block == NULL) {
		return EAI_MEMORY;
	}
	block->sin.sin_family = NET_AF_INET;
	block->sin.sin_port = addr.port;
	block->sin.sin_addr.s_addr = addr.addr;
	block->ai.ai_family = NET_AF_INET;
	block->ai.ai_socktype = socktype;
	block->ai.ai_protocol = protocol;
	block->ai.ai_addrlen = sizeof(block->sin);
	block->ai.ai_addr = (struct net_sockaddr *)&block->sin;
	*res = &block->ai;
	return 0;
}

static void dns_freeaddrinfo(struct zsock_addrinfo *ai)
{
	while (ai != NULL) {
		struct zsock_addrinfo *next = ai->ai_next;

		free(ai);
		ai = next;
	}
}

static const struct socket_dns_offload dns_ops = {
	.getaddrinfo = dns_getaddrinfo,
	.freeaddrinfo = dns_freeaddrinfo,
};

void zephyr_net_socket_dns_register(void)
{
	socket_offload_dns_register(&dns_ops);
}
