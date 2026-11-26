/* SPDX-License-Identifier: MIT */
/*
 * The OTA service (ota_service.h): the thread "ota_service" listens on TCP
 * port OTA_SERVICE_PORT and serves one client at a time, one command per
 * connection, on the shell's sockets (zsock_*(), as the Telnet shell and
 * the zperf server use them).  The update itself is port/ota_slots.h.
 *
 * The client sends a header of 16 bytes (integers little-endian):
 *
 *   0  "XOTA"
 *   4  version, 1
 *   5  command: 1 write an image, 2 confirm the running image
 *   6  flags: bit 0, restart once the image is written and switched to
 *   7  reserved
 *   8  write: the image's size in bytes, then 4 reserved bytes;
 *      confirm: the first 8 bytes of the ELF SHA-256 of the image the
 *      client expects to run (esp_app_desc_t.app_elf_sha256)
 *
 * and the board answers with lines of text, each ending in "\n":
 *
 *   write    "OK <slot>" after the header, the image may follow, or
 *            "ERR <reason>"; then, once the whole image is in, checked and
 *            made the one to boot, "OK", or "ERR <reason>".  With bit 0 the
 *            board restarts 0.5 s after the "OK".
 *   confirm  "OK <slot> <state> <sha>": that image runs, and was confirmed
 *            if it was pending (the state after); "ERR other <slot>
 *            <state> <sha>": another image runs, nothing changed; or "ERR
 *            <reason>".  <sha> is the first 16 hex digits of the running
 *            image's ELF SHA-256.
 *
 *   reasons  header, version, command (the header); busy (another client
 *            or update), noslot, pending (this image waits for its
 *            confirmation), size, memory (begin); image, nohash, project
 *            (the image's first bytes), flash (a write or erase failed),
 *            timeout (nothing came for RECV_TIMEOUT_MS), verify (the image
 *            as written does not check out), otadata (the switch failed);
 *            notota (confirm: not running from an OTA slot).
 *
 * A failure at any point aborts the update (port/ota_slots.h: the slot is
 * closed again, otadata keeps selecting the running image) and closes the
 * connection; the service then waits for the next client.  After an
 * "ERR" during the image the service reads and drops what still comes for
 * up to DRAIN_MS, so that the client sees the line before the connection
 * is closed under its data.
 *
 * A client that connects while another is served gets "ERR busy" and is
 * closed at once.  When the image is accepted, a USB audio stream that runs
 * is stopped (port/usb/audio.h): each sector erase masks the interrupts for
 * tens of milliseconds, which would cost the stream its packets.  It is not
 * started again.
 *
 * The transfer goes in blocks of CHUNK bytes, one flash sector, in a buffer
 * taken from the heap for the length of the transfer.  The service holds no
 * lock while it waits for the network or the flash: the Telnet shell and
 * the serial console go on (except while a sector erase or a program chunk
 * masks the interrupts).  The start, every tenth of the image and the
 * result are logged.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>

#include "app_log.h"
#include "board.h"
#include "ota_service.h"
#include "ota_slots.h"
#include "priorities.h"
#include "usb/audio.h"

LOG_MODULE_REGISTER(ota, LOG_LEVEL_INF);

#define HEADER_SIZE         16u
#define HEADER_VERSION      1u
#define CMD_WRITE           1u
#define CMD_CONFIRM         2u
#define FLAG_RESTART        0x01u

#define CHUNK               4096u
#define RECV_TIMEOUT_MS     10000
#define SEND_TIMEOUT_MS     10000
#define DRAIN_MS            2000
#define RESTART_DELAY_MS    500
#define SETUP_RETRY_MS      1000
#define ACCEPT_POLL_MS      1000

BUILD_ASSERT(HEADER_SIZE - 8u == OTA_SHA_PREFIX_BYTES, "the confirmation's SHA-256 prefix ends the header");

static K_THREAD_STACK_DEFINE(service_stack, APP_STACK_OTA_SERVICE);
static k_tid_t service_tid;
static volatile int listen_fd = -1;

bool ota_service_listening(void)
{
	return listen_fd >= 0;
}

static int setup_server(void)
{
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(OTA_SERVICE_PORT),
		.sin_addr = {.s_addr = htonl(INADDR_ANY)},
	};
	int on = 1;
	int fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	if (fd < 0) {
		return -errno;
	}
	/*
	 * The connections this side closed wait in TIME_WAIT on the port, which
	 * a listening socket set up again after an error has to share.
	 */
	(void)zsock_setsockopt(fd, ZSOCK_SOL_SOCKET, ZSOCK_SO_REUSEADDR, &on, sizeof(on));
	if (zsock_bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || zsock_listen(fd, 1) < 0) {
		int ret = -errno;

		(void)zsock_close(fd);
		return ret;
	}
	listen_fd = fd;
	return 0;
}

