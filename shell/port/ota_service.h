/* SPDX-License-Identifier: MIT */
/*
 * The OTA service (shell/port/ota_service.c): firmware updates over TCP
 * from script/ota.py, into the OTA slot that is not running
 * (port/ota_slots.h).
 */
#ifndef SHELL_PORT_OTA_SERVICE_H
#define SHELL_PORT_OTA_SERVICE_H

#include <stdbool.h>

#define OTA_SERVICE_PORT 3232

/*
 * Starts the thread "ota_service", which listens on OTA_SERVICE_PORT (any
 * address) from then on, from a thread once lwIP runs (after
 * shell_telnet_init()).  0, or a negative errno when the thread could not
 * be started (logged).  Once only.
 */
int ota_service_init(void);

/* Whether the service's socket listens. */
bool ota_service_listening(void);

#endif
