/* SPDX-License-Identifier: MIT */
/*
 * ESP-IDF's Wi-Fi driver (port/net/wifi.h) as Zephyr's offloaded Wi-Fi
 * interface, for the Wi-Fi management layer, the "wifi" and "ifconfig"
 * commands (shell/common/compat/zephyr_wifi_mgmt.c, zephyr_net_mgmt.c,
 * zephyr_wifi_shell.c, shell/common/shell/ifconfig_shell.c, on the
 * interface's state in shell/common/compat/net_offload/zephyr_net_if.c):
 * the counterpart of rp2040-connect's binding of its Wi-Fi module
 * (common/drivers/nina/port/zephyr/nina_net.c), and its start-up
 * (common/compat/net_offload/zephyr_net_init.c).  The interface's init
 * registers lwIP's name resolution as the sockets' resolver
 * (zephyr_net_socket.c), as nina_net.c registers the NINA's.
 *
 * The operations (struct wifi_mgmt_ops):
 *   scan          the driver's scan of all channels; the request's
 *                 parameters are the command's, all zero (anything else is
 *                 refused, as rp2040-connect's binding does)
 *   connect       open (-k 0) or WPA2-PSK and better (-k 1) with the
 *                 passphrase (net_wifi_connect())
 *   disconnect    net_wifi_disconnect(); -EALREADY when there is nothing
 *                 to end ("Already disconnected")
 *   iface_status  the state, and the access point once associated
 *
 * What happens on the connection (struct net_wifi_ops) goes into the
 * interface (address, netmask, gateway, dormant) at once, and the layer's
 * events (scan results and done, connect and disconnect results) are
 * raised by this file's thread "wifi_evt", one at a time, at the priority
 * of rp2040-connect's NINA Wi-Fi thread, which raises them there: below
 * the network event thread "net_mgmt", which so takes each event as soon
 * as it is raised.  The layer's queue (5 events, a full one makes the
 * event wait 10 ms and then drops it) never holds more than one of them:
 * the thread raises the next only after "net_mgmt" has taken the one
 * before (this file's own event callback says so), however long the
 * output of the one before takes.  A scan's records are taken from the
 * driver in that thread too, one by one.
 */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/util.h>

#include "priorities.h"
#include "net/wifi.h"
#include "zephyr_net_socket.h"
#include "zephyr_net_wifi.h"

LOG_MODULE_REGISTER(net_wifi_if);

/* The interface, set at the start (zephyr_net_initialize()). */
static struct net_if wifi_iface;

/* The scan in progress: the layer's callback. */
static scan_result_cb_t scan_cb;

/* The layer's events ------------------------------------------------------ */

#define WIFI_EVENTS (NET_EVENT_WIFI_SCAN_RESULT | NET_EVENT_WIFI_SCAN_DONE | \
		     NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT)

/* After this long without "net_mgmt" taking the event raised, it is logged (once per event). */
#define TAKEN_WARN_MS 2000

/* The work for the thread, in order: a scan's end, a connect or disconnect result. */
enum work_kind {
	WORK_SCAN_DONE,
	WORK_CONNECT_RESULT,
	WORK_DISCONNECT_RESULT,
};

struct work {
	enum work_kind kind;
	int status;
	unsigned int count;
};

/* More than the driver's side can have outstanding (one scan, a few results). */
#define WORK_SLOTS 8

static struct k_spinlock work_lock;
static struct work works[WORK_SLOTS];
static unsigned int work_head;
static unsigned int work_count;
static K_SEM_DEFINE(work_ready, 0, WORK_SLOTS);

/* The event raised and not yet taken by "net_mgmt" (0: none), and its taking. */
static uint64_t in_flight;
static K_SEM_DEFINE(taken, 1, 1);

static struct net_mgmt_event_callback taken_cb;

/* From the driver's side (must not block): the work goes to the thread. */
static void work_put(enum work_kind kind, int status, unsigned int count)
{
	k_spinlock_key_t key = k_spin_lock(&work_lock);
	bool full = work_count == WORK_SLOTS;

	if (!full) {
		works[(work_head + work_count) % WORK_SLOTS] =
			(struct work){.kind = kind, .status = status, .count = count};
		work_count++;
	}
	k_spin_unlock(&work_lock, key);
	if (full) {
		LOG_ERR("event %d (%d) dropped: %d waiting", (int)kind, status, WORK_SLOTS);
		return;
	}
	k_sem_give(&work_ready);
}

static struct work work_get(void)
{
	k_spinlock_key_t key;
	struct work w;

	(void)k_sem_take(&work_ready, K_FOREVER);
	key = k_spin_lock(&work_lock);
	w = works[work_head];
	work_head = (work_head + 1) % WORK_SLOTS;
	work_count--;
	k_spin_unlock(&work_lock, key);
	return w;
}

