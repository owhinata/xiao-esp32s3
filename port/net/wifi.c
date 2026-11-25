/*
 * Wi-Fi on the XIAO ESP32S3: ESP-IDF's Wi-Fi driver (esp_wifi with its
 * binary libraries, wpa_supplicant on mbedTLS, esp_phy) in station mode,
 * brought up as ESP-IDF's station examples do it, without a network
 * interface (no esp_netif, no lwIP yet).
 *
 * esp_wifi_init() starts the driver's task and allocates its buffers,
 * esp_wifi_start() powers the radio and enables the PHY, which on the
 * first start loads the init data compiled into esp_phy and runs the full
 * RF calibration; from then on esp_phy tracks the PLL against the
 * temperature once a second, in an esp_timer callback.  The driver's
 * events arrive in the default event loop's task ("sys_evt"), where this
 * file logs them and hands scan results over.
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
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"

#include "app_log.h"
#include "fault_record.h"

#include "wifi.h"

LOG_MODULE_REGISTER(net_wifi);

/* Set once the driver runs (WIFI_EVENT_STA_START). */
static volatile bool sta_started;

/* The scan in progress: its callbacks, NULL when there is none. */
static void (*scan_result)(const struct net_wifi_scan_entry *entry, void *arg);
static void (*scan_done)(int status, void *arg);
static void *scan_arg;
static volatile bool scanning;

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

/* See enum net_wifi_mfp. */
static enum net_wifi_mfp mfp_of(enum net_wifi_security security)
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

/*
 * The end of a scan, in the event loop's task: the driver's records one
 * by one (each read frees it), then what is left of the list; then the
 * next scan may start, and the scan's end is reported.
 */
static void scan_finish(const wifi_event_sta_scan_done_t *info)
{
    void (*result)(const struct net_wifi_scan_entry *, void *) = scan_result;
    void (*done)(int, void *) = scan_done;
    void *arg = scan_arg;
    uint16_t found = 0;
    wifi_ap_record_t record;
    struct net_wifi_scan_entry entry;

    LOG_INF("scan done: %u access points%s", (unsigned int) info->number,
            info->status == 0u ? "" : ", failed");
    if (info->status == 0u && esp_wifi_scan_get_ap_num(&found) == ESP_OK) {
        while (found-- > 0u && esp_wifi_scan_get_ap_record(&record) == ESP_OK) {
            if (result == NULL) {
                continue;
            }
            memset(&entry, 0, sizeof(entry));
            entry.ssid_len = (uint8_t) strnlen((const char *) record.ssid, NET_WIFI_SSID_MAX_LEN);
            memcpy(entry.ssid, record.ssid, entry.ssid_len);
            memcpy(entry.bssid, record.bssid, sizeof(entry.bssid));
            entry.channel = record.primary;
            entry.rssi = record.rssi;
            entry.security = security_of(record.authmode);
            entry.mfp = mfp_of(entry.security);
            result(&entry, arg);
        }
    }
    (void) esp_wifi_clear_ap_list();
    scan_result = NULL;
    scan_done = NULL;
    scanning = false;
    if (done != NULL) {
        done(info->status == 0u ? 0 : -EIO, arg);
    }
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
    case WIFI_EVENT_STA_CONNECTED: {
        const wifi_event_sta_connected_t *c = data;

        LOG_INF("connected to \"%.*s\", channel %u", (int) c->ssid_len, (const char *) c->ssid,
                (unsigned int) c->channel);
        break;
    }
    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *d = data;

        LOG_INF("disconnected from \"%.*s\", reason %u, RSSI %d", (int) d->ssid_len,
                (const char *) d->ssid, (unsigned int) d->reason, (int) d->rssi);
        break;
    }
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

int net_wifi_start(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    size_t heap_before;
    size_t pool_before;
    int64_t start;

    if (last_run_failed_here()) {
        LOG_ERR("Wi-Fi left off: the last run ended with a fatal error in this bring-up (\"fault\")");
        return -1;
    }

    heap_before = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    pool_before = esp_compat_freertos_pool_free();
    start = esp_timer_get_time();

    if (failed("esp_event_loop_create_default", esp_event_loop_create_default())
        || failed("esp_wifi_init", esp_wifi_init(&cfg))
        || failed("esp_wifi_set_storage", esp_wifi_set_storage(WIFI_STORAGE_RAM))
        || failed("esp_event_handler_register",
                  esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL))
        || failed("esp_wifi_set_mode", esp_wifi_set_mode(WIFI_MODE_STA))
        || failed("esp_wifi_start", esp_wifi_start())
        || failed("esp_wifi_set_ps", esp_wifi_set_ps(WIFI_PS_NONE))) {
        return -1;
    }

    LOG_INF("up in %lu ms; heap free %u -> %u bytes, FreeRTOS pool free %u -> %u bytes",
            (unsigned long) ((esp_timer_get_time() - start + 500) / 1000),
            (unsigned int) heap_before, (unsigned int) heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
            (unsigned int) pool_before, (unsigned int) esp_compat_freertos_pool_free());
    return 0;
}

int net_wifi_scan(void (*result)(const struct net_wifi_scan_entry *entry, void *arg),
                  void (*done)(int status, void *arg), void *arg)
{
    TX_INTERRUPT_SAVE_AREA
    esp_err_t err;

    if (!sta_started) {
        return -ENETDOWN;
    }
    TX_DISABLE
    if (scanning) {
        TX_RESTORE
        return -EBUSY;
    }
    scanning = true;
    scan_result = result;
    scan_done = done;
    scan_arg = arg;
    TX_RESTORE

    err = esp_wifi_scan_start(NULL, false);
    if (err != ESP_OK) {
        LOG_ERR("esp_wifi_scan_start failed: 0x%x", (unsigned int) err);
        scan_result = NULL;
        scan_done = NULL;
        scanning = false;
        return -EIO;
    }
    return 0;
}

enum net_wifi_state net_wifi_state(void)
{
    if (!sta_started) {
        return NET_WIFI_STATE_DOWN;
    }
    return scanning ? NET_WIFI_STATE_SCANNING : NET_WIFI_STATE_IDLE;
}
