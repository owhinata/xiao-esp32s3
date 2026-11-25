/*
 * Wi-Fi on the XIAO ESP32S3: ESP-IDF's Wi-Fi driver (esp_wifi with its
 * binary libraries, wpa_supplicant on mbedTLS, esp_phy) in station mode,
 * brought up as ESP-IDF's station examples do it, with lwIP as the TCP/IP
 * stack (port/net/wlanif.c, in place of esp_netif).
 *
 * esp_wifi_init() starts the driver's task and allocates its buffers,
 * esp_wifi_start() powers the radio and enables the PHY, which on the
 * first start loads the init data compiled into esp_phy and runs the full
 * RF calibration; from then on esp_phy tracks the PLL against the
 * temperature once a second, in an esp_timer callback.  The driver's
 * events (WIFI_EVENT) and the interface's (IP_EVENT, wlanif.h) arrive in
 * the default event loop's task ("sys_evt"), where this file follows the
 * connection; the handlers only change the state and hand work on (to the
 * tcpip thread, to net_wifi_ops), so that the loop is never held up.
 *
 * The connection:
 *
 *   IDLE --net_wifi_connect()--> CONNECTING: the driver associates (the
 *     passphrase's PMK, PBKDF2 in software, then the handshake), DHCP
 *     runs; 30 s for both (lease_timer).
 *   CONNECTING --address--> CONNECTED, connect_result(0); --driver's
 *     disconnection or 30 s--> IDLE, connect_result(status).
 *   CONNECTED --loss without a request--> RECONNECTING: link down (the
 *     address stays, wlanif.h), disconnected(reason); a new attempt every
 *     RETRY_DELAY_MS (retry_timer), 30 s for each after its association.
 *   RECONNECTING --address--> CONNECTED, connect_result(0); the address
 *     goes (ip_clear()) when the loss lasts WLANIF_ADDRESS_HOLD_MS.
 *   any --net_wifi_disconnect()--> IDLE.
 *
 * Reconnecting after a loss is this board's policy, in one place
 * (reconnect_*() below).
 *
 * The driver's failures that it reports come back as esp_err_t and are
 * logged; the board keeps running with the driver stopped where it
 * failed.  Others end in abort() or an assertion, or hang: a fault record
 * and a restart (port/panic.c, port/watchdog.c).  So that such a failure
 * does not repeat at every start, the bring-up is skipped once when the
 * last run ended with a fatal error in the same thread.
 */
#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "tx_api.h"

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_netif_types.h"
#include "esp_private/wifi.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"

#include "app_log.h"
#include "fault_record.h"

#include "wifi.h"
#include "wlanif.h"

LOG_MODULE_REGISTER(net_wifi);

/* The time an attempt has, to the address (rp2040-connect's NINA driver: 60 x 500 ms). */
#define CONNECT_TIMEOUT_US      (30 * 1000000LL)

/* Between the reconnection attempts after a loss. */
#define RETRY_DELAY_US          (3 * 1000000LL)

enum conn_state {
    CONN_IDLE,
    CONN_CONNECTING,
    CONN_CONNECTED,
    CONN_RECONNECTING,
};

/*
 * The state, under lock.  The driver's functions are called outside it:
 * its task posts events with the loop's queue full waiting for "sys_evt",
 * which may be waiting for the lock.
 */
static TX_MUTEX lock;
static volatile bool sta_started;       /* set once the driver runs (WIFI_EVENT_STA_START) */
static enum conn_state conn;
static bool associated;                 /* WIFI_EVENT_STA_CONNECTED .. _DISCONNECTED */
static bool scanning;
static bool have_address;
static uint8_t address[4];              /* the address ip_set() gave, while have_address */
static int64_t attempt_start;           /* esp_timer_get_time() of the attempt's start */
static int64_t associated_at;
static int64_t lost_at;                 /* of the connection that RECONNECTING replaces */
static unsigned int retries;
static esp_timer_handle_t lease_timer;
static esp_timer_handle_t retry_timer;
static const struct net_wifi_ops *ops;