/* In "net_mgmt", for every Wi-Fi event: the one raised is taken. */
static void event_taken(struct net_mgmt_event_callback *cb, uint64_t mgmt_event,
			struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);
	if (mgmt_event != in_flight) {
		LOG_ERR("net_mgmt took event 0x%x, 0x%x was raised: one was dropped",
			(unsigned int)mgmt_event, (unsigned int)in_flight);
	}
	in_flight = 0;
	k_sem_give(&taken);
}

/* In "wifi_evt": waits until the event before is taken, then event is the one in flight. */
static void wait_taken(uint64_t event)
{
	if (k_sem_take(&taken, K_MSEC(TAKEN_WARN_MS)) != 0) {
		LOG_WRN("net_mgmt has not taken event 0x%x for %d ms", (unsigned int)in_flight,
			TAKEN_WARN_MS);
		(void)k_sem_take(&taken, K_FOREVER);
	}
	in_flight = event;
}

static enum wifi_security_type security_of(enum net_wifi_security security)
{
	switch (security) {
	case NET_WIFI_SECURITY_OPEN:
		return WIFI_SECURITY_TYPE_NONE;
	case NET_WIFI_SECURITY_WEP:
		return WIFI_SECURITY_TYPE_WEP;
	case NET_WIFI_SECURITY_WPA_PSK:
		return WIFI_SECURITY_TYPE_WPA_PSK;
	case NET_WIFI_SECURITY_WPA2_PSK:
		return WIFI_SECURITY_TYPE_PSK;
	case NET_WIFI_SECURITY_WPA_WPA2_PSK:
	case NET_WIFI_SECURITY_WPA2_WPA3:
		return WIFI_SECURITY_TYPE_WPA_AUTO_PERSONAL;
	case NET_WIFI_SECURITY_WPA3_SAE:
		return WIFI_SECURITY_TYPE_SAE;
	case NET_WIFI_SECURITY_WAPI:
		return WIFI_SECURITY_TYPE_WAPI;
	case NET_WIFI_SECURITY_OWE:
		return WIFI_SECURITY_TYPE_OWE;
	case NET_WIFI_SECURITY_ENTERPRISE:
		return WIFI_SECURITY_TYPE_EAP;
	case NET_WIFI_SECURITY_DPP:
		return WIFI_SECURITY_TYPE_DPP;
	default:
		return WIFI_SECURITY_TYPE_UNKNOWN;
	}
}

static enum wifi_mfp_options mfp_of(enum net_wifi_mfp mfp)
{
	switch (mfp) {
	case NET_WIFI_MFP_DISABLED:
		return WIFI_MFP_DISABLE;
	case NET_WIFI_MFP_OPTIONAL:
		return WIFI_MFP_OPTIONAL;
	case NET_WIFI_MFP_REQUIRED:
		return WIFI_MFP_REQUIRED;
	default:
		return WIFI_MFP_UNKNOWN;
	}
}

/* In "wifi_evt": the records of the scan that ended, then its end. */
static void scan_deliver(int status)
{
	struct net_wifi_scan_entry found;
	struct wifi_scan_result result;

	while (status == 0 && net_wifi_scan_next(&found) == 0) {
		memset(&result, 0, sizeof(result));
		memcpy(result.ssid, found.ssid, found.ssid_len);
		result.ssid_length = found.ssid_len;
		result.band = WIFI_FREQ_BAND_2_4_GHZ;
		result.channel = found.channel;
		result.security = security_of(found.security);
		result.mfp = mfp_of(found.mfp);
		result.rssi = found.rssi;
		memcpy(result.mac, found.bssid, WIFI_MAC_ADDR_LEN);
		result.mac_length = WIFI_MAC_ADDR_LEN;
		wait_taken(NET_EVENT_WIFI_SCAN_RESULT);
		scan_cb(&wifi_iface, 0, &result);
	}
	net_wifi_scan_end();
	wait_taken(NET_EVENT_WIFI_SCAN_DONE);
	scan_cb(&wifi_iface, status, NULL);
}

static K_THREAD_STACK_DEFINE(wifi_evt_stack, APP_STACK_WIFI_EVENTS);
static k_tid_t wifi_evt_tid;