static void close_server(void)
{
	if (listen_fd >= 0) {
		(void)zsock_close(listen_fd);
		listen_fd = -1;
	}
}

static void set_timeout(int fd, int optname, int ms)
{
	struct zsock_timeval tv = {.tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000};

	(void)zsock_setsockopt(fd, ZSOCK_SOL_SOCKET, optname, &tv, sizeof(tv));
}

/* A client that came while another is served. */
static void refuse_client(void)
{
	static const char busy[] = "ERR busy\n";
	int fd = zsock_accept(listen_fd, NULL, NULL);

	if (fd < 0) {
		return;
	}
	(void)zsock_send(fd, busy, sizeof(busy) - 1u, ZSOCK_MSG_DONTWAIT);
	(void)zsock_close(fd);
	LOG_WRN("Refused a second client");
}

/*
 * Waits until fd has data or an end, up to timeout_ms, refusing other
 * clients meanwhile.  0, -ETIMEDOUT, or a negative errno of the poll.
 */
static int wait_readable(int fd, int timeout_ms)
{
	int64_t deadline = k_uptime_get() + timeout_ms;

	for (;;) {
		struct zsock_pollfd fds[2] = {
			{.fd = fd, .events = ZSOCK_POLLIN},
			{.fd = listen_fd, .events = ZSOCK_POLLIN},
		};
		int64_t left = deadline - k_uptime_get();
		int ret;

		if (left <= 0) {
			return -ETIMEDOUT;
		}
		ret = zsock_poll(fds, ARRAY_SIZE(fds), (int)left);
		if (ret < 0) {
			return -errno;
		}
		if (ret == 0) {
			return -ETIMEDOUT;
		}
		if (fds[1].fd >= 0 && (fds[1].revents & (ZSOCK_POLLERR | ZSOCK_POLLNVAL))) {
			/* Set up again by the service once this client is done. */
			close_server();
		} else if (fds[1].revents & ZSOCK_POLLIN) {
			refuse_client();
		}
		if (fds[0].revents != 0) {
			return 0;
		}
	}
}

/* Exactly len bytes into dst: 0, -ETIMEDOUT, -ECONNRESET (the client closed), or a negative errno. */
static int recv_exact(int fd, void *dst, size_t len)
{
	uint8_t *pos = dst;

	while (len > 0u) {
		ssize_t n;
		int ret = wait_readable(fd, RECV_TIMEOUT_MS);

		if (ret < 0) {
			return ret;
		}
		n = zsock_recv(fd, pos, len, ZSOCK_MSG_DONTWAIT);
		if (n < 0 && errno == EAGAIN) {
			continue;
		}
		if (n <= 0) {
			return n == 0 ? -ECONNRESET : -errno;
		}
		pos += n;
		len -= (size_t)n;
	}
	return 0;
}

/* One line to the client (SO_SNDTIMEO bounds each send). */
static int reply(int fd, const char *fmt, ...)
{
	char line[64];
	va_list ap;
	int len;
	size_t sent = 0;

	va_start(ap, fmt);
	len = vsnprintf(line, sizeof(line) - 1u, fmt, ap);
	va_end(ap);
	if (len < 0) {
		return -EINVAL;
	}
	if ((size_t)len > sizeof(line) - 2u) {
		len = (int)(sizeof(line) - 2u);
	}
	line[len++] = '\n';
	while (sent < (size_t)len) {
		ssize_t n = zsock_send(fd, line + sent, (size_t)len - sent, 0);

		if (n <= 0) {
			return -errno;
		}
		sent += (size_t)n;
	}
	return 0;
}