/* The scan in progress. */
static void (*scan_done)(int status, unsigned int count, void *arg);
static void *scan_arg;

static void lock_take(void)
{
    (void) tx_mutex_get(&lock, TX_WAIT_FOREVER);
}

static void lock_give(void)
{
    (void) tx_mutex_put(&lock);
}

static unsigned long ms_since(int64_t start)
{
    return (unsigned long) ((esp_timer_get_time() - start + 500) / 1000);
}

/* The last run's fatal error happened in the calling thread. */
static int last_run_failed_here(void)
{
    const struct fault_record *last = fault_last();
    TX_THREAD *self = tx_thread_identify();

    return last != NULL && self != TX_NULL && self->tx_thread_name != TX_NULL
           && strncmp(last->task, self->tx_thread_name, sizeof(last->task)) == 0;
}

static enum net_wifi_security security_of(wifi_auth_mode_t mode)
{
    switch (mode) {
    case WIFI_AUTH_OPEN:
        return NET_WIFI_SECURITY_OPEN;
    case WIFI_AUTH_WEP:
        return NET_WIFI_SECURITY_WEP;
    case WIFI_AUTH_WPA_PSK:
        return NET_WIFI_SECURITY_WPA_PSK;
    case WIFI_AUTH_WPA2_PSK:
        return NET_WIFI_SECURITY_WPA2_PSK;
    case WIFI_AUTH_WPA_WPA2_PSK:
        return NET_WIFI_SECURITY_WPA_WPA2_PSK;
    case WIFI_AUTH_WPA3_PSK:
        return NET_WIFI_SECURITY_WPA3_SAE;
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return NET_WIFI_SECURITY_WPA2_WPA3;
    case WIFI_AUTH_WAPI_PSK:
        return NET_WIFI_SECURITY_WAPI;
    case WIFI_AUTH_OWE:
        return NET_WIFI_SECURITY_OWE;
    case WIFI_AUTH_ENTERPRISE:
    case WIFI_AUTH_WPA3_ENT_192:
    case WIFI_AUTH_WPA3_ENTERPRISE:
    case WIFI_AUTH_WPA2_WPA3_ENTERPRISE:
    case WIFI_AUTH_WPA_ENTERPRISE:
        return NET_WIFI_SECURITY_ENTERPRISE;
    case WIFI_AUTH_DPP:
        return NET_WIFI_SECURITY_DPP;
    default:
        return NET_WIFI_SECURITY_UNKNOWN;
    }
}

/* See enum net_wifi_mfp: an access point a scan found. */
static enum net_wifi_mfp scan_mfp_of(enum net_wifi_security security)
{
    switch (security) {
    case NET_WIFI_SECURITY_WPA3_SAE:
    case NET_WIFI_SECURITY_OWE:
        return NET_WIFI_MFP_REQUIRED;
    case NET_WIFI_SECURITY_WPA2_WPA3:
        return NET_WIFI_MFP_OPTIONAL;
    case NET_WIFI_SECURITY_OPEN:
    case NET_WIFI_SECURITY_WEP:
    case NET_WIFI_SECURITY_WPA_PSK:
        return NET_WIFI_MFP_DISABLED;
    default:
        return NET_WIFI_MFP_UNKNOWN;
    }
}

/* See enum net_wifi_mfp: the connection, where the station offers it. */
static enum net_wifi_mfp link_mfp_of(enum net_wifi_security security)
{
    enum net_wifi_mfp mfp = scan_mfp_of(security);

    return mfp == NET_WIFI_MFP_UNKNOWN ? NET_WIFI_MFP_OPTIONAL : mfp;
}

