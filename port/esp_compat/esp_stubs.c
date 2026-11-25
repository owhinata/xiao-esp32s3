/*
 * Functions and data that esp-hal-3rdparty sources expect from parts of
 * ESP-IDF this project does not build.  Each entry says why it is correct
 * here.  This file is kept in RAM (see the linker script): its users run
 * before the flash is mapped.
 */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_private/esp_sleep_internal.h"

/*
 * State of the critical sections of port/esp_compat/include/freertos/FreeRTOS.h:
 * the interrupt level to return to and how deeply the sections are nested.
 */
uint32_t esp_compat_critical_saved_ps;
uint32_t esp_compat_critical_nesting;

/*
 * rtc_clk.c tells the sleep code which oscillators must stay powered during
 * light/deep sleep, and asks which have been requested.  This firmware never
 * enters those sleep modes: nothing has to be recorded, and no sub-mode is
 * ever requested.
 */
esp_err_t esp_sleep_sub_mode_config(esp_sleep_sub_mode_t mode, bool activate)
{
    (void) mode;
    (void) activate;
    return ESP_OK;
}

esp_err_t esp_sleep_sub_mode_force_disable(esp_sleep_sub_mode_t mode)
{
    (void) mode;
    return ESP_OK;
}

int32_t *esp_sleep_sub_mode_dump_config(FILE *stream)
{
    static int32_t requests[ESP_SLEEP_MODE_MAX];   /* all zero: nothing requested */

    (void) stream;
    return requests;
}
