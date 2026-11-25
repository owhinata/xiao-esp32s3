/*
 * Stand-in for the one function of ESP-IDF's esp_mspi timing tuning
 * (esp_mspi/mspi_timing_tuning/mspi_timing_tuning.c, not in esp-hal-3rdparty;
 * its header comes from ESP-IDF, lib/esp-idf) that the compiled sources
 * call: rtc_init.c, around the o-code calibration, which runs the CPU from
 * the crystal with the PLL off.
 *
 * ESP-IDF removes the extra delays that timing tuning adds to the flash and
 * PSRAM interface before that switch, and puts them back after it, with the
 * cache frozen.  This firmware runs the flash at 80 MHz in DIO mode, which
 * needs no tuning (MSPI_TIMING_FLASH_NEEDS_TUNING is for higher speeds),
 * and does not use the PSRAM: there are no delays to remove or restore.
 * rtc_init() runs from RAM with interrupts masked (see the linker script),
 * so nothing reads the flash during the switch either.
 */
#include <stdbool.h>

#include "esp_private/mspi_timing_tuning.h"

void mspi_timing_change_speed_mode_cache_safe(bool switch_down)
{
    (void) switch_down;
}