/* The names of wifi_err_reason_t (esp_wifi_types_generic.h), for the log. */
static const char *reason_name(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_UNSPECIFIED: return "UNSPECIFIED";
    case WIFI_REASON_AUTH_EXPIRE: return "AUTH_EXPIRE";
    case WIFI_REASON_AUTH_LEAVE: return "AUTH_LEAVE";
    case WIFI_REASON_DISASSOC_DUE_TO_INACTIVITY: return "DISASSOC_DUE_TO_INACTIVITY";
    case WIFI_REASON_ASSOC_TOOMANY: return "ASSOC_TOOMANY";
    case WIFI_REASON_CLASS2_FRAME_FROM_NONAUTH_STA: return "CLASS2_FRAME_FROM_NONAUTH_STA";
    case WIFI_REASON_CLASS3_FRAME_FROM_NONASSOC_STA: return "CLASS3_FRAME_FROM_NONASSOC_STA";
    case WIFI_REASON_ASSOC_LEAVE: return "ASSOC_LEAVE";
    case WIFI_REASON_ASSOC_NOT_AUTHED: return "ASSOC_NOT_AUTHED";
    case WIFI_REASON_DISASSOC_PWRCAP_BAD: return "DISASSOC_PWRCAP_BAD";
    case WIFI_REASON_DISASSOC_SUPCHAN_BAD: return "DISASSOC_SUPCHAN_BAD";
    case WIFI_REASON_BSS_TRANSITION_DISASSOC: return "BSS_TRANSITION_DISASSOC";
    case WIFI_REASON_IE_INVALID: return "IE_INVALID";
    case WIFI_REASON_MIC_FAILURE: return "MIC_FAILURE";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "4WAY_HANDSHAKE_TIMEOUT";
    case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT: return "GROUP_KEY_UPDATE_TIMEOUT";
    case WIFI_REASON_IE_IN_4WAY_DIFFERS: return "IE_IN_4WAY_DIFFERS";
    case WIFI_REASON_GROUP_CIPHER_INVALID: return "GROUP_CIPHER_INVALID";
    case WIFI_REASON_PAIRWISE_CIPHER_INVALID: return "PAIRWISE_CIPHER_INVALID";
    case WIFI_REASON_AKMP_INVALID: return "AKMP_INVALID";
    case WIFI_REASON_UNSUPP_RSN_IE_VERSION: return "UNSUPP_RSN_IE_VERSION";
    case WIFI_REASON_INVALID_RSN_IE_CAP: return "INVALID_RSN_IE_CAP";
    case WIFI_REASON_802_1X_AUTH_FAILED: return "802_1X_AUTH_FAILED";
    case WIFI_REASON_CIPHER_SUITE_REJECTED: return "CIPHER_SUITE_REJECTED";
    case WIFI_REASON_BAD_CIPHER_OR_AKM: return "BAD_CIPHER_OR_AKM";
    case WIFI_REASON_MISSING_ACKS: return "MISSING_ACKS";
    case WIFI_REASON_STA_LEAVING: return "STA_LEAVING";
    case WIFI_REASON_TIMEOUT: return "TIMEOUT";
    case WIFI_REASON_PEER_INITIATED: return "PEER_INITIATED";
    case WIFI_REASON_AP_INITIATED: return "AP_INITIATED";
    case WIFI_REASON_INVALID_PMKID: return "INVALID_PMKID";
    case WIFI_REASON_BEACON_TIMEOUT: return "BEACON_TIMEOUT";
    case WIFI_REASON_NO_AP_FOUND: return "NO_AP_FOUND";
    case WIFI_REASON_AUTH_FAIL: return "AUTH_FAIL";
    case WIFI_REASON_ASSOC_FAIL: return "ASSOC_FAIL";
    case WIFI_REASON_HANDSHAKE_TIMEOUT: return "HANDSHAKE_TIMEOUT";
    case WIFI_REASON_CONNECTION_FAIL: return "CONNECTION_FAIL";
    case WIFI_REASON_AP_TSF_RESET: return "AP_TSF_RESET";
    case WIFI_REASON_ROAMING: return "ROAMING";
    case WIFI_REASON_ASSOC_COMEBACK_TIME_TOO_LONG: return "ASSOC_COMEBACK_TIME_TOO_LONG";
    case WIFI_REASON_SA_QUERY_TIMEOUT: return "SA_QUERY_TIMEOUT";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY: return "NO_AP_FOUND_W_COMPATIBLE_SECURITY";
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD: return "NO_AP_FOUND_IN_AUTHMODE_THRESHOLD";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD: return "NO_AP_FOUND_IN_RSSI_THRESHOLD";
    default: return "?";
    }
}

