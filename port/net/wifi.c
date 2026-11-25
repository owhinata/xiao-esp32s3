/*
 * Wi-Fi on the XIAO ESP32S3: the radio, with ESP-IDF's esp_phy component
 * and its binary libraries (lib/esp-phy-lib).
 *
 * The radio is brought up the way ESP-IDF's Wi-Fi driver does it, up to
 * where the driver itself would start: esp_wifi_init() powers the
 * Wi-Fi/Bluetooth domain up (esp_wifi_bt_power_domain_on()), and
 * esp_wifi_start() enables the PHY for Wi-Fi (esp_phy_enable(), through the
 * driver's OS adapter), which on the first call loads the init data
 * compiled into esp_phy and runs the RF calibration.  From then on esp_phy
 * tracks the PLL against the temperature once a second, in an esp_timer
 * callback.
 *
 * esp_phy has no way to report a failure: it ends in abort(), or hangs.
 * Either becomes a fault record and a restart (port/panic.c,
 * port/watchdog.c).  So that such a failure does not repeat at every
 * start, the bring-up is skipped once when the last run ended with a fatal
 * error in the same thread.
 */
#include <stdint.h>
#include <string.h>

#include "tx_api.h"

#include "esp_phy_init.h"
#include "esp_timer.h"

#include "app_log.h"
#include "fault_record.h"

#include "wifi.h"

LOG_MODULE_REGISTER(wifi);

/* The last run's fatal error happened in the calling thread. */
static int last_run_failed_here(void)
{
    const struct fault_record *last = fault_last();
    TX_THREAD *self = tx_thread_identify();

    return last != NULL && self != TX_NULL && self->tx_thread_name != TX_NULL
           && strncmp(last->task, self->tx_thread_name, sizeof(last->task)) == 0;
}

int net_wifi_start(void)
{
    int64_t start;
    int64_t took_us;

    if (last_run_failed_here()) {
        LOG_ERR("PHY left off: the last run ended with a fatal error in this bring-up (\"fault\")");
        return -1;
    }

    start = esp_timer_get_time();
    esp_wifi_bt_power_domain_on();
    esp_phy_enable(PHY_MODEM_WIFI);
    took_us = esp_timer_get_time() - start;

    LOG_INF("PHY on for Wi-Fi in %lu ms (full calibration), version %s",
            (unsigned long) ((took_us + 500) / 1000), get_phy_version_str());
    return 0;
}
