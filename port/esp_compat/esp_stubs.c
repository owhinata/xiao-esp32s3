/*
 * Data that esp-hal-3rdparty sources expect from parts of ESP-IDF this
 * project does not build.  Each entry says why it is correct here.
 */
#include <stdint.h>

/*
 * State of the critical sections of port/esp_compat/include/freertos/FreeRTOS.h:
 * the interrupt level to return to and how deeply the sections are nested.
 */
uint32_t esp_compat_critical_saved_ps;
uint32_t esp_compat_critical_nesting;