static void wifi_evt_thread(void *p1, void *p2, void *p3)
{
	uint8_t mac[NET_WIFI_MAC_LEN];

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	/* The link address, from eFuse (in a thread: its API takes a lock). */
	if (net_wifi_mac(mac) == 0) {
		net_if_set_link_addr(&wifi_iface, mac, sizeof(mac), NET_LINK_ETHERNET);
	}

	for (;;) {
		struct work w = work_get();

		switch (w.kind) {
		case WORK_SCAN_DONE:
			scan_deliver(w.status);
			break;
		case WORK_CONNECT_RESULT:
			wait_taken(NET_EVENT_WIFI_CONNECT_RESULT);
			wifi_mgmt_raise_connect_result_event(&wifi_iface, w.status);
			break;
		case WORK_DISCONNECT_RESULT:
			wait_taken(NET_EVENT_WIFI_DISCONNECT_RESULT);
			wifi_mgmt_raise_disconnect_result_event(&wifi_iface, w.status);
			break;
		}
	}
}

/* The driver's side (struct net_wifi_ops; in the event loop's task) ------- */

static void ip_set(const uint8_t *addr, const uint8_t *netmask, const uint8_t *gateway)
{
	struct net_in_addr address, mask, gw;

	memcpy(&address, addr, 4);
	memcpy(&mask, netmask, 4);
	memcpy(&gw, gateway, 4);
	if (net_if_ipv4_addr_add(&wifi_iface, &address, NET_ADDR_DHCP, 0)) {
		net_if_ipv4_set_netmask_by_addr(&wifi_iface, &address, &mask);
		net_if_ipv4_set_gw(&wifi_iface, &gw);
	}
}

static void ip_clear(const uint8_t *addr)
{
	const struct net_in_addr no_gateway = {0};
	struct net_in_addr address;

	memcpy(&address, addr, 4);
	net_if_ipv4_addr_rm(&wifi_iface, &address);
	net_if_ipv4_set_gw(&wifi_iface, &no_gateway);
}

static void link_up(void)
{
	net_if_dormant_off(&wifi_iface);
}

static void link_down(void)
{
	net_if_dormant_on(&wifi_iface);
}

static void connect_result(int status)
{
	work_put(WORK_CONNECT_RESULT, status, 0);
}

static void disconnected(int reason)
{
	work_put(WORK_DISCONNECT_RESULT, reason, 0);
}

static const struct net_wifi_ops driver_ops = {
	.ip_set = ip_set,
	.ip_clear = ip_clear,
	.link_up = link_up,
	.link_down = link_down,
	.connect_result = connect_result,
	.disconnected = disconnected,
};

/* In the event loop's task. */
static void scan_done(int status, unsigned int count, void *arg)
{
	ARG_UNUSED(arg);
	work_put(WORK_SCAN_DONE, status, count);
}

/* Zephyr's Wi-Fi management ---------------------------------------------- */

static int wifi_scan(const struct device *dev, struct net_if *iface,
		     struct wifi_scan_params *params, scan_result_cb_t cb)
{
	ARG_UNUSED(dev);
	if (!iface || !cb) {
		return -EINVAL;
	}
	if (params) {
		if (params->scan_type != WIFI_SCAN_TYPE_ACTIVE ||
		    params->bands || params->dwell_time_active ||
		    params->dwell_time_passive || params->max_bss_cnt) {
			return -ENOTSUP;
		}
		for (size_t i = 0; i < ARRAY_SIZE(params->ssids); i++) {
			if (params->ssids[i]) {
				return -ENOTSUP;
			}
		}
		for (size_t i = 0; i < ARRAY_SIZE(params->band_chan); i++) {
			if (params->band_chan[i].channel) {
				return -ENOTSUP;
			}
		}
	}
	/* Zephyr's management layer passes the same callback with every
	 * request, so a request the driver refuses (a scan running) leaves
	 * the running scan's as it was.
	 */
	scan_cb = cb;
	return net_wifi_scan(scan_done, NULL);
}

static int wifi_connect(const struct device *dev, struct net_if *iface,
			struct wifi_connect_req_params *params)
{
	struct net_wifi_connect_params p;

	ARG_UNUSED(dev);
	ARG_UNUSED(iface);

	if (!params) {
		return -EINVAL;
	}
	/* The types the copied "wifi connect" accepts (-k 0 and 1), as the NINA's binding. */
	if (params->security != WIFI_SECURITY_TYPE_NONE &&
	    params->security != WIFI_SECURITY_TYPE_PSK) {
		return -ENOTSUP;
	}
	if (params->psk_is_pbkdf2) {
		return -EINVAL;
	}
	p = (struct net_wifi_connect_params){
		.ssid = params->ssid,
		.ssid_len = params->ssid_length,
		.passphrase = params->security == WIFI_SECURITY_TYPE_PSK ? params->psk : NULL,
		.passphrase_len = params->security == WIFI_SECURITY_TYPE_PSK ? params->psk_length : 0,
	};
	return net_wifi_connect(&p);
}

static int wifi_disconnect(const struct device *dev, struct net_if *iface)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(iface);
	return net_wifi_disconnect();
}

