/* SPDX-License-Identifier: MIT */
/*
 * ESP-IDF's Wi-Fi driver (port/net/wifi.h) as Zephyr's offloaded Wi-Fi
 * interface, for the Wi-Fi management layer and the "wifi" command
 * (shell/common/compat/zephyr_wifi_mgmt.c, zephyr_net_mgmt.c,
 * zephyr_wifi_shell.c): the counterpart of rp2040-connect's binding of its
 * Wi-Fi module (common/drivers/nina/port/zephyr/nina_net.c), and its
 * start-up (common/compat/net_offload/zephyr_net_init.c).
 *
 * The operations (struct wifi_mgmt_ops):
 *   scan          the driver's scan of all channels; each access point and
 *                 the end arrive through the callback, from the event
 *                 loop's task ("sys_evt"); the request's parameters are the
 *                 command's, all zero (anything else is refused, as
 *                 rp2040-connect's binding does)
 *   iface_status  the state: INTERFACE_DISABLED while the driver is not
 *                 running, SCANNING during a scan, DISCONNECTED otherwise
 *   connect       not yet: -ENOTSUP (the network stack is not built)
 *   disconnect    never connected: -EALREADY ("Already disconnected")
 */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/sys/util.h>

#include "net/wifi.h"
#include "zephyr_net_wifi.h"

/* The callback of the scan in progress, and its interface. */
static scan_result_cb_t scan_cb;
static struct net_if *scan_iface;

static enum wifi_security_type scan_security(enum net_wifi_security security)
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

static enum wifi_mfp_options scan_mfp(enum net_wifi_mfp mfp)
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

/* In the event loop's task. */
static void scan_result(const struct net_wifi_scan_entry *found, void *arg)
{
	struct wifi_scan_result result = {0};

	ARG_UNUSED(arg);
	memcpy(result.ssid, found->ssid, found->ssid_len);
	result.ssid_length = found->ssid_len;
	result.band = WIFI_FREQ_BAND_2_4_GHZ;
	result.channel = found->channel;
	result.security = scan_security(found->security);
	result.mfp = scan_mfp(found->mfp);
	result.rssi = found->rssi;
	memcpy(result.mac, found->bssid, WIFI_MAC_ADDR_LEN);
	result.mac_length = WIFI_MAC_ADDR_LEN;
	scan_cb(scan_iface, 0, &result);
}

static void scan_done(int status, void *arg)
{
	ARG_UNUSED(arg);
	scan_cb(scan_iface, status, NULL);
}

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
	/* A request the driver refuses (a scan running) leaves the running
	 * scan's callback as it was: Zephyr's management layer passes the
	 * same one with every request.
	 */
	if (net_wifi_state() == NET_WIFI_STATE_SCANNING) {
		return -EBUSY;
	}
	scan_cb = cb;
	scan_iface = iface;
	return net_wifi_scan(scan_result, scan_done, NULL);
}

static int wifi_connect(const struct device *dev, struct net_if *iface,
			struct wifi_connect_req_params *params)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(iface);
	ARG_UNUSED(params);
	return -ENOTSUP;
}

static int wifi_disconnect(const struct device *dev, struct net_if *iface)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(iface);
	return -EALREADY;
}

static int wifi_status(const struct device *dev, struct net_if *iface,
		       struct wifi_iface_status *status)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(iface);

	switch (net_wifi_state()) {
	case NET_WIFI_STATE_DOWN:
		status->state = WIFI_STATE_INTERFACE_DISABLED;
		break;
	case NET_WIFI_STATE_SCANNING:
		status->state = WIFI_STATE_SCANNING;
		break;
	default:
		status->state = WIFI_STATE_DISCONNECTED;
		break;
	}
	status->security = WIFI_SECURITY_TYPE_UNKNOWN;
	status->mfp = WIFI_MFP_UNKNOWN;
	status->link_mode = WIFI_LINK_MODE_UNKNOWN;
	status->band = WIFI_FREQ_BAND_UNKNOWN;
	status->iface_mode = WIFI_MODE_INFRA;
	return 0;
}

static void iface_init(struct net_if *iface)
{
	ARG_UNUSED(iface);
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

static struct net_if wifi_iface;

int zephyr_net_initialize(void)
{
	const struct net_wifi_mgmt_offload *iface_api = z_net_offload_api;
	int ret;

	/* Zephyr's net_init(): the event thread, then the interface. */
	ret = net_mgmt_event_init();
	if (ret < 0) {
		return ret;
	}

	wifi_iface.api = iface_api;
	z_net_if_register(&wifi_iface);

	ret = z_net_offload_init(NULL);
	if (ret < 0) {
		return ret;
	}

	iface_api->wifi_iface.iface_api.init(&wifi_iface);

	/* SYS_INIT(APPLICATION) of the "wifi" command. */
	return wifi_shell_init();
}
