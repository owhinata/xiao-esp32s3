/*
 * Stand-in for "esp_private/esp_sleep_internal.h" of ESP-IDF's esp_sleep
 * component, which esp-hal-3rdparty does not carry.
 *
 * rtc_clk.c (rtc_clk_slow_src_set()) tells the sleep code which oscillators
 * must stay powered during light/deep sleep.  This firmware never enters
 * those sleep modes, so only the part of the interface that rtc_clk.c uses is
 * declared, with the same names and signatures as upstream, so that rtc_clk.c
 * compiles in both images.  The functions have no implementation:
 * rtc_clk_slow_src_set() is not linked into the application, and the
 * bootloader's build of it (BOOTLOADER_BUILD) does not call them.  A source
 * that needs any other sleep sub-mode fails to compile here, on purpose.
 */
#ifndef ESP_COMPAT_ESP_SLEEP_INTERNAL_H
#define ESP_COMPAT_ESP_SLEEP_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"

typedef enum {
    ESP_SLEEP_RTC_USE_RC_FAST_MODE,     /* keep RC_FAST on for RTC peripherals during sleep */
    ESP_SLEEP_MODE_MAX,
} esp_sleep_sub_mode_t;

esp_err_t esp_sleep_sub_mode_config(esp_sleep_sub_mode_t mode, bool activate);
esp_err_t esp_sleep_sub_mode_force_disable(esp_sleep_sub_mode_t mode);
int32_t  *esp_sleep_sub_mode_dump_config(FILE *stream);

#endif