/* An attempt's failure, as Zephyr's wifi_conn_status. */
static int conn_status_of(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return NET_WIFI_CONN_AP_NOT_FOUND;
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
        return NET_WIFI_CONN_WRONG_PASSWORD;
    case WIFI_REASON_AUTH_EXPIRE:
        return NET_WIFI_CONN_AUTH_REJECT;
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_ASSOC_TOOMANY:
        return NET_WIFI_CONN_ASSOC_REJECT;
    default:
        return NET_WIFI_CONN_FAIL;
    }
}

/* A connection's end without a request, as Zephyr's wifi_disconn_reason. */
static int disconn_reason_of(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_LEAVE:
    case WIFI_REASON_ASSOC_LEAVE:
    case WIFI_REASON_AP_INITIATED:
        return NET_WIFI_DISCONN_AP_LEAVING;
    case WIFI_REASON_DISASSOC_DUE_TO_INACTIVITY:
    case WIFI_REASON_BEACON_TIMEOUT:
        return NET_WIFI_DISCONN_INACTIVITY;
    default:
        return NET_WIFI_DISCONN_UNSPECIFIED;
    }
}

static void timer_restart(esp_timer_handle_t timer, int64_t timeout_us)
{
    (void) esp_timer_stop(timer);
    (void) esp_timer_start_once(timer, (uint64_t) timeout_us);
}

/* The address goes, from the interface's owner too.  Under lock. */
static void address_clear(void)
{
    if (have_address) {
        have_address = false;
        ops->ip_clear(address);
    }
}

/* Reconnection after a loss --------------------------------------------- */

/* A loss without a request: the attempts start.  Under lock. */
static void reconnect_request(void)
{
    retries = 0;
    lost_at = esp_timer_get_time();
    timer_restart(retry_timer, RETRY_DELAY_US);
}

/* An attempt failed: the next one later.  Under lock. */
static void reconnect_failed(void)
{
    retries++;
    timer_restart(retry_timer, RETRY_DELAY_US);
}

/* net_wifi_disconnect(): no more attempts.  Under lock. */
static void reconnect_pause(void)
{
    (void) esp_timer_stop(retry_timer);
}

/* retry_timer, in the esp_timer task. */
static void retry_expired(void *arg)
{
    bool go;
    esp_err_t err;

    (void) arg;
    lock_take();
    go = conn == CONN_RECONNECTING;
    if (go) {
        attempt_start = esp_timer_get_time();
    }
    lock_give();
    if (!go) {
        return;
    }
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        LOG_DBG("esp_wifi_connect failed: 0x%x", (unsigned int) err);
        lock_take();
        if (conn == CONN_RECONNECTING) {
            reconnect_failed();
        }
        lock_give();
    }
}

/* ------------------------------------------------------------------------ */

/* lease_timer: the attempt has not reached an address in time (esp_timer task). */
static void lease_expired(void *arg)
{
    bool disconnect = false;

    (void) arg;
    lock_take();
    if (conn == CONN_CONNECTING) {
        LOG_INF("connection failed: no address in %lu ms", ms_since(attempt_start));
        conn = CONN_IDLE;
        wlanif_stop();
        ops->connect_result(NET_WIFI_CONN_TIMEOUT);
        disconnect = true;
    } else if (conn == CONN_RECONNECTING && associated) {
        /* WIFI_EVENT_STA_DISCONNECTED follows and schedules the next attempt. */
        disconnect = true;
    }
    lock_give();
    if (disconnect) {
        (void) esp_wifi_disconnect();
    }
}

