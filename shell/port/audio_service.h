/* SPDX-License-Identifier: MIT */
/*
 * The audio stream service (shell/port/audio_service.c): the 16 kHz
 * samples of the USB audio input (port/usb/audio.h) over TCP to one
 * client, script/audio_rx.py or the host application.
 */
#ifndef SHELL_PORT_AUDIO_SERVICE_H
#define SHELL_PORT_AUDIO_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#define AUDIO_SERVICE_PORT 3233

/*
 * Starts the thread "audio_service", which listens on AUDIO_SERVICE_PORT
 * (any address) from then on, from a thread once lwIP runs (after
 * shell_telnet_init()).  0, or a negative errno when the thread could not
 * be started (logged).  Once only.
 */
int audio_service_init(void);

/*
 * The service's counters.  blocks, samples and lost are those of the
 * client connected, or of the last one; dropped counts the clients that
 * were closed because a send failed or timed out, since the start.  Each
 * field is read on its own while the service updates them.
 */
struct audio_service_stats {
	bool listening;
	bool connected;
	uint32_t blocks;            /* blocks sent, keep-alive blocks included */
	uint32_t samples;           /* samples sent */
	uint32_t lost;              /* samples the reader skipped (it fell behind the ring) */
	uint32_t dropped;
};

void audio_service_stats(struct audio_service_stats *stats);

#endif