/* After an "ERR" during the image: drop what still comes, until the client closes or DRAIN_MS. */
static void drain(int fd, uint8_t *sink, size_t size)
{
	int64_t deadline = k_uptime_get() + DRAIN_MS;

	for (;;) {
		int64_t left = deadline - k_uptime_get();
		ssize_t n;

		if (left <= 0 || wait_readable(fd, (int)left) < 0) {
			return;
		}
		n = zsock_recv(fd, sink, size, ZSOCK_MSG_DONTWAIT);
		if (n == 0 || (n < 0 && errno != EAGAIN)) {
			return;
		}
	}
}

static const char *begin_reason(int err)
{
	switch (err) {
	case -EBUSY:
		return "busy";
	case -ENODEV:
		return "noslot";
	case -EPERM:
		return "pending";
	default:
		return "size";
	}
}

static const char *write_reason(int err)
{
	switch (err) {
	case -ENOEXEC:
		return "image";
	case -ENOTSUP:
		return "nohash";
	case -EXDEV:
		return "project";
	default:
		return "flash";
	}
}

/* The flash's erases would cost a running stream its packets (see the top). */
static void stop_audio(void)
{
	int ret = usbaudio_stream_stop();

	if (ret == 0) {
		LOG_INF("Stopped the USB audio stream for the update");
	} else if (ret != -ESRCH && ret != -ENODEV && ret != -EALREADY) {
		LOG_WRN("Could not stop the USB audio stream (%d)", ret);
	}
}

/* Command 1.  Whether the board is to restart. */
static bool cmd_write(int fd, uint32_t size, uint8_t flags)
{
	char slot[OTA_LABEL_SIZE];
	uint8_t *buf;
	uint32_t received = 0;
	unsigned int tenth = 1;
	int64_t start;
	const char *reason;
	int ret;

	ret = ota_update_begin(size, slot);
	if (ret < 0) {
		LOG_ERR("Update refused: %s", begin_reason(ret));
		(void)reply(fd, "ERR %s", begin_reason(ret));
		return false;
	}
	buf = malloc(CHUNK);
	if (buf == NULL) {
		ota_update_abort();
		LOG_ERR("Update refused: no memory");
		(void)reply(fd, "ERR memory");
		return false;
	}
	stop_audio();
	if (reply(fd, "OK %s", slot) < 0) {
		free(buf);
		ota_update_abort();
		LOG_ERR("%s: the client went before the image", slot);
		return false;
	}
	LOG_INF("%s: receiving an image of %u bytes", slot, (unsigned int)size);
	start = k_uptime_get();

	reason = NULL;
	while (received < size) {
		uint32_t n = MIN(CHUNK, size - received);

		ret = recv_exact(fd, buf, n);
		if (ret < 0) {
			reason = ret == -ETIMEDOUT ? "timeout" : NULL;
			break;
		}
		ret = ota_update_write(buf, n);
		if (ret < 0) {
			reason = write_reason(ret);
			break;
		}
		received += n;
		while (tenth < 10u && (uint64_t)received * 10u >= (uint64_t)size * tenth) {
			LOG_INF("%s: %u %% (%u bytes)", slot, tenth * 10u, (unsigned int)received);
			tenth++;
		}
	}

	if (received < size) {
		ota_update_abort();
		if (reason != NULL) {
			LOG_ERR("%s: update failed after %u of %u bytes: %s", slot, (unsigned int)received,
				(unsigned int)size, reason);
			if (reply(fd, "ERR %s", reason) == 0) {
				drain(fd, buf, CHUNK);
			}
		} else {
			LOG_ERR("%s: update failed after %u of %u bytes: the connection ended (%d)",
				slot, (unsigned int)received, (unsigned int)size, ret);
		}
		free(buf);
		return false;
	}
	free(buf);

	ret = ota_update_finish();
	if (ret < 0) {
		reason = ret == -EBADMSG ? "verify" : ret == -EIO ? "otadata" : "image";
		LOG_ERR("%s: update failed after all %u bytes: %s", slot, (unsigned int)size, reason);
		(void)reply(fd, "ERR %s", reason);
		return false;
	}
	LOG_INF("%s: written and checked in %u ms; it boots next, pending its confirmation", slot,
		(unsigned int)(k_uptime_get() - start));
	if (reply(fd, "OK") < 0) {
		LOG_WRN("%s: the client went before the result", slot);
	}
	return (flags & FLAG_RESTART) != 0u;
}

