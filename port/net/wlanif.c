/*
 * The Wi-Fi station as lwIP's network interface: ESP-IDF's
 * esp_netif/lwip/netif/wlanif.c and the Wi-Fi driver binding of
 * esp_wifi/src/wifi_netif.c, with what esp_netif (esp_netif_lwip.c, not
 * built) does for a DHCP client station, written for the one interface.
 * The lwIP calls run in the tcpip thread (no core locking): the functions
 * of wlanif.h hand them over with tcpip_callback().
 *
 * Frames: the driver hands each received frame to wlanif_input() in its
 * task; as ESP-IDF does by default (CONFIG_LWIP_L2_TO_L3_COPY off,
 * esp_netif's esp_pbuf_ref.c), the frame is not copied: a custom pbuf
 * refers to the driver's buffer and gives it back
 * (esp_wifi_internal_free_rx_buffer()) when lwIP frees it.  Frames sent
 * are copied by esp_wifi_internal_tx() into the driver's buffers; when
 * those are full it answers ESP_ERR_NO_MEM, which lwIP gets as ERR_MEM
 * (TCP sends again later), as wlanif.c's low_level_output().
 *
 * The address: DHCP's, reported with IP_EVENT_STA_GOT_IP once per
 * connection and on every change (as esp_netif_internal_dhcpc_cb()), and
 * IP_EVENT_STA_LOST_IP when it is gone.  When the access point is lost the
 * address and DHCP stay for WLANIF_ADDRESS_HOLD_MS (the time of esp_netif's
 * IP-lost timer, esp_netif_start_ip_lost_timer(), also a sys_timeout() in
 * the tcpip thread), so that TCP connections survive a short loss.  For
 * that time lwIP's link stays up as well and the frames are dropped in
 * low_level_output(): lwIP finds no route through an interface whose link
 * is down, and a send without a route fails at once (ERR_RTE, which the
 * socket's writer gets), where a frame that is lost is sent again.  When
 * the station is associated again the link goes down and up in one step,
 * which makes DHCP ask for the same address first (lwIP's
 * dhcp_network_changed_link_up(): DHCP's reboot); its answer reports the
 * address again and ends the hold.  When the time is over without that
 * answer, DHCP releases the address, and starts afresh if the station is
 * associated, else the link goes down until it is.  esp_netif
 * instead clears the address at once (esp_netif_down_api()) and only
 * delays the "lost" event, which ends TCP connections with the link.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "lwip/opt.h"
#include "lwip/dhcp.h"
#include "lwip/mem.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/sys.h"
#include "lwip/tcpip.h"
#include "lwip/timeouts.h"
#include "netif/etharp.h"
#include "lwip_default_hooks.h"

#include "esp_event.h"
#include "esp_log_timestamp.h"
#include "esp_netif_types.h"
#include "esp_private/wifi.h"
#include "esp_random.h"
#include "esp_wifi.h"

#include "app_log.h"

#include "wlanif.h"

LOG_MODULE_REGISTER(net_wlanif);

/* esp_netif defines it; esp_netif is not built. */
ESP_EVENT_DEFINE_BASE(IP_EVENT);

/* The DHCP client's host name (DHCP option 12, what the access point lists). */
#define WLANIF_HOSTNAME         "xiao-esp32s3"

/* How long the tcpip thread waits for room in the event loop's queue. */
#define WLANIF_EVENT_POST_MS    1000u

/* The changes of esp_netif_lwip.c's DHCP_CB_CHANGE. */
#define WLANIF_IPV4_CHANGED                                                    \
    (LWIP_NSC_IPV4_SETTINGS_CHANGED | LWIP_NSC_IPV4_ADDRESS_CHANGED            \
     | LWIP_NSC_IPV4_GATEWAY_CHANGED | LWIP_NSC_IPV4_NETMASK_CHANGED)

static struct netif sta_netif;
static netif_ext_callback_t ext_callback;

/* Set once wlanif_start() has succeeded. */
static volatile bool started;

/* The tcpip thread's own state. */
static bool dhcp_running;
static bool associated;         /* the station has an access point (tcpip thread) */
static bool hold_pending;               /* hold_expired() is due */
static bool announced;                  /* GOT_IP posted since the link came up */
static esp_netif_ip_info_t last_info;   /* what GOT_IP last said */

/* A received frame: lwIP's pbuf on the driver's buffer. */
struct wlanif_rx_pbuf {
    struct pbuf_custom p;
    void *eb;
};

static void hold_expired(void *arg);

