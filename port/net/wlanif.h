/*
 * The Wi-Fi station's lwIP network interface (port/net/wlanif.c), for the
 * driver's side (port/net/wifi.c).  This header does not include lwIP's:
 * its users see only these functions.
 *
 * The interface reports what happens to its IPv4 address as ESP-IDF's
 * esp_netif does, with the events of the default event loop:
 * IP_EVENT_STA_GOT_IP (ip_event_got_ip_t: the address, netmask and gateway
 * DHCP gave, after every connection and whenever they change) and
 * IP_EVENT_STA_LOST_IP (the address is gone), posted from the tcpip
 * thread.  The functions below hand their work to the tcpip thread and
 * return; it runs it in the order the calls were made.
 */
#ifndef PORT_NET_WLANIF_H
#define PORT_NET_WLANIF_H

#include <stdbool.h>

/*
 * Milliseconds the address is kept after the station loses its access
 * point (wlanif_link_down()): esp_netif's CONFIG_ESP_NETIF_IP_LOST_TIMER_
 * INTERVAL, 120 s.
 */
#define WLANIF_ADDRESS_HOLD_MS  120000u

/*
 * Starts lwIP (the TCP initial sequence numbers' secret from the random
 * number generator, then the tcpip thread) and adds the station's
 * interface, up, with its link down, no address, the driver's MAC
 * address, as the default route.  Once, from a thread, after
 * esp_wifi_start() (the radio is on, so esp_fill_random() is random).
 * 0, or -1 when lwIP could not start (logged).
 */
int wlanif_start(void);

/* wlanif_start() has succeeded: lwIP's API may be called.  From any thread. */
bool wlanif_started(void);

/*
 * The station is associated (WIFI_EVENT_STA_CONNECTED): received frames go
 * to lwIP from now on, the link is up, and DHCP starts, or asks again for
 * the address the interface still has (DHCP's reboot).
 */
void wlanif_link_up(void);

/*
 * The station has lost its access point: the link is down; the address,
 * if there is one, stays for WLANIF_ADDRESS_HOLD_MS from the first loss,
 * so that TCP connections outlive a short one, and is released after that
 * unless DHCP has confirmed it on a new link (wlanif_link_up()) first.
 */
void wlanif_link_down(void);

/* The station leaves on request: DHCP releases the address and stops, the link is down. */
void wlanif_stop(void);

#endif
