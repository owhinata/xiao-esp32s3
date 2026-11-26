/*
 * The Wi-Fi station's lwIP network interface (port/net/wlanif.c), for the
 * driver's side (port/net/wifi.c).  This header does not include lwIP's:
 * its users see only these functions.
 *
 * The interface reports what happens to its IPv4 address as ESP-IDF's
 * esp_netif does, with the events of the default event loop:
 * IP_EVENT_STA_GOT_IP (the address, netmask and gateway DHCP gave, after
 * every connection and whenever they change) and IP_EVENT_STA_LOST_IP (the
 * address is gone), posted from the tcpip thread, each until it goes out.
 * Their data is struct wlanif_ip_event: ESP-IDF's ip_event_got_ip_t, then
 * the session they belong to.  The functions below hand their work to the
 * tcpip thread; it runs it in the order the calls were made.  Called from
 * one thread at a time.
 */
#ifndef PORT_NET_WLANIF_H
#define PORT_NET_WLANIF_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_netif_types.h"

/*
 * A session runs from wlanif_link_up() with its number until
 * wlanif_stop(); losses (wlanif_link_down()) and links that come back
 * within it keep it.  No events outside one.  An event still in the
 * loop's queue when its session has ended carries the old number.
 */
struct wlanif_ip_event {
    ip_event_got_ip_t ip;
    uint32_t session;
    uint8_t lost;               /* IP_EVENT_STA_LOST_IP: enum wlanif_lost */
};

/* Why the address went (IP_EVENT_STA_LOST_IP). */
enum wlanif_lost {
    WLANIF_LOST_DHCP,           /* DHCP gave it up: refused (NAK) or the lease expired */
    WLANIF_LOST_HOLD_LINKED,    /* the address hold ended, associated but not confirmed;
                                   DHCP asks for an address afresh */
    WLANIF_LOST_HOLD_UNLINKED,  /* the address hold ended without an access point */
};

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
 * the address the interface still has (DHCP's reboot).  session (not 0)
 * tags the IP events from now on: a new number after wlanif_stop(), the
 * same while the session runs.
 */
void wlanif_link_up(uint32_t session);

/*
 * The station has lost its access point: the link is down; the address,
 * if there is one, stays for WLANIF_ADDRESS_HOLD_MS from the first loss,
 * so that TCP connections outlive a short one, and is released after that
 * unless DHCP has confirmed it on a new link (wlanif_link_up()) first.
 */
void wlanif_link_down(void);

/*
 * The station leaves: the session ends, DHCP releases the address and
 * stops, the link is down.  Returns when the tcpip thread has done it, so
 * that the release goes out before the driver is told to disconnect.
 */
void wlanif_stop(void);

#endif
