/* SPDX-License-Identifier: MIT */
/*
 * The part of rp2040-connect's NINA driver interface
 * (common/drivers/nina/include/nina_wifi.h) that the copy of its Wi-Fi
 * profile (shell/common/imu/wifi_profile.c) uses: the hooks of the
 * automatic connection.  This board has no NINA module.  The hooks go to
 * this board's Wi-Fi driver (port/net/wifi.c, struct
 * net_wifi_reconnect_ops) through shell/port/zephyr_net_wifi.c, which
 * defines nina_wifi_set_reconnect_ops().  The header has the NINA name, as
 * shell/port/fault_record.h has the name of rp2040-connect's header, so
 * that the copy compiles unchanged (shell/port is first on the include
 * path).
 *
 * The struct is the NINA header's.  The driver calls the hooks at these
 * points (port/net/wifi.h says more):
 *   connection_allowed()  net_wifi_connect(), in the caller's thread, under
 *                         the driver's request lock; false refuses the
 *                         connection (-ECANCELED)
 *   pause()               net_wifi_disconnect(), in the caller's thread,
 *                         under the same lock
 *   request()             the driver has given up a connection that was
 *                         lost without a request: the address hold is over
 *                         (port/net/wlanif.h); in the event loop's task
 * Any of them may be NULL.
 */
#ifndef SHELL_PORT_NINA_WIFI_H
#define SHELL_PORT_NINA_WIFI_H

#include <stdbool.h>

struct nina_wifi_reconnect_ops {
	bool (*connection_allowed)(void);
	void (*pause)(void);
	void (*request)(void);
};

/*
 * Keeps OPS (NULL: none) from now on.  From a thread, at any time: the
 * driver's side reads the registration atomically, so OPS and its
 * functions must stay valid for good (static).
 */
void nina_wifi_set_reconnect_ops(const struct nina_wifi_reconnect_ops *ops);

#endif /* SHELL_PORT_NINA_WIFI_H */