/* The end of a scan, in the event loop's task: the records stay for net_wifi_scan_next(). */
static void scan_finish(const wifi_event_sta_scan_done_t *info)
{
    void (*done)(int, unsigned int, void *);
    void *arg;
    uint16_t found = 0;
    int status = info->status == 0u ? 0 : -EIO;

    LOG_INF("scan done: %u access points%s", (unsigned int) info->number,
            info->status == 0u ? "" : ", failed");
    if (status == 0 && esp_wifi_scan_get_ap_num(&found) != ESP_OK) {
        status = -EIO;
    }
    lock_take();
    done = scan_done;
    arg = scan_arg;
    scan_done = NULL;
    lock_give();
    if (done != NULL) {
        done(status, found, arg);
    } else {
        net_wifi_scan_end();
    }
}

static void sta_connected(const wifi_event_sta_connected_t *c)
{
    (void) c;
    lock_take();
    associated = true;
    associated_at = esp_timer_get_time();
    if (conn == CONN_CONNECTING || conn == CONN_RECONNECTING) {
        if (conn == CONN_CONNECTING) {
            LOG_INF("associated in %lu ms", ms_since(attempt_start));
        } else {
            timer_restart(lease_timer, CONNECT_TIMEOUT_US);
        }
        wlanif_link_up();
    }
    lock_give();
}

static void sta_disconnected(const wifi_event_sta_disconnected_t *d)
{
    bool was_associated;

    lock_take();
    was_associated = associated;
    associated = false;
    switch (conn) {
    case CONN_CONNECTING:
        LOG_INF("connection failed: %s (%u), RSSI %d", reason_name(d->reason),
                (unsigned int) d->reason, (int) d->rssi);
        (void) esp_timer_stop(lease_timer);
        conn = CONN_IDLE;
        wlanif_stop();
        ops->connect_result(conn_status_of(d->reason));
        break;
    case CONN_CONNECTED:
        LOG_INF("connection lost: %s (%u), RSSI %d", reason_name(d->reason),
                (unsigned int) d->reason, (int) d->rssi);
        conn = CONN_RECONNECTING;
        wlanif_link_down();
        ops->link_down();
        ops->disconnected(disconn_reason_of(d->reason));
        reconnect_request();
        break;
    case CONN_RECONNECTING:
        LOG_DBG("attempt %u failed: %s (%u)", retries + 1u, reason_name(d->reason),
                (unsigned int) d->reason);
        (void) esp_timer_stop(lease_timer);
        if (was_associated) {
            wlanif_link_down();
        }
        reconnect_failed();
        break;
    default:
        LOG_INF("disconnected: %s (%u)", reason_name(d->reason), (unsigned int) d->reason);
        break;
    }
    lock_give();
}

/* The driver's events, in the event loop's task. */
static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void) arg;
    (void) base;

    switch (id) {
    case WIFI_EVENT_STA_START:
        sta_started = true;
        LOG_INF("station started");
        break;
    case WIFI_EVENT_STA_STOP:
        sta_started = false;
        LOG_INF("station stopped");
        break;
    case WIFI_EVENT_SCAN_DONE:
        scan_finish(data);
        break;
    case WIFI_EVENT_STA_CONNECTED:
        sta_connected(data);
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        sta_disconnected(data);
        break;
    default:
        break;
    }
}

