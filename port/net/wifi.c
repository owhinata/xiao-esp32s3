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
 * temperature once a second, in an esp_timer callback.
 *
 * Everything that changes the connection runs in the default event loop's
 * task ("sys_evt"), one event after the other: the driver's events
 * (WIFI_EVENT), the interface's (IP_EVENT, wlanif.h), and this file's own
 * (ctl_base): the requests of net_wifi_connect() and net_wifi_disconnect(),
 * whose callers wait for the result, and the expiries of the two timers,
 * which their callbacks post from the esp_timer task without waiting.  So
 * the connection has one writer, and the driver, the interface
 * (wlanif.h) and the interface's owner (net_wifi_ops) are called in the
 * order of the events, from that task, as ESP-IDF's station examples call
 * esp_wifi_connect() from their event handlers.  None of them waits for
 * the loop: the tcpip thread posts its events without waiting (wlanif.c),
 * and the driver's task only waits for room in the loop's queue.
 *
 * The connection:
 *
 *   IDLE --net_wifi_connect()--> CONNECTING: the driver associates (the
 *     passphrase's PMK, PBKDF2 in software, then the handshake), DHCP
 *     runs; 30 s for both (lease timer).
 *   CONNECTING --address--> CONNECTED, connect_result(0); --driver's
 *     disconnection or 30 s--> IDLE, connect_result(status).
 *   CONNECTED --loss without a request--> RECONNECTING: link down (the
 *     address stays, wlanif.h), disconnected(reason); a new attempt to the
 *     same network every RETRY_DELAY_US (retry timer), 30 s for each after
 *     its association.
 *   RECONNECTING --address--> CONNECTED, connect_result(0); --the address
 *     hold ends without an access point (WLANIF_ADDRESS_HOLD_MS after the
 *     loss)--> IDLE: ip_clear(), then the reconnection policy's request().
 *     DHCP dropping the address on a new association (a NAK) only clears
 *     it (ip_clear()): the attempt goes on, to a new address.
 *   any --net_wifi_disconnect()--> IDLE.
 *
 * What belongs to an earlier attempt is told apart: the interface's events
 * carry the session (wlanif.h), a new one for each attempt from IDLE; a
 * timer's expiry counts only once its deadline is reached, so a callback
 * that was already on its way when the timer was stopped or set again
 * finds no deadline or a later one; and the driver's report of a
 * disconnection this file asked for (WIFI_REASON_ASSOC_LEAVE) is taken as
 * that, once, not as the failure of the attempt that may follow it.
 *
 * The driver connects again by itself only within the address hold, so
 * that TCP connections survive a short loss (retry_*() below).  When to
 * connect otherwise (at the start, after the hold, after a disconnection,
 * and to which network) is the policy of the interface's owner (struct
 * net_wifi_reconnect_ops: rp2040-connect's Wi-Fi profile), which this file
 * asks in net_wifi_connect(), tells in net_wifi_disconnect(), and calls
 * when it stops trying.
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

/* Between the attempts after a loss, within the address hold. */
#define RETRY_DELAY_US          (3 * 1000000LL)

/* A timer's expiry that found the loop's queue full is posted again this much later. */
#define TIMER_REPOST_US         (10 * 1000LL)

enum conn_state {
    CONN_IDLE,
    CONN_CONNECTING,
    CONN_CONNECTED,
    CONN_RECONNECTING,
};

/* This file's events, in the default event loop. */
static const char ctl_base[] = "NET_WIFI_CTL";

enum ctl_event {
    CTL_CONNECT,                /* net_wifi_connect() */
    CTL_DISCONNECT,             /* net_wifi_disconnect() */
    CTL_LEASE,                  /* the lease timer expired */
    CTL_RETRY,                  /* the retry timer expired */
};

/*
 * What other threads read (net_wifi_status(), net_wifi_scan()), under
 * lock, which is held only while these are read or written; the loop's
 * task writes conn and associated, net_wifi_scan() and
 * net_wifi_scan_end() scanning and the scan's callback.
 */
static TX_MUTEX lock;
static enum conn_state conn;
static bool associated;                 /* WIFI_EVENT_STA_CONNECTED .. _DISCONNECTED */
static bool scanning;
static void (*scan_done)(int status, unsigned int count, void *arg);
static void *scan_arg;

static volatile bool sta_started;       /* set once the driver runs (WIFI_EVENT_STA_START) */
static TX_THREAD *loop_thread;          /* the loop's task, from WIFI_EVENT_STA_START on */

/* The loop's task's own. */
static uint32_t session;                /* the attempt's interface session; 0 in IDLE */
static uint32_t last_session;
static bool leave_pending;              /* esp_wifi_disconnect() called, its report not seen */
static bool have_address;
static uint8_t address[4];              /* the address ip_set() gave, while have_address */
static int64_t attempt_start;           /* esp_timer_get_time() of the attempt's start */
static int64_t associated_at;
static int64_t lost_at;                 /* of the connection that RECONNECTING replaces */
static unsigned int retries;
static int64_t lease_deadline;          /* esp_timer_get_time() it expires at; 0: stopped */
static int64_t retry_deadline;
static esp_timer_handle_t lease_timer;
static esp_timer_handle_t retry_timer;
static const struct net_wifi_ops *ops;
static const struct net_wifi_reconnect_ops *policy;

/*
 * The request in the loop: one at a time (request_lock), its network
 * (CTL_CONNECT; wiped once taken) and its result (request_done).
 */
static TX_MUTEX request_lock;
static TX_SEMAPHORE request_done;
static wifi_config_t request_config;
static int request_result;

static void lock_take(void)
{
    (void) tx_mutex_get(&lock, TX_WAIT_FOREVER);
}

static void lock_give(void)
{
    (void) tx_mutex_put(&lock);
}

static enum conn_state conn_get(void)
{
    enum conn_state c;

    lock_take();
    c = conn;
    lock_give();
    return c;
}

static void conn_set(enum conn_state c)
{
    lock_take();
    conn = c;
    lock_give();
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

/* The threshold of an attempt (see enum net_wifi_auth), for the log. */
static const char *threshold_name(wifi_auth_mode_t mode)
{
    switch (mode) {
    case WIFI_AUTH_OPEN: return "any security";
    case WIFI_AUTH_WPA_PSK: return "WPA-PSK or better";
    case WIFI_AUTH_OWE: return "OWE or better";
    case WIFI_AUTH_WPA2_PSK: return "WPA2-PSK or better";
    case WIFI_AUTH_WPA3_PSK: return "WPA3-SAE";
    default: return "?";
    }
}

static wifi_auth_mode_t threshold_of(enum net_wifi_auth auth)
{
    switch (auth) {
    case NET_WIFI_AUTH_WPA_PSK: return WIFI_AUTH_WPA_PSK;
    case NET_WIFI_AUTH_OWE: return WIFI_AUTH_OWE;
    case NET_WIFI_AUTH_WPA2_PSK: return WIFI_AUTH_WPA2_PSK;
    case NET_WIFI_AUTH_WPA3_SAE: return WIFI_AUTH_WPA3_PSK;
    default: return WIFI_AUTH_OPEN;
    }
}

static wifi_sae_pwe_method_t sae_pwe_of(enum net_wifi_sae_pwe pwe)
{
    switch (pwe) {
    case NET_WIFI_SAE_PWE_HUNT_AND_PECK: return WPA3_SAE_PWE_HUNT_AND_PECK;
    case NET_WIFI_SAE_PWE_HASH_TO_ELEMENT: return WPA3_SAE_PWE_HASH_TO_ELEMENT;
    default: return WPA3_SAE_PWE_BOTH;
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

/* Timers ------------------------------------------------------------------ */

static void timer_arm(esp_timer_handle_t timer, int64_t *deadline, int64_t timeout_us)
{
    *deadline = esp_timer_get_time() + timeout_us;
    (void) esp_timer_stop(timer);
    (void) esp_timer_start_once(timer, (uint64_t) timeout_us);
}

static void timer_disarm(esp_timer_handle_t timer, int64_t *deadline)
{
    *deadline = 0;
    (void) esp_timer_stop(timer);
}

/* An expiry that counts: the timer is armed and its deadline is reached.  It is disarmed then. */
static bool timer_due(int64_t *deadline)
{
    if (*deadline == 0 || esp_timer_get_time() < *deadline) {
        return false;
    }
    *deadline = 0;
    return true;
}

/* In the esp_timer task: the expiry goes to the loop; with its queue full, again a little later. */
static void timer_post(esp_timer_handle_t timer, int32_t id)
{
    if (esp_event_post(ctl_base, id, NULL, 0, 0) != ESP_OK) {
        (void) esp_timer_start_once(timer, (uint64_t) TIMER_REPOST_US);
    }
}

static void lease_fired(void *arg)
{
    (void) arg;
    timer_post(lease_timer, CTL_LEASE);
}

static void retry_fired(void *arg)
{
    (void) arg;
    timer_post(retry_timer, CTL_RETRY);
}

/* The address goes, from the interface's owner too. */
static void address_clear(void)
{
    if (have_address) {
        have_address = false;
        ops->ip_clear(address);
    }
}

/* The driver is told to leave; its report of that is not an attempt's failure. */
static void driver_leave(void)
{
    leave_pending = true;
    (void) esp_wifi_disconnect();
}

/* The end of an attempt, or of a connection, back to IDLE: the session ends. */
static void go_idle(void)
{
    timer_disarm(lease_timer, &lease_deadline);
    conn_set(CONN_IDLE);
    session = 0;
    wlanif_stop();
}

/* Reconnection within the address hold ------------------------------------ */

/* A loss without a request: the attempts start. */
static void retry_start(void)
{
    retries = 0;
    lost_at = esp_timer_get_time();
    timer_arm(retry_timer, &retry_deadline, RETRY_DELAY_US);
}

/* An attempt failed: the next one later. */
static void retry_next(void)
{
    retries++;
    timer_arm(retry_timer, &retry_deadline, RETRY_DELAY_US);
}

/* No more attempts. */
static void retry_stop(void)
{
    timer_disarm(retry_timer, &retry_deadline);
}

/*
 * The address hold is over without a connection: the address goes, the
 * attempts end, and one in progress is stopped; the policy decides what
 * comes next.
 */
static void retry_give_up(void)
{
    if (have_address) {
        LOG_INF("%u.%u.%u.%u dropped: no connection for %lu s, %u attempts failed; "
                "reconnection stopped", address[0], address[1], address[2], address[3],
                ms_since(lost_at) / 1000u, retries);
    } else {
        LOG_INF("no connection for %lu s, %u attempts failed; reconnection stopped",
                ms_since(lost_at) / 1000u, retries);
    }
    address_clear();
    retry_stop();
    go_idle();
    driver_leave();
    if (policy != NULL && policy->request != NULL) {
        policy->request();
    }
}

/*
 * The retry timer: the next attempt, unless a scan runs (then later).  With
 * the address gone (DHCP dropped it after the loss) nothing ends the hold
 * in the interface, so the attempts end here once WLANIF_ADDRESS_HOLD_MS
 * have passed since the loss.
 */
static void retry_due(void)
{
    bool busy;
    bool linked;
    esp_err_t err;

    if (!timer_due(&retry_deadline)) {
        return;
    }
    lock_take();
    busy = scanning;
    linked = associated;
    lock_give();
    if (conn_get() != CONN_RECONNECTING) {
        return;
    }
    if (!have_address && !linked
        && esp_timer_get_time() - lost_at >= (int64_t) WLANIF_ADDRESS_HOLD_MS * 1000) {
        retry_give_up();
        return;
    }
    if (busy) {
        timer_arm(retry_timer, &retry_deadline, RETRY_DELAY_US);
        return;
    }
    attempt_start = esp_timer_get_time();
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        LOG_DBG("esp_wifi_connect failed: 0x%x", (unsigned int) err);
        retry_next();
    }
}

/* ------------------------------------------------------------------------ */

/* The lease timer: the attempt has not reached an address in time. */
static void lease_due(void)
{
    bool linked;

    if (!timer_due(&lease_deadline)) {
        return;
    }
    lock_take();
    linked = associated;
    lock_give();
    switch (conn_get()) {
    case CONN_CONNECTING:
        LOG_INF("connection failed: no address in %lu ms", ms_since(attempt_start));
        go_idle();
        ops->connect_result(NET_WIFI_CONN_TIMEOUT);
        driver_leave();
        break;
    case CONN_RECONNECTING:
        if (linked) {
            LOG_DBG("attempt %u failed: no address", retries + 1u);
            wlanif_link_down();
            driver_leave();
            retry_next();
        }
        break;
    default:
        break;
    }
}

/* net_wifi_connect()'s request, with request_config. */
static int connect_request(void)
{
    esp_err_t err;
    int result = 0;

    lock_take();
    if (conn == CONN_CONNECTED || conn == CONN_RECONNECTING) {
        result = -EALREADY;
    } else if (conn == CONN_CONNECTING || scanning) {
        result = -EBUSY;
    } else {
        conn = CONN_CONNECTING;
    }
    lock_give();
    if (result != 0) {
        return result;
    }
    if (++last_session == 0u) {
        last_session = 1;
    }
    session = last_session;
    attempt_start = esp_timer_get_time();
    timer_arm(lease_timer, &lease_deadline, CONNECT_TIMEOUT_US);
    LOG_INF("connecting to \"%.*s\": %s%s",
            (int) strnlen((const char *) request_config.sta.ssid, sizeof(request_config.sta.ssid)),
            (const char *) request_config.sta.ssid,
            threshold_name(request_config.sta.threshold.authmode),
            request_config.sta.pmf_cfg.required ? ", management frame protection required" : "");

    err = esp_wifi_set_config(WIFI_IF_STA, &request_config);
    if (err == ESP_OK) {
        err = esp_wifi_connect();
    }
    if (err != ESP_OK) {
        LOG_ERR("connection refused by the driver: 0x%x", (unsigned int) err);
        timer_disarm(lease_timer, &lease_deadline);
        conn_set(CONN_IDLE);
        session = 0;
        return -EIO;
    }
    return 0;
}

/*
 * net_wifi_disconnect()'s request.  The release goes out before the driver
 * leaves: wlanif_stop() returns once the tcpip thread has sent it.
 */
static int disconnect_request(void)
{
    if (conn_get() == CONN_IDLE) {
        return -EALREADY;
    }
    retry_stop();
    go_idle();
    ops->link_down();
    address_clear();
    ops->disconnected(NET_WIFI_DISCONN_REQUESTED);
    driver_leave();
    return 0;
}

/* This file's events, in the event loop's task. */
static void on_ctl_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void) arg;
    (void) base;
    (void) data;

    switch (id) {
    case CTL_CONNECT:
        request_result = connect_request();
        memset(&request_config, 0, sizeof(request_config));
        (void) tx_semaphore_put(&request_done);
        break;
    case CTL_DISCONNECT:
        request_result = disconnect_request();
        (void) tx_semaphore_put(&request_done);
        break;
    case CTL_LEASE:
        lease_due();
        break;
    case CTL_RETRY:
        retry_due();
        break;
    default:
        break;
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

static void sta_connected(void)
{
    enum conn_state c;

    lock_take();
    associated = true;
    c = conn;
    lock_give();
    /* The driver reports in order: a disconnection asked for earlier has been reported by now. */
    leave_pending = false;
    associated_at = esp_timer_get_time();
    if (c == CONN_CONNECTING) {
        LOG_INF("associated in %lu ms", ms_since(attempt_start));
        wlanif_link_up(session);
    } else if (c == CONN_RECONNECTING) {
        timer_arm(lease_timer, &lease_deadline, CONNECT_TIMEOUT_US);
        wlanif_link_up(session);
    }
}

static void sta_disconnected(const wifi_event_sta_disconnected_t *d)
{
    bool was_associated;
    enum conn_state c;

    lock_take();
    was_associated = associated;
    associated = false;
    c = conn;
    lock_give();
    if (leave_pending && d->reason == WIFI_REASON_ASSOC_LEAVE) {
        leave_pending = false;
        LOG_INF("disconnected: %s (%u)", reason_name(d->reason), (unsigned int) d->reason);
        return;
    }
    switch (c) {
    case CONN_CONNECTING:
        LOG_INF("connection failed: %s (%u), RSSI %d", reason_name(d->reason),
                (unsigned int) d->reason, (int) d->rssi);
        go_idle();
        ops->connect_result(conn_status_of(d->reason));
        break;
    case CONN_CONNECTED:
        LOG_INF("connection lost: %s (%u), RSSI %d", reason_name(d->reason),
                (unsigned int) d->reason, (int) d->rssi);
        conn_set(CONN_RECONNECTING);
        wlanif_link_down();
        ops->link_down();
        ops->disconnected(disconn_reason_of(d->reason));
        retry_start();
        break;
    case CONN_RECONNECTING:
        LOG_DBG("attempt %u failed: %s (%u)", retries + 1u, reason_name(d->reason),
                (unsigned int) d->reason);
        timer_disarm(lease_timer, &lease_deadline);
        if (was_associated) {
            wlanif_link_down();
        }
        retry_next();
        break;
    default:
        LOG_INF("disconnected: %s (%u)", reason_name(d->reason), (unsigned int) d->reason);
        break;
    }
}

/* The driver's events, in the event loop's task. */
static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void) arg;
    (void) base;

    switch (id) {
    case WIFI_EVENT_STA_START:
        loop_thread = tx_thread_identify();
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
        sta_connected();
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        sta_disconnected(data);
        break;
    default:
        break;
    }
}

static void got_ip(const struct wlanif_ip_event *event)
{
    uint8_t got[4];
    uint8_t mask[4];
    uint8_t gw[4];
    enum conn_state c;
    bool linked;
    esp_err_t err;

    lock_take();
    c = conn;
    linked = associated;
    lock_give();
    /*
     * Another session's (one that has ended), or one from before the loss
     * that the loop delivers after it: the new link confirms the address.
     */
    if (session == 0u || event->session != session || (c != CONN_CONNECTED && !linked)) {
        return;
    }
    memcpy(got, &event->ip.ip_info.ip.addr, 4);
    memcpy(mask, &event->ip.ip_info.netmask.addr, 4);
    memcpy(gw, &event->ip.ip_info.gw.addr, 4);
    if (c == CONN_CONNECTING) {
        LOG_INF("%u.%u.%u.%u, netmask %u.%u.%u.%u, gateway %u.%u.%u.%u: DHCP in %lu ms, "
                "connected in %lu ms", got[0], got[1], got[2], got[3], mask[0], mask[1],
                mask[2], mask[3], gw[0], gw[1], gw[2], gw[3], ms_since(associated_at),
                ms_since(attempt_start));
    } else if (c == CONN_RECONNECTING) {
        LOG_INF("%u.%u.%u.%u, netmask %u.%u.%u.%u, gateway %u.%u.%u.%u: reconnected "
                "%lu ms after the loss, attempt %u", got[0], got[1], got[2], got[3], mask[0],
                mask[1], mask[2], mask[3], gw[0], gw[1], gw[2], gw[3], ms_since(lost_at),
                retries + 1u);
    } else {
        LOG_INF("%u.%u.%u.%u, netmask %u.%u.%u.%u, gateway %u.%u.%u.%u: new from DHCP",
                got[0], got[1], got[2], got[3], mask[0], mask[1], mask[2], mask[3], gw[0],
                gw[1], gw[2], gw[3]);
    }
    /* A different address replaces the one the interface's owner has. */
    if (have_address && memcmp(address, got, 4) != 0) {
        address_clear();
    }
    memcpy(address, got, 4);
    have_address = true;
    /* wifi_default.c's handler of IP_EVENT_STA_GOT_IP. */
    err = esp_wifi_internal_set_sta_ip();
    if (err != ESP_OK) {
        LOG_ERR("esp_wifi_internal_set_sta_ip failed: 0x%x", (unsigned int) err);
    }
    ops->ip_set(address, mask, gw);
    ops->link_up();
    if (c != CONN_CONNECTED) {
        timer_disarm(lease_timer, &lease_deadline);
        conn_set(CONN_CONNECTED);
        ops->connect_result(NET_WIFI_CONN_SUCCESS);
    }
}

/*
 * The address is gone.  The interface says why (enum wlanif_lost): only the
 * end of the address hold without an access point, then and now, ends the
 * attempts after a loss (RECONNECTING).  Otherwise DHCP has let the address go (a NAK, also
 * to the reboot on a new association, or the lease's expiry) or asks afresh
 * after the hold: the station stays associated (or keeps trying), DHCP
 * runs, the interface's owner drops the address as for a new one, and the
 * next address ends the attempt (got_ip()), or the lease timer does.  In
 * CONNECTED the link goes down until DHCP gives an address again; in
 * CONNECTING and IDLE there is no address.
 */
static void lost_ip(const struct wlanif_ip_event *event)
{
    enum conn_state c;
    bool linked;

    lock_take();
    c = conn;
    linked = associated;
    lock_give();
    if (session == 0u || event->session != session || !have_address) {
        return;
    }
    /* Associated again by now: the new link's DHCP starts (wlanif_link_up() comes after). */
    if (c == CONN_RECONNECTING && event->lost == WLANIF_LOST_HOLD_UNLINKED && !linked) {
        retry_give_up();
        return;
    }
    if (event->lost == WLANIF_LOST_DHCP) {
        LOG_INF("%u.%u.%u.%u dropped by DHCP%s", address[0], address[1], address[2],
                address[3], c == CONN_RECONNECTING ? ", asking for a new one" : "");
    } else {
        LOG_INF("%u.%u.%u.%u dropped: not confirmed by DHCP in %u s, asking for a new one",
                address[0], address[1], address[2], address[3],
                (unsigned int) (WLANIF_ADDRESS_HOLD_MS / 1000u));
    }
    if (c == CONN_CONNECTED) {
        ops->link_down();
    }
    address_clear();
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
        lost_ip(data);
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

void net_wifi_set_reconnect_ops(const struct net_wifi_reconnect_ops *table)
{
    policy = table;
}

int net_wifi_start(void)
{
    static const esp_timer_create_args_t lease_args = {.callback = lease_fired, .name = "wifi_lease"};
    static const esp_timer_create_args_t retry_args = {.callback = retry_fired, .name = "wifi_retry"};
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    size_t heap_before;
    size_t pool_before;
    int64_t start;

    if (last_run_failed_here()) {
        LOG_ERR("Wi-Fi left off: the last run ended with a fatal error in this bring-up (\"fault\")");
        return -1;
    }
    if (ops == NULL || tx_mutex_create(&lock, "net_wifi", TX_INHERIT) != TX_SUCCESS
        || tx_mutex_create(&request_lock, "net_wifi_request", TX_INHERIT) != TX_SUCCESS
        || tx_semaphore_create(&request_done, "net_wifi_request", 0) != TX_SUCCESS) {
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
        || failed("esp_event_handler_register",
                  esp_event_handler_register(ctl_base, ESP_EVENT_ANY_ID, on_ctl_event, NULL))
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

/*
 * Runs a request in the loop's task and returns its result; the caller
 * holds request_lock.  -EDEADLK in that task itself, which would wait for
 * itself.
 */
static int request(int32_t id)
{
    esp_err_t err;

    if (tx_thread_identify() == loop_thread) {
        return -EDEADLK;
    }
    err = esp_event_post(ctl_base, id, NULL, 0, portMAX_DELAY);
    if (err != ESP_OK) {
        LOG_ERR("request %ld not posted: 0x%x", (long) id, (unsigned int) err);
        return -EIO;
    }
    (void) tx_semaphore_get(&request_done, TX_WAIT_FOREVER);
    return request_result;
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
    bool keyed;
    bool pmf_required;
    int result;

    if (params == NULL || params->ssid == NULL || params->ssid_len == 0u
        || params->ssid_len > NET_WIFI_SSID_MAX_LEN || params->auth > NET_WIFI_AUTH_WPA3_SAE) {
        return -EINVAL;
    }
    keyed = params->auth == NET_WIFI_AUTH_WPA_PSK || params->auth == NET_WIFI_AUTH_WPA2_PSK
            || params->auth == NET_WIFI_AUTH_WPA3_SAE;
    if (keyed != (params->passphrase != NULL)
        || (keyed && (params->passphrase_len < 8u || params->passphrase_len > 64u))) {
        return -EINVAL;
    }
    if (!sta_started) {
        return -ENETDOWN;
    }
    pmf_required = params->pmf_required || params->auth == NET_WIFI_AUTH_OWE
                || params->auth == NET_WIFI_AUTH_WPA3_SAE;

    (void) tx_mutex_get(&request_lock, TX_WAIT_FOREVER);
    if (policy != NULL && policy->connection_allowed != NULL && !policy->connection_allowed()) {
        (void) tx_mutex_put(&request_lock);
        return -ECANCELED;
    }
    /*
     * As Zephyr's ESP32 driver (drivers/wifi/esp32/src/esp_wifi_drv.c)
     * sets it up, with the threshold, the management frame protection and
     * the SAE method of the request; OWE also enables the station's OWE
     * (wifi_sta_config_t.owe_enabled).
     */
    memset(&request_config, 0, sizeof(request_config));
    memcpy(request_config.sta.ssid, params->ssid, params->ssid_len);
    if (keyed) {
        memcpy(request_config.sta.password, params->passphrase, params->passphrase_len);
    }
    request_config.sta.threshold.authmode = threshold_of(params->auth);
    request_config.sta.owe_enabled = params->auth == NET_WIFI_AUTH_OWE;
    request_config.sta.pmf_cfg.capable = true;
    request_config.sta.pmf_cfg.required = pmf_required;
    request_config.sta.sae_pwe_h2e = sae_pwe_of(params->sae_pwe);
    result = request(CTL_CONNECT);
    memset(&request_config, 0, sizeof(request_config));
    (void) tx_mutex_put(&request_lock);
    return result;
}

int net_wifi_disconnect(void)
{
    int result;

    if (!sta_started) {
        return -ENETDOWN;
    }
    (void) tx_mutex_get(&request_lock, TX_WAIT_FOREVER);
    if (policy != NULL && policy->pause != NULL) {
        policy->pause();
    }
    result = request(CTL_DISCONNECT);
    (void) tx_mutex_put(&request_lock);
    return result;
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
