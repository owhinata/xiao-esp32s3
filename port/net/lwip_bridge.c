/*
 * lwIP's side of the shell's sockets (lwip_bridge.h): the lwip_*() calls
 * with the values of lwIP and ESP-IDF's <sys/poll.h>, translated from and to
 * the bridge's own.  The other side, with the POSIX values of the shell's
 * headers, is shell/port/zephyr_net_socket.c.
 */
#include <errno.h>
#include <stddef.h>
#include <string.h>

#include "lwip/opt.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "lwip_bridge.h"
#include "wlanif.h"

#if !LWIP_NETCONN_SEM_PER_THREAD || !LWIP_SOCKET_POLL || !SO_REUSE \
    || !LWIP_SO_SNDTIMEO || !LWIP_SO_RCVTIMEO || LWIP_SO_SNDRCVTIMEO_NONSTANDARD \
    || LWIP_TIMEVAL_PRIVATE || LWIP_IPV6
#error "lwip_bridge.c needs lwIP's poll, SO_REUSEADDR, timeouts in a struct timeval, a semaphore per thread, IPv4 only"
#endif

_Static_assert(LWIP_BRIDGE_SOCKETS == MEMP_NUM_NETCONN, "the bridge numbers lwIP's sockets");
_Static_assert(LWIP_SOCKET_OFFSET + MEMP_NUM_NETCONN <= FD_SETSIZE, "lwIP's descriptors");
_Static_assert(EWOULDBLOCK == EAGAIN, "a socket without data says EAGAIN, as the layer expects");
_Static_assert(sizeof(struct lwip_bridge_iovec) == sizeof(struct iovec)
               && offsetof(struct lwip_bridge_iovec, base) == offsetof(struct iovec, iov_base)
               && offsetof(struct lwip_bridge_iovec, len) == offsetof(struct iovec, iov_len),
               "the send vectors are lwIP's struct iovec");
_Static_assert(sizeof(((struct sockaddr_in *) 0)->sin_addr.s_addr) == sizeof(uint32_t)
               && sizeof(((struct sockaddr_in *) 0)->sin_port) == sizeof(uint16_t),
               "address and port in network order, 4 and 2 bytes");

static int fd_of(int sock)
{
    return sock + LWIP_SOCKET_OFFSET;
}

/* lwIP's descriptor to the bridge's number; anything else: -1 with ENFILE. */
static int sock_of(int fd)
{
    if (fd < LWIP_SOCKET_OFFSET || fd >= LWIP_SOCKET_OFFSET + MEMP_NUM_NETCONN) {
        errno = ENFILE;
        return -1;
    }
    return fd - LWIP_SOCKET_OFFSET;
}

static void to_lwip(const struct lwip_bridge_addr *a, struct sockaddr_in *sin)
{
    memset(sin, 0, sizeof(*sin));
    sin->sin_len = sizeof(*sin);
    sin->sin_family = AF_INET;
    sin->sin_port = a->port;
    sin->sin_addr.s_addr = a->addr;
}