static void got_ip(const ip_event_got_ip_t *event)
{
    uint8_t mask[4];
    uint8_t gw[4];
    const uint8_t *a;
    esp_err_t err;

    lock_take();
    if (conn == CONN_IDLE) {
        /* Bound just before a disconnection that released it. */
        lock_give();
        return;
    }
    memcpy(address, &event->ip_info.ip.addr, 4);
    memcpy(mask, &event->ip_info.netmask.addr, 4);
    memcpy(gw, &event->ip_info.gw.addr, 4);
    have_address = true;
    a = address;
    if (conn == CONN_CONNECTING) {
        LOG_INF("%u.%u.%u.%u, netmask %u.%u.%u.%u, gateway %u.%u.%u.%u: DHCP in %lu ms, "
                "connected in %lu ms", a[0], a[1], a[2], a[3], mask[0], mask[1], mask[2],
                mask[3], gw[0], gw[1], gw[2], gw[3], ms_since(associated_at),
                ms_since(attempt_start));
    } else if (conn == CONN_RECONNECTING) {
        LOG_INF("%u.%u.%u.%u, netmask %u.%u.%u.%u, gateway %u.%u.%u.%u: reconnected "
                "%lu ms after the loss, attempt %u", a[0], a[1], a[2], a[3], mask[0], mask[1],
                mask[2], mask[3], gw[0], gw[1], gw[2], gw[3], ms_since(lost_at), retries + 1u);
    } else {
        LOG_INF("%u.%u.%u.%u, netmask %u.%u.%u.%u, gateway %u.%u.%u.%u: new from DHCP",
                a[0], a[1], a[2], a[3], mask[0], mask[1], mask[2], mask[3], gw[0], gw[1],
                gw[2], gw[3]);
    }
    /* wifi_default.c's handler of IP_EVENT_STA_GOT_IP. */
    err = esp_wifi_internal_set_sta_ip();
    if (err != ESP_OK) {
        LOG_ERR("esp_wifi_internal_set_sta_ip failed: 0x%x", (unsigned int) err);
    }
    ops->ip_set(address, mask, gw);
    ops->link_up();
    if (conn != CONN_CONNECTED) {
        (void) esp_timer_stop(lease_timer);
        conn = CONN_CONNECTED;
        ops->connect_result(NET_WIFI_CONN_SUCCESS);
    }
    lock_give();
}

static void lost_ip(void)
{
    lock_take();
    if (have_address) {
        LOG_INF("%u.%u.%u.%u dropped%s", address[0], address[1], address[2], address[3],
                conn == CONN_RECONNECTING ? ": no connection for 120 s" : " by DHCP");
        if (conn == CONN_CONNECTED) {
            ops->link_down();
        }
        address_clear();
    }
    lock_give();
}

/* The interface's events (wlanif.h), in the event loop's task. */
static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void) arg;
    (void) base;

    switch (id) {
    case IP_EVENT_STA_GOT_IP:
        got_ip(data);
        break;
    case IP_EVENT_STA_LOST_IP:
        lost_ip();
        break;
    default:
        break;
    }
}

/* Logs a failed step; true if it failed. */
static bool failed(const char *step, esp_err_t err)
{
    if (err != ESP_OK) {
        LOG_ERR("%s failed: 0x%x", step, (unsigned int) err);
        return true;
    }
    return false;
}

void net_wifi_set_ops(const struct net_wifi_ops *table)
{
    ops = table;
}

