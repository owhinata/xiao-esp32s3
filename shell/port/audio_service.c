/* SPDX-License-Identifier: MIT */
/*
 * The audio stream service (audio_service.h): the thread "audio_service"
 * listens on TCP port AUDIO_SERVICE_PORT and sends the 16 kHz samples of
 * port/usb/audio.h (usbaudio_read_16k(), with a reader of its own) to one
 * client at a time, on the shell's sockets (zsock_*(), as the OTA service
 * uses them).  The stream itself is started and stopped with "uac".
 *
 * The board sends, integers little-endian, a stream header of 16 bytes
 * once:
 *
 *   0  "XAUD"
 *   4  protocol version, 1
 *   5  sample format, 1: signed 16 bits, one channel
 *   6  reserved, 0 (2 bytes)
 *   8  sample rate, 16000
 *  12  reserved, 0 (4 bytes)
 *
 * then blocks until the connection ends, each an 8-byte header and its
 * samples:
 *
 *   0  index of the block's first sample: the samples the reader has
 *      passed since the connection, read and lost (wraps at 2^32)
 *   4  count of the samples that follow, up to BLOCK_SAMPLES
 *   6  flags: FLAG_IDLE, no stream with 16 kHz samples runs (count 0);
 *      FLAG_NEW_STREAM, the first block of a stream that started during
 *      the connection
 *   8  the samples
 *
 * While a stream runs, each read asks for BLOCK_SAMPLES (20 ms) and waits
 * up to READ_WAIT_MS; a block goes out with what came.  A stream that gives
 * nothing for KEEPALIVE_MS gets a block with count 0 and no flag.  While no
 * stream runs (or one at a rate the 16 kHz ring does not serve), a block
 * with count 0 and FLAG_IDLE goes out at once and then every KEEPALIVE_MS,
 * and the service waits on the sockets for up to IDLE_POLL_MS at a time,
 * short enough that a stream that starts meanwhile is read from its first
 * sample (the ring holds USBAUDIO_RING_SAMPLES, 256 ms).
 *
 * Between the reads the client's socket and the listening socket are
 * polled: what the client sends is read and dropped, its close (or an
 * error) ends the connection, and a second client is accepted and closed
 * at once.  Each send waits up to SEND_TIMEOUT_MS (SO_SNDTIMEO); a client
 * that takes nothing for that long is dropped.  TCP_NODELAY sends each
 * block when it is written.
 *
 * The block is one static buffer, header and samples together, filled by
 * the read in place.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>

#include "app_log.h"
#include "audio_service.h"
#include "priorities.h"
#include "usb/audio.h"

LOG_MODULE_REGISTER(audio_service, LOG_LEVEL_INF);

#define STREAM_HEADER_SIZE  16u
#define PROTOCOL_VERSION    1u
#define FORMAT_S16LE_MONO   1u
#define SAMPLE_RATE         16000u

#define BLOCK_HEADER_SIZE   8u
#define BLOCK_SAMPLES       320u
#define FLAG_IDLE           0x0001u
#define FLAG_NEW_STREAM     0x0002u

#define READ_WAIT_MS        40u
#define KEEPALIVE_MS        1000
#define IDLE_POLL_MS        100
#define SEND_TIMEOUT_MS     3000
#define SETUP_RETRY_MS      1000
#define ACCEPT_POLL_MS      1000

BUILD_ASSERT(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "the samples go out as the CPU stores them");
BUILD_ASSERT(IDLE_POLL_MS * SAMPLE_RATE / 1000u < USBAUDIO_RING_SAMPLES,
	     "a stream that starts while the service waits is read from its first sample");

static K_THREAD_STACK_DEFINE(service_stack, APP_STACK_AUDIO_SERVICE);
static k_tid_t service_tid;
static int listen_fd = -1;

static struct {
	uint8_t header[BLOCK_HEADER_SIZE];
	int16_t samples[BLOCK_SAMPLES];
} block;

static volatile struct audio_service_stats stats;

void audio_service_stats(struct audio_service_stats *out)
{
	out->listening = stats.listening;
	out->connected = stats.connected;
	out->blocks = stats.blocks;
	out->samples = stats.samples;
	out->lost = stats.lost;
	out->dropped = stats.dropped;
}

static void put_le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
	put_le16(p, (uint16_t)v);
	put_le16(p + 2, (uint16_t)(v >> 16));
}

static int setup_server(void)
{
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(AUDIO_SERVICE_PORT),
		.sin_addr = {.s_addr = htonl(INADDR_ANY)},
	};
	int on = 1;
	int fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	if (fd < 0) {
		return -errno;
	}
	/* As the OTA service: the connections closed here wait in TIME_WAIT on the port. */
	(void)zsock_setsockopt(fd, ZSOCK_SOL_SOCKET, ZSOCK_SO_REUSEADDR, &on, sizeof(on));
	if (zsock_bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || zsock_listen(fd, 1) < 0) {
		int ret = -errno;

		(void)zsock_close(fd);
		return ret;
	}
	listen_fd = fd;
	stats.listening = true;
	return 0;
}