static int from_lwip(const struct sockaddr_in *sin, socklen_t len, struct lwip_bridge_addr *a)
{
    if (len < sizeof(*sin) || sin->sin_family != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    a->addr = sin->sin_addr.s_addr;
    a->port = sin->sin_port;
    return 0;
}

static int flags_to_lwip(int flags)
{
    return ((flags & LWIP_BRIDGE_MSG_PEEK) ? MSG_PEEK : 0)
           | ((flags & LWIP_BRIDGE_MSG_DONTWAIT) ? MSG_DONTWAIT : 0)
           | ((flags & LWIP_BRIDGE_MSG_WAITALL) ? MSG_WAITALL : 0);
}

int lwip_bridge_socket(enum lwip_bridge_type type)
{
    int fd;
    int sock;

    if (!wlanif_started()) {
        errno = ENETDOWN;
        return -1;
    }
    fd = lwip_socket(AF_INET, type == LWIP_BRIDGE_STREAM ? SOCK_STREAM : SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }
    sock = sock_of(fd);
    if (sock < 0) {
        (void) lwip_close(fd);
    }
    return sock;
}

int lwip_bridge_bind(int sock, const struct lwip_bridge_addr *addr)
{
    struct sockaddr_in sin;

    to_lwip(addr, &sin);
    return lwip_bind(fd_of(sock), (struct sockaddr *) &sin, sizeof(sin));
}

int lwip_bridge_connect(int sock, const struct lwip_bridge_addr *addr)
{
    struct sockaddr_in sin;

    to_lwip(addr, &sin);
    return lwip_connect(fd_of(sock), (struct sockaddr *) &sin, sizeof(sin));
}

int lwip_bridge_listen(int sock, int backlog)
{
    return lwip_listen(fd_of(sock), backlog);
}

int lwip_bridge_accept(int sock, struct lwip_bridge_addr *peer, struct lwip_bridge_addr *local)
{
    struct sockaddr_in sin;
    socklen_t len = sizeof(sin);
    int fd = lwip_accept(fd_of(sock), (struct sockaddr *) &sin, &len);
    int new_sock;

    if (fd < 0) {
        return -1;
    }
    new_sock = sock_of(fd);
    if (new_sock < 0 || from_lwip(&sin, len, peer) != 0
        || lwip_bridge_getsockname(new_sock, local) != 0) {
        int err = errno;

        (void) lwip_close(fd);
        errno = err;
        return -1;
    }
    return new_sock;
}

ssize_t lwip_bridge_send(int sock, const void *buf, size_t len, int flags,
                         const struct lwip_bridge_addr *to)
{
    struct sockaddr_in sin;

    if (to == NULL) {
        return lwip_send(fd_of(sock), buf, len, flags_to_lwip(flags));
    }
    to_lwip(to, &sin);
    return lwip_sendto(fd_of(sock), buf, len, flags_to_lwip(flags), (struct sockaddr *) &sin,
                       sizeof(sin));
}

ssize_t lwip_bridge_sendmsg(int sock, const struct lwip_bridge_iovec *iov, size_t iovcnt,
                            int flags, const struct lwip_bridge_addr *to)
{
    struct sockaddr_in sin;
    struct msghdr msg;

    if (iovcnt > IOV_MAX) {
        errno = EMSGSIZE;
        return -1;
    }
    memset(&msg, 0, sizeof(msg));
    if (to != NULL) {
        to_lwip(to, &sin);
        msg.msg_name = &sin;
        msg.msg_namelen = sizeof(sin);
    }
    msg.msg_iov = (struct iovec *) iov;
    msg.msg_iovlen = (msg_iovlen_t) iovcnt;
    return lwip_sendmsg(fd_of(sock), &msg, flags_to_lwip(flags));
}

ssize_t lwip_bridge_recv(int sock, void *buf, size_t len, int flags, struct lwip_bridge_addr *from)
{
    struct sockaddr_in sin;
    socklen_t sin_len = sizeof(sin);
    ssize_t n;

    if (from == NULL) {
        return lwip_recv(fd_of(sock), buf, len, flags_to_lwip(flags));
    }
    n = lwip_recvfrom(fd_of(sock), buf, len, flags_to_lwip(flags), (struct sockaddr *) &sin,
                      &sin_len);
    if (n >= 0 && from_lwip(&sin, sin_len, from) != 0) {
        memset(from, 0, sizeof(*from));
    }
    return n;
}

int lwip_bridge_shutdown(int sock, enum lwip_bridge_shut how)
{
    static const int hows[] = {
        [LWIP_BRIDGE_SHUT_RD] = SHUT_RD,
        [LWIP_BRIDGE_SHUT_WR] = SHUT_WR,
        [LWIP_BRIDGE_SHUT_RDWR] = SHUT_RDWR,
    };

    return lwip_shutdown(fd_of(sock), hows[how]);
}

int lwip_bridge_close(int sock)
{
    return lwip_close(fd_of(sock));
}

int lwip_bridge_getpeername(int sock, struct lwip_bridge_addr *addr)
{
    struct sockaddr_in sin;
    socklen_t len = sizeof(sin);

    if (lwip_getpeername(fd_of(sock), (struct sockaddr *) &sin, &len) != 0) {
        return -1;
    }
    return from_lwip(&sin, len, addr);
}

int lwip_bridge_getsockname(int sock, struct lwip_bridge_addr *addr)
{
    struct sockaddr_in sin;
    socklen_t len = sizeof(sin);

    if (lwip_getsockname(fd_of(sock), (struct sockaddr *) &sin, &len) != 0) {
        return -1;
    }
    return from_lwip(&sin, len, addr);
}

static void opt_of(enum lwip_bridge_opt opt, int *level, int *name)
{
    switch (opt) {
    case LWIP_BRIDGE_OPT_REUSEADDR:
        *level = SOL_SOCKET;
        *name = SO_REUSEADDR;
        break;
    case LWIP_BRIDGE_OPT_ERROR:
        *level = SOL_SOCKET;
        *name = SO_ERROR;
        break;
    default:
        *level = IPPROTO_TCP;
        *name = TCP_NODELAY;
        break;
    }
}

int lwip_bridge_set_int(int sock, enum lwip_bridge_opt opt, int value)
{
    int level;
    int name;

    opt_of(opt, &level, &name);
    return lwip_setsockopt(fd_of(sock), level, name, &value, sizeof(value));
}

int lwip_bridge_get_int(int sock, enum lwip_bridge_opt opt, int *value)
{
    socklen_t len = sizeof(*value);
    int level;
    int name;

    opt_of(opt, &level, &name);
    return lwip_getsockopt(fd_of(sock), level, name, value, &len);
}

int lwip_bridge_set_timeout(int sock, bool send, const struct timeval *tv)
{
    return lwip_setsockopt(fd_of(sock), SOL_SOCKET, send ? SO_SNDTIMEO : SO_RCVTIMEO, tv,
                           sizeof(*tv));
}

int lwip_bridge_get_timeout(int sock, bool send, struct timeval *tv)
{
    socklen_t len = sizeof(*tv);

    return lwip_getsockopt(fd_of(sock), SOL_SOCKET, send ? SO_SNDTIMEO : SO_RCVTIMEO, tv, &len);
}

int lwip_bridge_set_nonblocking(int sock, bool on)
{
    return lwip_fcntl(fd_of(sock), F_SETFL, on ? O_NONBLOCK : 0);
}

int lwip_bridge_get_nonblocking(int sock, bool *on)
{
    int flags = lwip_fcntl(fd_of(sock), F_GETFL, 0);

    if (flags < 0) {
        return -1;
    }
    *on = (flags & O_NONBLOCK) != 0;
    return 0;
}

static short events_to_lwip(uint16_t events)
{
    return (short) (((events & LWIP_BRIDGE_POLLIN) ? POLLIN : 0)
                    | ((events & LWIP_BRIDGE_POLLOUT) ? POLLOUT : 0));
}

static uint16_t events_from_lwip(short revents)
{
    return (uint16_t) (((revents & POLLIN) ? LWIP_BRIDGE_POLLIN : 0)
                       | ((revents & POLLOUT) ? LWIP_BRIDGE_POLLOUT : 0)
                       | ((revents & POLLERR) ? LWIP_BRIDGE_POLLERR : 0)
                       | ((revents & POLLHUP) ? LWIP_BRIDGE_POLLHUP : 0)
                       | ((revents & POLLNVAL) ? LWIP_BRIDGE_POLLNVAL : 0));
}

int lwip_bridge_poll(struct lwip_bridge_pollfd *fds, size_t nfds, int timeout_ms)
{
    struct pollfd pfds[LWIP_BRIDGE_POLL_MAX];
    int ready;

    if (nfds == 0u || nfds > LWIP_BRIDGE_POLL_MAX) {
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 0; i < nfds; i++) {
        pfds[i].fd = fds[i].sock < 0 ? -1 : fd_of(fds[i].sock);
        pfds[i].events = events_to_lwip(fds[i].events);
        pfds[i].revents = 0;
    }

    errno = 0;
    ready = lwip_poll(pfds, (nfds_t) nfds, timeout_ms);
    if (ready < 0) {
        if (errno == ENOMEM || errno == EINVAL) {
            /* No semaphore for the calling thread. */
            return -1;
        }
        /*
         * lwip_pollscan() ends the whole call, without an errno, when one
         * socket is not open (closed by another thread meanwhile): each on
         * its own, without waiting, to tell which.
         */
        ready = 0;
        for (size_t i = 0; i < nfds; i++) {
            struct pollfd one = pfds[i];

            if (one.fd < 0) {
                continue;
            }
            if (lwip_poll(&one, 1, 0) < 0) {
                one.revents = POLLNVAL;
            }
            pfds[i].revents = one.revents;
            if (one.revents != 0) {
                ready++;
            }
        }
    }
    for (size_t i = 0; i < nfds; i++) {
        fds[i].revents = events_from_lwip(pfds[i].revents);
    }
    return ready;
}

static enum lwip_bridge_eai eai_of(int err)
{
    switch (err) {
    case 0:
        return LWIP_BRIDGE_EAI_OK;
    case EAI_SERVICE:
        return LWIP_BRIDGE_EAI_SERVICE;
    case EAI_MEMORY:
        return LWIP_BRIDGE_EAI_MEMORY;
    case EAI_FAMILY:
        return LWIP_BRIDGE_EAI_FAMILY;
    case EAI_FAIL:
        return LWIP_BRIDGE_EAI_FAIL;
    default:
        /* EAI_NONAME, HOST_NOT_FOUND, NO_DATA, NO_RECOVERY, TRY_AGAIN */
        return LWIP_BRIDGE_EAI_NONAME;
    }
}

enum lwip_bridge_eai lwip_bridge_getaddrinfo(const char *node, const char *service, int flags,
                                             struct lwip_bridge_addr *result)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    enum lwip_bridge_eai eai;

    if (!wlanif_started()) {
        return LWIP_BRIDGE_EAI_FAIL;
    }
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_flags = ((flags & LWIP_BRIDGE_AI_PASSIVE) ? AI_PASSIVE : 0)
                     | ((flags & LWIP_BRIDGE_AI_NUMERICHOST) ? AI_NUMERICHOST : 0);
    eai = eai_of(lwip_getaddrinfo(node, service, &hints, &res));
    if (eai == LWIP_BRIDGE_EAI_OK) {
        if (res == NULL || res->ai_addr == NULL
            || from_lwip((const struct sockaddr_in *) res->ai_addr, res->ai_addrlen, result) != 0) {
            eai = LWIP_BRIDGE_EAI_FAIL;
        }
    }
    if (res != NULL) {
        lwip_freeaddrinfo(res);
    }
    return eai;
}