int net_wifi_start(void)
{
    static const esp_timer_create_args_t lease_args = {.callback = lease_expired, .name = "wifi_lease"};
    static const esp_timer_create_args_t retry_args = {.callback = retry_expired, .name = "wifi_retry"};
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    size_t heap_before;
    size_t pool_before;
    int64_t start;

    if (last_run_failed_here()) {
        LOG_ERR("Wi-Fi left off: the last run ended with a fatal error in this bring-up (\"fault\")");
        return -1;
    }
    if (ops == NULL || tx_mutex_create(&lock, "net_wifi", TX_INHERIT) != TX_SUCCESS) {
        LOG_ERR("Wi-Fi left off: no interface owner or no lock");
        return -1;
    }

    heap_before = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    pool_before = esp_compat_freertos_pool_free();
    start = esp_timer_get_time();

    if (failed("esp_timer_create", esp_timer_create(&lease_args, &lease_timer))
        || failed("esp_timer_create", esp_timer_create(&retry_args, &retry_timer))
        || failed("esp_event_loop_create_default", esp_event_loop_create_default())
        || failed("esp_wifi_init", esp_wifi_init(&cfg))
        || failed("esp_wifi_set_storage", esp_wifi_set_storage(WIFI_STORAGE_RAM))
        || failed("esp_event_handler_register",
                  esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL))
        || failed("esp_event_handler_register",
                  esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, on_ip_event, NULL))
        || failed("esp_wifi_set_mode", esp_wifi_set_mode(WIFI_MODE_STA))
        || failed("esp_wifi_start", esp_wifi_start())
        || failed("esp_wifi_set_ps", esp_wifi_set_ps(WIFI_PS_NONE))
        || wlanif_start() != 0) {
        return -1;
    }

    LOG_INF("up with lwIP in %lu ms; heap free %u -> %u bytes, FreeRTOS pool free %u -> %u bytes",
            ms_since(start), (unsigned int) heap_before,
            (unsigned int) heap_caps_get_free_size(MALLOC_CAP_DEFAULT), (unsigned int) pool_before,
            (unsigned int) esp_compat_freertos_pool_free());
    return 0;
}

int net_wifi_mac(uint8_t mac[NET_WIFI_MAC_LEN])
{
    return esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK ? 0 : -EIO;
}

int net_wifi_scan(void (*done)(int status, unsigned int count, void *arg), void *arg)
{
    esp_err_t err;

    if (!sta_started) {
        return -ENETDOWN;
    }
    lock_take();
    if (scanning || conn == CONN_CONNECTING || conn == CONN_RECONNECTING) {
        lock_give();
        return -EBUSY;
    }
    scanning = true;
    scan_done = done;
    scan_arg = arg;
    lock_give();

    err = esp_wifi_scan_start(NULL, false);
    if (err != ESP_OK) {
        LOG_ERR("esp_wifi_scan_start failed: 0x%x", (unsigned int) err);
        lock_take();
        scan_done = NULL;
        scanning = false;
        lock_give();
        return -EIO;
    }
    return 0;
}

int net_wifi_scan_next(struct net_wifi_scan_entry *entry)
{
    wifi_ap_record_t record;

    if (esp_wifi_scan_get_ap_record(&record) != ESP_OK) {
        return -ENOENT;
    }
    memset(entry, 0, sizeof(*entry));
    entry->ssid_len = (uint8_t) strnlen((const char *) record.ssid, NET_WIFI_SSID_MAX_LEN);
    memcpy(entry->ssid, record.ssid, entry->ssid_len);
    memcpy(entry->bssid, record.bssid, sizeof(entry->bssid));
    entry->channel = record.primary;
    entry->rssi = record.rssi;
    entry->security = security_of(record.authmode);
    entry->mfp = scan_mfp_of(entry->security);
    return 0;
}

void net_wifi_scan_end(void)
{
    (void) esp_wifi_clear_ap_list();
    lock_take();
    scanning = false;
    lock_give();
}