static enum wifi_link_mode link_mode_of(enum net_wifi_phy phy)
{
	switch (phy) {
	case NET_WIFI_PHY_11B:
		return WIFI_1;
	case NET_WIFI_PHY_11G:
		return WIFI_3;
	case NET_WIFI_PHY_11N:
		return WIFI_4;
	case NET_WIFI_PHY_11AX:
		return WIFI_6;
	default:
		return WIFI_LINK_MODE_UNKNOWN;
	}
}

static int wifi_status(const struct device *dev, struct net_if *iface,
		       struct wifi_iface_status *status)
{
	struct net_wifi_status s;

	ARG_UNUSED(dev);
	ARG_UNUSED(iface);

	(void)net_wifi_status(&s);
	switch (s.state) {
	case NET_WIFI_STATE_DOWN:
		status->state = WIFI_STATE_INTERFACE_DISABLED;
		break;
	case NET_WIFI_STATE_SCANNING:
		status->state = WIFI_STATE_SCANNING;
		break;
	case NET_WIFI_STATE_ASSOCIATING:
		status->state = WIFI_STATE_ASSOCIATING;
		break;
	case NET_WIFI_STATE_ASSOCIATED:
		status->state = WIFI_STATE_ASSOCIATED;
		break;
	case NET_WIFI_STATE_CONNECTED:
		status->state = WIFI_STATE_COMPLETED;
		break;
	default:
		status->state = WIFI_STATE_DISCONNECTED;
		break;
	}
	status->iface_mode = WIFI_MODE_INFRA;
	status->security = WIFI_SECURITY_TYPE_UNKNOWN;
	status->mfp = WIFI_MFP_UNKNOWN;
	status->link_mode = WIFI_LINK_MODE_UNKNOWN;
	status->band = WIFI_FREQ_BAND_UNKNOWN;
	if (s.state != NET_WIFI_STATE_ASSOCIATED && s.state != NET_WIFI_STATE_CONNECTED) {
		return 0;
	}
	memcpy(status->ssid, s.ssid, s.ssid_len + 1u);
	status->ssid_len = s.ssid_len;
	memcpy(status->bssid, s.bssid, WIFI_MAC_ADDR_LEN);
	status->band = WIFI_FREQ_BAND_2_4_GHZ;
	status->channel = s.channel;
	status->rssi = s.rssi;
	status->security = security_of(s.security);
	status->mfp = mfp_of(s.mfp);
	status->link_mode = link_mode_of(s.phy);
	return 0;
}

static void iface_init(struct net_if *iface)
{
	net_if_dormant_on(iface);
	net_if_set_mtu(iface, 1500);
	zephyr_net_socket_dns_register();
}

static int driver_init(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

static enum offloaded_net_if_types get_type(void)
{
	return L2_OFFLOADED_NET_IF_TYPE_WIFI;
}

static const struct wifi_mgmt_ops mgmt = {.scan = wifi_scan,
					  .connect = wifi_connect,
					  .disconnect = wifi_disconnect,
					  .iface_status = wifi_status};
static const struct net_wifi_mgmt_offload api = {
	.wifi_iface.iface_api.init = iface_init,
	.wifi_iface.get_type = get_type,
	.wifi_mgmt_api = &mgmt};
NET_DEVICE_DT_OFFLOAD_DEFINE(0, driver_init, NULL, NULL, NULL, 0, &api, 1500);

/* Start-up --------------------------------------------------------------- */

int wifi_shell_init(void);

int zephyr_net_initialize(void)
{
	static const struct z_thread_static thread = {
		.name = "wifi_evt",
		.stack = wifi_evt_stack,
		.stack_size = K_THREAD_STACK_SIZEOF(wifi_evt_stack),
		.entry = wifi_evt_thread,
		.prio = APP_PRIO_NINA_WIFI,
		.tid = &wifi_evt_tid,
	};
	const struct net_wifi_mgmt_offload *iface_api = z_net_offload_api;
	int ret;

	/* Zephyr's net_init(): the event thread, then the interface. */
	ret = net_mgmt_event_init();
	if (ret < 0) {
		return ret;
	}
	net_mgmt_init_event_callback(&taken_cb, event_taken, WIFI_EVENTS);
	net_mgmt_add_event_callback(&taken_cb);

	wifi_iface.api = iface_api;
	z_net_if_register(&wifi_iface);

	ret = z_net_offload_init(NULL);
	if (ret < 0) {
		return ret;
	}

	iface_api->wifi_iface.iface_api.init(&wifi_iface);
	net_wifi_set_ops(&driver_ops);

	ret = z_thread_static_start(&thread);
	if (ret < 0) {
		return ret;
	}

	/* SYS_INIT(APPLICATION) of the "wifi" command. */
	return wifi_shell_init();
}
