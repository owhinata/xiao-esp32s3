/*
 * Stand-in for the sdkconfig.h that ESP-IDF generates from Kconfig.
 *
 * One file for both images, the application and the second-stage bootloader
 * (bootloader/CMakeLists.txt), as in ESP-IDF, where both are built from the
 * same sdkconfig: the flash geometry, the crystal, the MMU page size and the
 * partition table offset have to be the same for both, and a second copy
 * could drift.
 *
 * Only symbols that a compiled source, header or linker script tests or uses
 * are here, with ESP-IDF's default value for the ESP32-S3 unless a comment
 * says why this board differs.  Everything else is left undefined, which is
 * what ESP-IDF's sdkconfig.h does for options that are off ("n").  Add a
 * symbol when a newly used source asks for it.
 */
#ifndef SDKCONFIG_H
#define SDKCONFIG_H

/* ---- Target ------------------------------------------------------------- */

#define CONFIG_IDF_TARGET_ESP32S3               1
#define CONFIG_IDF_TARGET_ARCH_XTENSA           1
#define CONFIG_IDF_TARGET                       "esp32s3"
#define CONFIG_IDF_FIRMWARE_CHIP_ID             0x0009

/* Capabilities of the chip that sources test in their CONFIG_ form (Kconfig.soc_caps.in). */
#define CONFIG_SOC_RTC_FAST_MEM_SUPPORTED       1
#define CONFIG_SOC_RTC_SLOW_MEM_SUPPORTED       1
#define CONFIG_SOC_SUPPORTS_SECURE_DL_MODE      1

/* ---- Board: XIAO ESP32S3 ------------------------------------------------ */

/* 40 MHz crystal; only core 0 is used. */
#define CONFIG_XTAL_FREQ                        40
#define CONFIG_FREERTOS_UNICORE                 1
#define CONFIG_ESP_SYSTEM_SINGLE_CORE_MODE      1

/*
 * 8 MB flash, run in DIO mode at 80 MHz (the image headers say the same, see
 * the esptool arguments in CMakeLists.txt).  80 MHz also raises the core
 * voltage at 80/160 MHz CPU clock (DIG_DBIAS_80M_160M in soc/rtc.h), as
 * ESP-IDF requires for that flash speed.
 */
#define CONFIG_ESPTOOLPY_FLASHMODE_VAL          3       /* DIO, recorded in esp_app_desc_t */
#define CONFIG_ESPTOOLPY_FLASHFREQ_80M          1

/* Flash is mapped in 64 KiB pages (fixed on the ESP32-S3). */
#define CONFIG_MMU_PAGE_SIZE                    0x10000

/*
 * Console: UART0 on its default pins (GPIO43 TX = D6, GPIO44 RX = D7) at
 * 115200 baud, ESP-IDF's default.  In the bootloader this makes
 * bootloader_console_init() install the ROM's UART printf, which on this
 * chip also writes every character to the USB Serial/JTAG controller (the
 * ROM's channel 2, what ESP-IDF's Kconfig calls the secondary console,
 * ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG, its default; no compiled source
 * tests that symbol).  So the bootloader's log goes to both.  The
 * application drives both itself (port/board.c) and takes the baud rate from
 * here.
 */
#define CONFIG_ESP_CONSOLE_UART                 1
#define CONFIG_ESP_CONSOLE_UART_NUM             0
#define CONFIG_ESP_CONSOLE_ROM_SERIAL_PORT_NUM  0
#define CONFIG_ESP_CONSOLE_UART_BAUDRATE        115200

/*
 * This firmware is linked against newlib (port/threadx/src/tx_clib_newlib.c);
 * ESP-IDF's default is picolibc.
 */
#define CONFIG_LIBC_NEWLIB                      1

/* ---- Flash layout (partitions.csv) ------------------------------------- */

#define CONFIG_BOOTLOADER_OFFSET_IN_FLASH       0x0
#define CONFIG_PARTITION_TABLE_OFFSET           0x8000

/* ---- Second-stage bootloader (ESP-IDF's defaults) ---------------------- */

/* CPU clock the bootloader runs at; _start raises it to BOARD_CPU_FREQ_MHZ. */
#define CONFIG_BOOTLOADER_CPU_CLK_FREQ_MHZ      80

/* Log: version 1, level Info, text, milliseconds since boot. */
#define CONFIG_BOOTLOADER_LOG_VERSION           1
#define CONFIG_BOOTLOADER_LOG_LEVEL             3
#define CONFIG_BOOTLOADER_LOG_MODE_TEXT_EN      1

/* esp_bootloader_desc_t: version 1, build date and time. */
#define CONFIG_BOOTLOADER_PROJECT_VER           1
#define CONFIG_BOOTLOADER_COMPILE_TIME_DATE     1

/*
 * The bootloader starts the RTC watchdog (9 s) to cover the boot; the
 * application turns it off (_start, as ESP-IDF's init_disable_rtc_wdt).
 */
#define CONFIG_BOOTLOADER_WDT_ENABLE            1
#define CONFIG_BOOTLOADER_WDT_TIME_MS           9000

/* Accesses to unmapped regions of the address space raise an exception. */
#define CONFIG_BOOTLOADER_REGION_PROTECTION_ENABLE 1

/* VDD_SDIO at 1.9 V when it is configured for 1.8 V (not the case on this board). */
#define CONFIG_BOOTLOADER_VDDSDIO_BOOST_1_9V    1

/* The start-up sequence XMC flash chips need. */
#define CONFIG_BOOTLOADER_FLASH_XMC_SUPPORT     1

/* ---- System options the bootloader's sources test (ESP-IDF's defaults) - */

#define CONFIG_ESP_DEBUG_OCDAWARE               1
#define CONFIG_ESP_ERR_TO_NAME_LOOKUP           1
#define CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP 1
#define CONFIG_SPI_FLASH_HPM_ON                 1

/* ---- Application image (esp_app_desc_t, esp_app_format) ---------------- */

#define CONFIG_APP_COMPILE_TIME_DATE            1
#define CONFIG_APP_RETRIEVE_LEN_ELF_SHA         9
#define CONFIG_ESP_EFUSE_BLOCK_REV_MIN_FULL     0
#define CONFIG_ESP_EFUSE_BLOCK_REV_MAX_FULL     199

/*
 * Interrupt watchdog (port/watchdog.c), ESP-IDF's default timeout without
 * PSRAM.  The application's own code and esp_private/esp_int_wdt.h read
 * these; xtensa_vectors.S tests CONFIG_ESP_INT_WDT only together with
 * ESP32-only options, and no source of the bootloader tests either.
 */
#define CONFIG_ESP_INT_WDT                      1
#define CONFIG_ESP_INT_WDT_TIMEOUT_MS           300

/* ---- HAL and log in the application ----------------------------------- */

#define CONFIG_HAL_DEFAULT_ASSERTION_LEVEL      0

/*
 * The application does not use ESP-IDF's log: the HAL sources it compiles
 * need a log version and a timestamp source to compile (1 and "RTOS" are
 * ESP-IDF's defaults), and level 0 (none) removes their calls.
 */
#define CONFIG_LOG_VERSION                      1
#define CONFIG_LOG_TIMESTAMP_SOURCE_RTOS        1
#define CONFIG_LOG_MAXIMUM_LEVEL                0
#define CONFIG_LOG_DEFAULT_LEVEL                0

#endif