static void close_server(void)
{
	if (listen_fd >= 0) {
		(void)zsock_close(listen_fd);
		listen_fd = -1;
		stats.listening = false;
	}
}

/* A client that came while another is served. */
static void refuse_client(void)
{
	int fd = zsock_accept(listen_fd, NULL, NULL);

	if (fd < 0) {
		/* No descriptor or socket free: the connection waits; do not poll it in a loop. */
		k_msleep(IDLE_POLL_MS);
		return;
	}
	(void)zsock_close(fd);
	LOG_WRN("Refused a second client");
}

/* All of len bytes, each send bounded by SO_SNDTIMEO.  0 or a negative errno. */
static int send_all(int fd, const void *data, size_t len)
{
	const uint8_t *pos = data;

	while (len > 0u) {
		ssize_t n = zsock_send(fd, pos, len, 0);

		if (n <= 0) {
			return n == 0 ? -EIO : -errno;
		}
		pos += n;
		len -= (size_t)n;
	}
	return 0;
}

/*
 * Waits up to timeout_ms (0: does not wait) for the client or a second
 * one, refusing the second and dropping what the client sent.  false when
 * the client's connection has ended.
 */
static bool watch_sockets(int fd, int timeout_ms)
{
	struct zsock_pollfd fds[2] = {
		{.fd = fd, .events = ZSOCK_POLLIN},
		{.fd = listen_fd, .events = ZSOCK_POLLIN},
	};
	int ret = zsock_poll(fds, listen_fd >= 0 ? 2 : 1, timeout_ms);

	if (ret < 0) {
		return false;
	}
	if (ret == 0) {
		return true;
	}
	if (listen_fd >= 0 && (fds[1].revents & (ZSOCK_POLLERR | ZSOCK_POLLNVAL))) {
		/* Set up again by the service once this client is done. */
		close_server();
	} else if (fds[1].revents & ZSOCK_POLLIN) {
		refuse_client();
	}
	if (fds[0].revents & (ZSOCK_POLLERR | ZSOCK_POLLHUP | ZSOCK_POLLNVAL)) {
		return false;
	}
	if (fds[0].revents & ZSOCK_POLLIN) {
		uint8_t sink[64];
		ssize_t n = zsock_recv(fd, sink, sizeof(sink), ZSOCK_MSG_DONTWAIT);

		if (n == 0 || (n < 0 && errno != EAGAIN)) {
			return false;
		}
	}
	return true;
}

static int send_block(int fd, uint32_t index, uint16_t count, uint16_t flags)
{
	int ret;

	put_le32(&block.header[0], index);
	put_le16(&block.header[4], count);
	put_le16(&block.header[6], flags);
	ret = send_all(fd, &block, BLOCK_HEADER_SIZE + (size_t)count * sizeof(block.samples[0]));
	if (ret == 0) {
		stats.blocks++;
		stats.samples += count;
	}
	return ret;
}

