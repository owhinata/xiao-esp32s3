/*
 * Stand-in for the part of ESP-IDF's esp_sleep component (not in
 * esp-hal-3rdparty; its headers come from ESP-IDF, lib/esp-idf) that the
 * compiled sources call.  Declared in esp_sleep/include/esp_private/
 * esp_sleep_internal.h.
 *
 * These functions record what the light and deep sleep code has to keep
 * running while the chip sleeps: oscillators (rtc_clk.c, when the RTC slow
 * clock changes) and the PHY's hook that powers the RF down before deep
 * sleep (phy_init.c, after the first calibration).  This firmware never
 * enters light or deep sleep, so there is nothing to record, and each call
 * succeeds without doing anything.
 */
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"
#include "esp_sleep.h"
#include "esp_private/esp_sleep_internal.h"

/* Nothing is ever requested: every sub-mode's reference count stays 0. */
static int32_t sub_mode_refs[ESP_SLEEP_MODE_MAX];

esp_err_t esp_sleep_sub_mode_config(esp_sleep_sub_mode_t mode, bool activate)
{
    (void) activate;
    return (mode < ESP_SLEEP_MODE_MAX) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t esp_sleep_sub_mode_force_disable(esp_sleep_sub_mode_t mode)
{
    return (mode < ESP_SLEEP_MODE_MAX) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

int32_t *esp_sleep_sub_mode_dump_config(FILE *stream)
{
    (void) stream;
    return sub_mode_refs;
}

esp_err_t esp_deep_sleep_register_phy_hook(esp_deep_sleep_cb_t new_dslp_cb)
{
    (void) new_dslp_cb;
    return ESP_OK;
}
