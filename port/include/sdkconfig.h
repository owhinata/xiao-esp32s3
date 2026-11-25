/*
 * Minimal stand-in for the ESP-IDF generated sdkconfig.h.
 *
 * The esp-hal-3rdparty headers include "sdkconfig.h" and key off a handful
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

/*
 * CPU clock of the first clock set-up (RTC_CLK_CONFIG_DEFAULT() in soc/rtc.h,
 * bootloader_clock_configure()); 80 MHz is ESP-IDF's default for the
 * ESP32-S3.  _start raises it to BOARD_CPU_FREQ_MHZ once flash is mapped.
 */
#define CONFIG_BOOTLOADER_CPU_CLK_FREQ_MHZ      80

#define CONFIG_HAL_DEFAULT_ASSERTION_LEVEL      0

/*
 * esp_log.h is reached only through bootloader_flash_config_esp32s3.c, whose
 * log calls are in functions that are not linked.  It needs a log version to
 * compile; 1 is ESP-IDF's default.  Level 0 (none) removes the calls.
 */
#define CONFIG_LOG_VERSION                      1
#define CONFIG_LOG_MAXIMUM_LEVEL                0
#define CONFIG_LOG_DEFAULT_LEVEL                0

#endif
