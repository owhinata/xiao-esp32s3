/*
 * lwIP's sockets for the shell's socket layer (port/net/lwip_bridge.c),
 * with types and constants of their own: the layer's side
 * (shell/port/zephyr_net_socket.c) sees the POSIX names of the shell's
 * <sys/socket.h>, <netinet/in.h>, <poll.h> and <netdb.h>, lwIP's side those
 * of lwIP and ESP-IDF's <sys/poll.h>, which give the same names other
 * values (SOL_SOCKET, SO_*, MSG_*, POLL*, EAI_*, AI_*) and other layouts
 * (struct sockaddr).  So neither side includes the other's headers; this
 * one includes only the C library's.  Each side checks what it assumes of
 * this header with _Static_assert.
 *
 * A socket is its number, 0 .. LWIP_BRIDGE_SOCKETS - 1 (lwIP's descriptor
 * less LWIP_SOCKET_OFFSET).  The calls are lwIP's lwip_*() of the same
 * names, from threads only (each thread that calls them gets lwIP's
 * semaphore of its own, port/esp_compat/pthread_local.c): 0 or a count,
 * or -1 with errno, lwIP's (newlib's numbers, which are the layer's).
 */
#ifndef PORT_NET_LWIP_BRIDGE_H
#define PORT_NET_LWIP_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/time.h>
#include <sys/types.h>

/* lwIP's sockets: CONFIG_LWIP_MAX_SOCKETS (sdkconfig.h). */
#define LWIP_BRIDGE_SOCKETS     10

/* The most descriptors one lwip_bridge_poll() takes (the layer's CONFIG_ZVFS_POLL_MAX). */
#define LWIP_BRIDGE_POLL_MAX    16

/* An IPv4 address and port, both in network byte order. */
struct lwip_bridge_addr {
    uint32_t addr;
    uint16_t port;
};

/* A send vector, laid out as both sides' struct iovec. */
struct lwip_bridge_iovec {
    void *base;
    size_t len;
};

enum lwip_bridge_type {
    LWIP_BRIDGE_STREAM,         /* TCP */
    LWIP_BRIDGE_DGRAM,          /* UDP */
};

/* The flags of send and receive. */
#define LWIP_BRIDGE_MSG_PEEK        0x1
#define LWIP_BRIDGE_MSG_DONTWAIT    0x2
#define LWIP_BRIDGE_MSG_WAITALL     0x4

/* The events of lwip_bridge_poll(). */
#define LWIP_BRIDGE_POLLIN          0x1
#define LWIP_BRIDGE_POLLOUT         0x2
#define LWIP_BRIDGE_POLLERR         0x4
#define LWIP_BRIDGE_POLLHUP         0x8
#define LWIP_BRIDGE_POLLNVAL        0x10

enum lwip_bridge_shut {
    LWIP_BRIDGE_SHUT_RD,
    LWIP_BRIDGE_SHUT_WR,
    LWIP_BRIDGE_SHUT_RDWR,
};

/* The socket options with an int value. */
enum lwip_bridge_opt {
    LWIP_BRIDGE_OPT_REUSEADDR,      /* SOL_SOCKET, SO_REUSEADDR */
    LWIP_BRIDGE_OPT_ERROR,          /* SOL_SOCKET, SO_ERROR (get only): a positive errno or 0 */
    LWIP_BRIDGE_OPT_TCP_NODELAY,    /* IPPROTO_TCP, TCP_NODELAY */
};

/* The results of lwip_bridge_getaddrinfo(). */
enum lwip_bridge_eai {
    LWIP_BRIDGE_EAI_OK,
    LWIP_BRIDGE_EAI_NONAME,     /* no such name, or the lookup failed */
    LWIP_BRIDGE_EAI_SERVICE,    /* the service is not a port number */
    LWIP_BRIDGE_EAI_FAIL,
    LWIP_BRIDGE_EAI_MEMORY,
    LWIP_BRIDGE_EAI_FAMILY,
};

/* The flags of lwip_bridge_getaddrinfo() (AI_PASSIVE, AI_NUMERICHOST). */
#define LWIP_BRIDGE_AI_PASSIVE      0x1
#define LWIP_BRIDGE_AI_NUMERICHOST  0x2

struct lwip_bridge_pollfd {
    int sock;                   /* negative: ignored */
    uint16_t events;            /* LWIP_BRIDGE_POLLIN, LWIP_BRIDGE_POLLOUT */
    uint16_t revents;           /* any LWIP_BRIDGE_POLL* */
};

/*
 * A new socket; -1 with ENETDOWN while lwIP is not running (the Wi-Fi
 * driver did not start), ENFILE when all are open.
 */
int lwip_bridge_socket(enum lwip_bridge_type type);

int lwip_bridge_bind(int sock, const struct lwip_bridge_addr *addr);
int lwip_bridge_connect(int sock, const struct lwip_bridge_addr *addr);
int lwip_bridge_listen(int sock, int backlog);

/*
 * The next connection of a listening socket, with TCP keepalive on (see
 * lwip_bridge.c), its peer's and its own address.
 */
int lwip_bridge_accept(int sock, struct lwip_bridge_addr *peer, struct lwip_bridge_addr *local);

/* TO and FROM may be NULL (a connected socket; the sender not wanted). */
ssize_t lwip_bridge_send(int sock, const void *buf, size_t len, int flags,
                         const struct lwip_bridge_addr *to);
ssize_t lwip_bridge_sendmsg(int sock, const struct lwip_bridge_iovec *iov, size_t iovcnt,
                            int flags, const struct lwip_bridge_addr *to);
ssize_t lwip_bridge_recv(int sock, void *buf, size_t len, int flags, struct lwip_bridge_addr *from);

int lwip_bridge_shutdown(int sock, enum lwip_bridge_shut how);
int lwip_bridge_close(int sock);

int lwip_bridge_getpeername(int sock, struct lwip_bridge_addr *addr);
int lwip_bridge_getsockname(int sock, struct lwip_bridge_addr *addr);

int lwip_bridge_set_int(int sock, enum lwip_bridge_opt opt, int value);
int lwip_bridge_get_int(int sock, enum lwip_bridge_opt opt, int *value);

/* SO_SNDTIMEO (SEND) or SO_RCVTIMEO: the C library's struct timeval on both sides. */
int lwip_bridge_set_timeout(int sock, bool send, const struct timeval *tv);
int lwip_bridge_get_timeout(int sock, bool send, struct timeval *tv);

/* O_NONBLOCK of fcntl(). */
int lwip_bridge_set_nonblocking(int sock, bool on);
int lwip_bridge_get_nonblocking(int sock, bool *on);

/*
 * lwip_poll() of at most LWIP_BRIDGE_POLL_MAX sockets: the number of
 * entries with events, 0 after TIMEOUT_MS (negative: no limit).  A socket
 * that is not open gets LWIP_BRIDGE_POLLNVAL, as the others their events.
 */
int lwip_bridge_poll(struct lwip_bridge_pollfd *fds, size_t nfds, int timeout_ms);

/*
 * The first IPv4 address of NODE (a name, through lwIP's DNS client and
 * the servers DHCP gave, or a dotted address) with the port SERVICE (a
 * number), into *RESULT; NODE NULL: any address (LWIP_BRIDGE_AI_PASSIVE)
 * or the loopback address.  Blocks for the lookup.
 */
enum lwip_bridge_eai lwip_bridge_getaddrinfo(const char *node, const char *service, int flags,
                                             struct lwip_bridge_addr *result);

#endif
