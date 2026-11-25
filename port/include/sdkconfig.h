/*
 * Minimal stand-in for the ESP-IDF generated sdkconfig.h.
 *
 * The esp-hal-components headers include "sdkconfig.h" and key off a handful
 * of CONFIG_ symbols.  Only the ones this project actually needs are set; add
 * more when a newly used HAL header asks for them.
 */
#ifndef SDKCONFIG_H
#define SDKCONFIG_H

#define CONFIG_IDF_TARGET_ESP32S3               1
#define CONFIG_IDF_TARGET_ARCH_XTENSA           1
#define CONFIG_IDF_TARGET                       "esp32s3"
#define CONFIG_IDF_FIRMWARE_CHIP_ID             0x0009

/* XIAO ESP32S3: 40 MHz crystal, single core used. */
#define CONFIG_XTAL_FREQ                        40
#define CONFIG_FREERTOS_UNICORE                 1
#define CONFIG_ESP_SYSTEM_SINGLE_CORE_MODE      1

/* Flash is mapped in 64 KiB pages (fixed on the ESP32-S3). */
#define CONFIG_MMU_PAGE_SIZE                    0x10000

#define CONFIG_HAL_DEFAULT_ASSERTION_LEVEL      0
#define CONFIG_LOG_MAXIMUM_LEVEL                0
#define CONFIG_LOG_DEFAULT_LEVEL                0

#endif
