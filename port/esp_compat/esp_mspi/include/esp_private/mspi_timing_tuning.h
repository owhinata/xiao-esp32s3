/*
 * Stand-in for "esp_private/mspi_timing_tuning.h" of ESP-IDF's esp_mspi
 * component, which esp-hal-3rdparty does not carry.
 *
 * Its only user here is rtc_init.c, which both images compile for the
 * regulator bias variables that rtc_clk.c reads.  Neither calls rtc_init()
 * (see CMakeLists.txt), so the one function declared here, with upstream's
 * signature, is never referenced in a linked image and has no
 * implementation.  Calling rtc_init() would need the real MSPI timing code.
 */
#ifndef ESP_COMPAT_MSPI_TIMING_TUNING_H
#define ESP_COMPAT_MSPI_TIMING_TUNING_H

#include <stdbool.h>

void mspi_timing_change_speed_mode_cache_safe(bool switch_down);

#endif
