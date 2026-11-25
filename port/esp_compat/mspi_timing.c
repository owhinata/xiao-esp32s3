/*
 * Stand-in for the functions of ESP-IDF's esp_mspi (not in esp-hal-3rdparty;
 * its headers come from ESP-IDF, lib/esp-idf) that the compiled sources
 * call: of its timing tuning (mspi_timing_tuning/mspi_timing_tuning.c), and
 * one of flash_ops.c.
 *
 * This firmware runs the flash at 80 MHz in DIO mode, which needs no timing
 * tuning (MSPI_TIMING_FLASH_NEEDS_TUNING is for higher speeds), and does
 * not use the PSRAM.  With that configuration ESP-IDF's functions do what
 * these do:
 *
 *   mspi_timing_change_speed_mode_cache_safe()  rtc_init.c, around the
 *       o-code calibration, which runs the CPU from the crystal with the PLL
 *       off.  ESP-IDF removes the extra delays that timing tuning adds to
 *       the flash and PSRAM interface before that switch and puts them back
 *       after it, with the cache frozen; there are none to remove or
 *       restore.
 *   spi_flash_timing_is_tuned()  esp_flash_spi_init.c: false, so the flash
 *       driver keeps the SPI1 timing it finds, and
 *       spi_timing_get_flash_timing_param() is never called (ESP-IDF's
 *       aborts too).
 *   esp_mspi_32bit_address_flash_feature_check()  esp_flash_spi_init.c,
 *       only for flash over 16 MB: supported on this chip.
 *
 * This file runs from RAM (see the linker script), like rtc_init.c, which
 * calls it at the edges of the calibration.
 */
#include <stdbool.h>
#include <stdlib.h>

#include "esp_err.h"
#include "esp_private/mspi_timing_tuning.h"
#include "esp_private/spi_flash_os.h"

void mspi_timing_change_speed_mode_cache_safe(bool switch_down)
{
    (void) switch_down;
}

bool spi_flash_timing_is_tuned(void)
{
    return false;
}

void spi_timing_get_flash_timing_param(spi_flash_hal_timing_config_t *out_timing_config)
{
    (void) out_timing_config;
    abort();
}

esp_err_t esp_mspi_32bit_address_flash_feature_check(void)
{
    return ESP_OK;
}