/* One connection, until the client closes it or a send fails. */
static void serve(int fd)
{
	uint8_t header[STREAM_HEADER_SIZE] = {'X', 'A', 'U', 'D', PROTOCOL_VERSION, FORMAT_S16LE_MONO};
	struct zsock_timeval tv = {.tv_sec = SEND_TIMEOUT_MS / 1000,
				   .tv_usec = (SEND_TIMEOUT_MS % 1000) * 1000};
	struct usbaudio_reader reader;
	uint32_t read_total = 0;      /* samples read, without the lost ones */
	uint32_t stream;              /* of the last block with samples */
	int64_t last_send;
	bool idle = false;
	int on = 1;
	int ret;

	(void)zsock_setsockopt(fd, ZSOCK_SOL_SOCKET, ZSOCK_SO_SNDTIMEO, &tv, sizeof(tv));
	(void)zsock_setsockopt(fd, IPPROTO_TCP, ZSOCK_TCP_NODELAY, &on, sizeof(on));
	put_le32(&header[8], SAMPLE_RATE);

	stats.blocks = 0;
	stats.samples = 0;
	stats.lost = 0;
	stats.connected = true;
	usbaudio_reader_init(&reader);
	stream = reader.stream;
	ret = send_all(fd, header, sizeof(header));
	last_send = k_uptime_get();

	while (ret == 0) {
		int64_t now;
		int n;

		if (!watch_sockets(fd, 0)) {
			break;
		}
		n = usbaudio_read_16k(&reader, block.samples, BLOCK_SAMPLES, READ_WAIT_MS);
		stats.lost = reader.lost;
		now = k_uptime_get();
		if (n > 0) {
			uint16_t flags = reader.stream != stream ? FLAG_NEW_STREAM : 0u;

			stream = reader.stream;
			idle = false;
			/* The loss of a read comes before its samples (it skips to the oldest kept). */
			ret = send_block(fd, read_total + reader.lost, (uint16_t)n, flags);
			read_total += (uint32_t)n;
			last_send = now;
		} else if (n == 0) {
			idle = false;
			if (now - last_send >= KEEPALIVE_MS) {
				ret = send_block(fd, read_total + reader.lost, 0, 0);
				last_send = now;
			}
		} else {
			/* -ENODATA, -ENOTSUP: the read returns at once. */
			if (!idle || now - last_send >= KEEPALIVE_MS) {
				ret = send_block(fd, read_total + reader.lost, 0, FLAG_IDLE);
				last_send = now;
				idle = true;
			}
			if (ret == 0 && !watch_sockets(fd, IDLE_POLL_MS)) {
				break;
			}
		}
	}
	if (ret != 0) {
		stats.dropped++;
		LOG_WRN("Dropped the client: a send failed (%d)", ret);
	}
	stats.connected = false;
}

static void service_main(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		struct zsock_pollfd pfd;
		int fd;
		int ret;

		if (listen_fd < 0) {
			ret = setup_server();
			if (ret < 0) {
				k_msleep(SETUP_RETRY_MS);
				continue;
			}
			LOG_INF("Listening on TCP port %u", AUDIO_SERVICE_PORT);
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
		serve(fd);
		(void)zsock_close(fd);
	}
}

int audio_service_init(void)
{
	static const struct z_thread_static service = {
		.name = "audio_service",
		.stack = service_stack,
		.stack_size = K_THREAD_STACK_SIZEOF(service_stack),
		.entry = service_main,
		.prio = APP_PRIO_AUDIO_SERVICE,
		.tid = &service_tid,
	};
	int ret = z_thread_static_start(&service);

	if (ret < 0) {
		LOG_ERR("Could not start the audio stream service (%d)", ret);
	}
	return ret;
}