/* Command 2. */
static void cmd_confirm(int fd, const uint8_t *expected)
{
	struct ota_confirm c;
	int ret = ota_confirm(expected, &c);

	if (ret == -ENODEV) {
		(void)reply(fd, "ERR notota");
		return;
	}
	if (ret == -ESRCH) {
		LOG_WRN("%s: not confirmed: image %s runs, not the one expected", c.label, c.elf_sha256);
		(void)reply(fd, "ERR other %s %s %s", c.label, c.state, c.elf_sha256);
		return;
	}
	if (ret < 0) {
		LOG_ERR("%s: the confirmation failed: otadata was not written", c.label);
		(void)reply(fd, "ERR otadata");
		return;
	}
	if (c.changed) {
		LOG_INF("%s: image %s confirmed", c.label, c.elf_sha256);
	}
	(void)reply(fd, "OK %s %s %s", c.label, c.state, c.elf_sha256);
}

/* One connection.  Whether the board is to restart. */
static bool serve(int fd)
{
	uint8_t h[HEADER_SIZE];
	uint32_t size;

	set_timeout(fd, ZSOCK_SO_SNDTIMEO, SEND_TIMEOUT_MS);
	if (recv_exact(fd, h, sizeof(h)) < 0) {
		return false;
	}
	if (memcmp(h, "XOTA", 4) != 0) {
		LOG_WRN("Not an OTA header");
		(void)reply(fd, "ERR header");
		return false;
	}
	if (h[4] != HEADER_VERSION) {
		LOG_WRN("Header version %u", h[4]);
		(void)reply(fd, "ERR version");
		return false;
	}
	size = (uint32_t)h[8] | (uint32_t)h[9] << 8 | (uint32_t)h[10] << 16 | (uint32_t)h[11] << 24;
	switch (h[5]) {
	case CMD_WRITE:
		return cmd_write(fd, size, h[6]);
	case CMD_CONFIRM:
		cmd_confirm(fd, &h[8]);
		return false;
	default:
		LOG_WRN("Command %u", h[5]);
		(void)reply(fd, "ERR command");
		return false;
	}
}

static void service_main(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		struct zsock_pollfd pfd;
		bool restart;
		int fd;
		int ret;

		if (listen_fd < 0) {
			ret = setup_server();
			if (ret < 0) {
				k_msleep(SETUP_RETRY_MS);
				continue;
			}
			LOG_INF("Listening on TCP port %u", OTA_SERVICE_PORT);
		}
		pfd = (struct zsock_pollfd){.fd = listen_fd, .events = ZSOCK_POLLIN};
		ret = zsock_poll(&pfd, 1, ACCEPT_POLL_MS);
		if (ret < 0 || (pfd.revents & (ZSOCK_POLLERR | ZSOCK_POLLNVAL))) {
			close_server();
			k_msleep(SETUP_RETRY_MS);
			continue;
		}
		if (ret == 0) {
			continue;
		}
		fd = zsock_accept(listen_fd, NULL, NULL);
		if (fd < 0) {
			k_msleep(ACCEPT_POLL_MS / 10);
			continue;
		}
		restart = serve(fd);
		(void)zsock_close(fd);
		if (restart) {
			LOG_INF("Restarting into the new image");
			k_msleep(RESTART_DELAY_MS);
			board_restart();
		}
	}
}

int ota_service_init(void)
{
	static const struct z_thread_static service = {
		.name = "ota_service",
		.stack = service_stack,
		.stack_size = K_THREAD_STACK_SIZEOF(service_stack),
		.entry = service_main,
		.prio = APP_PRIO_OTA_SERVICE,
		.tid = &service_tid,
	};
	int ret = z_thread_static_start(&service);

	if (ret < 0) {
		LOG_ERR("Could not start the OTA service (%d)", ret);
	}
	return ret;
}
