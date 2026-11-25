/*
 * Functions that esp-hal-components sources call but whose real
 * implementation lives in a part of ESP-IDF this project does not build.
 * Each entry says why doing nothing is correct here.
 */
#include <stdbool.h>

#include "esp_err.h"
#include "esp_private/esp_sleep_internal.h"

/*
 * rtc_clk.c tells the sleep code which oscillators must stay powered during
 * light/deep sleep.  This firmware never enters those sleep modes.
 */
esp_err_t esp_sleep_sub_mode_config(esp_sleep_sub_mode_t mode, bool activate)
{
    (void) mode;
    (void) activate;
    return ESP_OK;
}