int net_wifi_connect(const struct net_wifi_connect_params *params)
{
    wifi_config_t config;
    esp_err_t err;

    if (params == NULL || params->ssid == NULL || params->ssid_len == 0u
        || params->ssid_len > NET_WIFI_SSID_MAX_LEN
        || (params->passphrase != NULL
            && (params->passphrase_len < 8u || params->passphrase_len > 64u))) {
        return -EINVAL;
    }
    if (!sta_started) {
        return -ENETDOWN;
    }

    /* Zephyr's ESP32 driver's (drivers/wifi/esp32/src/esp_wifi_drv.c) for the same request. */
    memset(&config, 0, sizeof(config));
    memcpy(config.sta.ssid, params->ssid, params->ssid_len);
    if (params->passphrase != NULL) {
        memcpy(config.sta.password, params->passphrase, params->passphrase_len);
        config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;
    config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    lock_take();
    if (conn == CONN_CONNECTED || conn == CONN_RECONNECTING) {
        lock_give();
        return -EALREADY;
    }
    if (conn == CONN_CONNECTING || scanning) {
        lock_give();
        return -EBUSY;
    }
    conn = CONN_CONNECTING;
    attempt_start = esp_timer_get_time();
    timer_restart(lease_timer, CONNECT_TIMEOUT_US);
    lock_give();

    err = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (err == ESP_OK) {
        err = esp_wifi_connect();
    }
    memset(&config, 0, sizeof(config));
    if (err != ESP_OK) {
        LOG_ERR("connection refused by the driver: 0x%x", (unsigned int) err);
        lock_take();
        if (conn == CONN_CONNECTING) {
            (void) esp_timer_stop(lease_timer);
            conn = CONN_IDLE;
        }
        lock_give();
        return -EIO;
    }
    return 0;
}

int net_wifi_disconnect(void)
{
    if (!sta_started) {
        return -ENETDOWN;
    }
    lock_take();
    if (conn == CONN_IDLE) {
        lock_give();
        return -EALREADY;
    }
    reconnect_pause();
    (void) esp_timer_stop(lease_timer);
    conn = CONN_IDLE;
    /* The release goes out before the driver leaves: the tcpip thread is more urgent than callers. */
    wlanif_stop();
    ops->link_down();
    address_clear();
    ops->disconnected(NET_WIFI_DISCONN_REQUESTED);
    lock_give();

    (void) esp_wifi_disconnect();
    return 0;
}

static enum net_wifi_phy phy_of(wifi_phy_mode_t mode)
{
    switch (mode) {
    case WIFI_PHY_MODE_11B:
        return NET_WIFI_PHY_11B;
    case WIFI_PHY_MODE_11G:
        return NET_WIFI_PHY_11G;
    case WIFI_PHY_MODE_HT20:
    case WIFI_PHY_MODE_HT40:
        return NET_WIFI_PHY_11N;
    case WIFI_PHY_MODE_HE20:
        return NET_WIFI_PHY_11AX;
    default:
        return NET_WIFI_PHY_UNKNOWN;
    }
}

int net_wifi_status(struct net_wifi_status *status)
{
    wifi_ap_record_t ap;
    wifi_phy_mode_t mode;
    bool linked;

    memset(status, 0, sizeof(*status));
    status->security = NET_WIFI_SECURITY_UNKNOWN;
    status->mfp = NET_WIFI_MFP_UNKNOWN;
    status->phy = NET_WIFI_PHY_UNKNOWN;
    if (!sta_started) {
        status->state = NET_WIFI_STATE_DOWN;
        return 0;
    }
    lock_take();
    linked = associated && (conn == CONN_CONNECTING || conn == CONN_CONNECTED);
    if (conn == CONN_CONNECTED && associated) {
        status->state = NET_WIFI_STATE_CONNECTED;
    } else if (conn == CONN_CONNECTING) {
        status->state = associated ? NET_WIFI_STATE_ASSOCIATED : NET_WIFI_STATE_ASSOCIATING;
    } else if (conn == CONN_RECONNECTING) {
        status->state = NET_WIFI_STATE_RECONNECTING;
    } else {
        status->state = scanning ? NET_WIFI_STATE_SCANNING : NET_WIFI_STATE_IDLE;
    }
    lock_give();

    if (linked && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        status->ssid_len = (uint8_t) strnlen((const char *) ap.ssid, NET_WIFI_SSID_MAX_LEN);
        memcpy(status->ssid, ap.ssid, status->ssid_len);
        memcpy(status->bssid, ap.bssid, sizeof(status->bssid));
        status->channel = ap.primary;
        status->rssi = ap.rssi;
        status->security = security_of(ap.authmode);
        status->mfp = link_mfp_of(status->security);
        if (esp_wifi_sta_get_negotiated_phymode(&mode) == ESP_OK) {
            status->phy = phy_of(mode);
        }
    }
    return 0;
}