static void rx_pbuf_free(struct pbuf *p)
{
    struct wlanif_rx_pbuf *rx = (struct wlanif_rx_pbuf *) p;

    esp_wifi_internal_free_rx_buffer(rx->eb);
    mem_free(rx);
}

/* In the driver's task (wifi_rxcb_t). */
static esp_err_t wlanif_input(void *buffer, uint16_t len, void *eb)
{
    struct wlanif_rx_pbuf *rx;
    struct pbuf *p;

    if (buffer == NULL || !netif_is_up(&sta_netif)) {
        if (eb != NULL) {
            esp_wifi_internal_free_rx_buffer(eb);
        }
        return ESP_FAIL;
    }
    rx = mem_malloc(sizeof(*rx));
    if (rx == NULL) {
        esp_wifi_internal_free_rx_buffer(eb);
        return ESP_ERR_NO_MEM;
    }
    rx->p.custom_free_function = rx_pbuf_free;
    rx->eb = eb;
    p = pbuf_alloced_custom(PBUF_RAW, len, PBUF_REF, &rx->p, buffer, len);
    if (p == NULL) {
        mem_free(rx);
        esp_wifi_internal_free_rx_buffer(eb);
        return ESP_ERR_NO_MEM;
    }
    /* tcpip_input(): to the tcpip thread's mailbox, or ERR_MEM when it is full. */
    if (sta_netif.input(p, &sta_netif) != ERR_OK) {
        pbuf_free(p);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* netif->linkoutput, in the tcpip thread. */
static err_t low_level_output(struct netif *netif, struct pbuf *p)
{
    /* No access point: the frame is lost (the address hold, see above). */
    if (!associated) {
        return ERR_IF;
    }
    struct pbuf *q = p;
    int ret;

    (void) netif;
    if (p->next != NULL) {
        /* A chain (LWIP_NETIF_TX_SINGLE_PBUF makes it rare): one buffer. */
        q = pbuf_alloc(PBUF_RAW_TX, p->tot_len, PBUF_RAM);
        if (q == NULL) {
            return ERR_MEM;
        }
        pbuf_copy(q, p);
    }
    ret = esp_wifi_internal_tx(WIFI_IF_STA, q->payload, q->len);
    if (q != p) {
        pbuf_free(q);
    }
    switch (ret) {
    case ESP_OK:
        return ERR_OK;
    case ESP_ERR_NO_MEM:
        return ERR_MEM;
    case ESP_ERR_INVALID_ARG:
        return ERR_ARG;
    default:
        return ERR_IF;
    }
}

/* netif_add()'s init function: wlanif_init_sta() with low_level_init(). */
static err_t wlanif_init(struct netif *netif)
{
    netif->name[0] = 's';
    netif->name[1] = 't';
    netif->hostname = WLANIF_HOSTNAME;
    netif->output = etharp_output;
    netif->linkoutput = low_level_output;
    netif->hwaddr_len = ETH_HWADDR_LEN;
    if (esp_wifi_get_mac(WIFI_IF_STA, netif->hwaddr) != ESP_OK) {
        return ERR_IF;
    }
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET | NETIF_FLAG_IGMP;
    return ERR_OK;
}

static void post(int32_t id, const ip_event_got_ip_t *event)
{
    esp_err_t err = esp_event_post(IP_EVENT, id, event, sizeof(*event),
                                   pdMS_TO_TICKS(WLANIF_EVENT_POST_MS));

    if (err != ESP_OK) {
        LOG_ERR("IP event %ld not posted: 0x%x", (long) id, (unsigned int) err);
    }
}

/*
 * The interface's changes, in the tcpip thread.  LWIP_NSC_IPV4_ADDR_VALID
 * comes with every address set, also an unchanged one (DHCP's reboot and
 * renewals): GOT_IP once per link and on changes.  An address that
 * becomes 0.0.0.0 (released, refused or expired): LOST_IP.
 */
static void netif_changed(struct netif *netif, netif_nsc_reason_t reason,
                          const netif_ext_callback_args_t *args)
{
    ip_event_got_ip_t event = {0};

    (void) args;
    if (netif != &sta_netif) {
        return;
    }
    event.ip_info.ip.addr = ip4_addr_get_u32(netif_ip4_addr(netif));
    event.ip_info.netmask.addr = ip4_addr_get_u32(netif_ip4_netmask(netif));
    event.ip_info.gw.addr = ip4_addr_get_u32(netif_ip4_gw(netif));

    if ((reason & LWIP_NSC_IPV4_ADDR_VALID) != 0 && event.ip_info.ip.addr != 0u) {
        event.ip_changed = memcmp(&event.ip_info, &last_info, sizeof(last_info)) != 0;
        if (hold_pending) {
            sys_untimeout(hold_expired, NULL);
            hold_pending = false;
        }
        if (!announced || event.ip_changed) {
            announced = true;
            last_info = event.ip_info;
            post(IP_EVENT_STA_GOT_IP, &event);
        }
    } else if ((reason & WLANIF_IPV4_CHANGED) != 0 && event.ip_info.ip.addr == 0u
               && last_info.ip.addr != 0u) {
        announced = false;
        memset(&last_info, 0, sizeof(last_info));
        post(IP_EVENT_STA_LOST_IP, &event);
    }
}

/*
 * The address hold is over (sys_timeout(), tcpip thread): no address was
 * confirmed since the link went down.  DHCP gives it up, and starts again
 * if a link is there by now.
 */
static void hold_expired(void *arg)
{
    (void) arg;
    hold_pending = false;
    if (dhcp_running) {
        dhcp_release_and_stop(&sta_netif);
        dhcp_running = false;
        if (associated) {
            if (dhcp_start(&sta_netif) != ERR_OK) {
                LOG_ERR("dhcp_start failed");
            } else {
                dhcp_running = true;
            }
        }
    }
    if (!associated) {
        netif_set_link_down(&sta_netif);
    }
}

static void link_up_in_tcpip(void *arg)
{
    (void) arg;
    announced = false;
    associated = true;
    if (!dhcp_running) {
        /* With the link down DHCP waits for netif_set_link_up(). */
        if (dhcp_start(&sta_netif) != ERR_OK) {
            LOG_ERR("dhcp_start failed");
        } else {
            dhcp_running = true;
        }
    }
    /* Back within the address hold: down and up, for DHCP's reboot. */
    if (netif_is_link_up(&sta_netif)) {
        netif_set_link_down(&sta_netif);
    }
    netif_set_link_up(&sta_netif);
}

static void link_down_in_tcpip(void *arg)
{
    (void) arg;
    associated = false;
    announced = false;
    if (!dhcp_running) {
        netif_set_link_down(&sta_netif);
    } else if (!hold_pending) {
        /* From the first loss: a link without an address does not extend it. */
        hold_pending = true;
        sys_timeout(WLANIF_ADDRESS_HOLD_MS, hold_expired, NULL);
    }
}

static void stop_in_tcpip(void *arg)
{
    (void) arg;
    sys_untimeout(hold_expired, NULL);
    hold_pending = false;
    if (dhcp_running) {
        dhcp_release_and_stop(&sta_netif);
        dhcp_running = false;
    }
    netif_set_link_down(&sta_netif);
    associated = false;
    announced = false;
}

static void call_in_tcpip(tcpip_callback_fn fn)
{
    if (tcpip_callback(fn, NULL) != ERR_OK) {
        LOG_ERR("tcpip_callback failed");
    }
}

void wlanif_link_up(void)
{
    esp_err_t err = esp_wifi_internal_reg_rxcb(WIFI_IF_STA, wlanif_input);

    if (err != ESP_OK) {
        LOG_ERR("esp_wifi_internal_reg_rxcb failed: 0x%x", (unsigned int) err);
    }
    call_in_tcpip(link_up_in_tcpip);
}

void wlanif_link_down(void)
{
    call_in_tcpip(link_down_in_tcpip);
}

void wlanif_stop(void)
{
    call_in_tcpip(stop_in_tcpip);
}

/* The rest of the start, in the tcpip thread (tcpip_init()'s callback). */
static void start_in_tcpip(void *arg)
{
    sys_sem_t *done = arg;

    if (netif_add(&sta_netif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4, NULL, wlanif_init,
                  tcpip_input) == NULL) {
        LOG_ERR("netif_add failed");
    } else {
        netif_add_ext_callback(&ext_callback, netif_changed);
        netif_set_default(&sta_netif);
        netif_set_up(&sta_netif);
    }
    sys_sem_signal(done);
}

int wlanif_start(void)
{
    uint8_t secret[16];
    sys_sem_t done;

    /* esp_netif_init(): the secret of the TCP initial sequence numbers, then the thread. */
    esp_fill_random(secret, sizeof(secret));
    lwip_init_tcp_isn(esp_log_timestamp(), secret);

    if (sys_sem_new(&done, 0) != ERR_OK) {
        LOG_ERR("lwIP not started: no semaphore");
        return -1;
    }
    tcpip_init(start_in_tcpip, &done);
    sys_arch_sem_wait(&done, 0);
    sys_sem_free(&done);
    if (!netif_is_up(&sta_netif)) {
        return -1;
    }
    started = true;
    return 0;
}

bool wlanif_started(void)
{
    return started;
}
