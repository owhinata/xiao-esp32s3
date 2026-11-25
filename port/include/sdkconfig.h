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
#define CONFIG_SOC_WIFI_SUPPORTED               1
#define CONFIG_SOC_TEMP_SENSOR_SUPPORTED        1

/* ---- Board: XIAO ESP32S3 ------------------------------------------------ */

/* 40 MHz crystal; only core 0 is used. */
#define CONFIG_XTAL_FREQ                        40
#define CONFIG_FREERTOS_UNICORE                 1
#define CONFIG_ESP_SYSTEM_SINGLE_CORE_MODE      1
#define CONFIG_FREERTOS_NUMBER_OF_CORES         1

/*
 * CPU clock esp_clk_init() sets (ESP-IDF: 160 MHz); BOARD_CPU_FREQ_MHZ in
 * port/board.h says the same (port/startup.c checks).
 */
#define CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ         240

/*
 * RTC slow clock: the internal 136 kHz RC oscillator (the XIAO has no 32 kHz
 * crystal; esp_clk_init() takes it when no other CONFIG_RTC_CLK_SRC_* is
 * defined), calibrated against the crystal over 1024 of its cycles at
 * start-up, ESP-IDF's defaults.
 */
#define CONFIG_RTC_CLK_CAL_CYCLES               1024

/*
 * Cache sizes (ESP-IDF's defaults; port/startup.c and the linker script set
 * the same): the heap's memory layout leaves SRAM0's instruction cache part
 * out and takes the rest of SRAM0.
 */
#define CONFIG_ESP32S3_INSTRUCTION_CACHE_16KB   1
#define CONFIG_ESP32S3_DATA_CACHE_32KB          1

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
#define CONFIG_USJ_ENABLE_USB_SERIAL_JTAG       1   /* esp_perip_clk_init() keeps the controller's clock */
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

/*
 * esp_timer (ESP-IDF's defaults): on SYSTIMER, its interrupt at level 1 and
 * its task on core 0, 3584 bytes of task stack (esp_task.h adds 512).  Its
 * code stays in flash (CONFIG_ESP_TIMER_IN_IRAM off), which nothing here
 * ever disables.
 */
#define CONFIG_ESP_TIMER_IMPL_SYSTIMER          1
#define CONFIG_ESP_TIMER_TASK_STACK_SIZE        3584
#define CONFIG_ESP_TIMER_INTERRUPT_LEVEL        1
#define CONFIG_ESP_TIMER_TASK_AFFINITY          0x0
#define CONFIG_ESP_TIMER_TASK_AFFINITY_CPU0     1
#define CONFIG_ESP_TIMER_ISR_AFFINITY_CPU0      1

/* ---- Heap (ESP-IDF's heap component, esp_libc's malloc()) ------------- */

/*
 * TLSF compiled from source (the ESP32-S3's ROM has none), no poisoning and
 * no tracing (ESP-IDF's defaults, which no symbol here needs to say),
 * memory that can execute is allocatable (no memory protection), the
 * allocation functions in flash: this firmware never disables the cache,
 * so the heap is usable from interrupt handlers without being in RAM.
 */
#define CONFIG_HEAP_HAS_EXEC_HEAP               1
#define CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH   1

/* ---- HAL and log in the application ----------------------------------- */

#define CONFIG_HAL_DEFAULT_ASSERTION_LEVEL      0

/*
 * ESP-IDF's log (log component, version 1, text), up to and by default at
 * Info.  port/esp_log_ram.c puts its lines into the RAM log ("dmesg").
 * Levels can be changed at run time (esp_log_level_set()), for all tags
 * together: no per-tag table (ESP-IDF's default keeps one, which takes a
 * mutex and allocates on a tag's first message; this way a message costs
 * neither, in any context).
 *
 * The shell's log layer (shell/common/compat/include/app_log.h) reads
 * CONFIG_LOG_DEFAULT_LEVEL too, as Zephyr's option of that name, wherever
 * this file is visible.  Zephyr's level numbers are ESP-IDF's (3 = Info),
 * so its default stays Info (port/esp_log_ram.c checks).
 */
#define CONFIG_LOG_VERSION                      1
#define CONFIG_LOG_TIMESTAMP_SOURCE_RTOS        1
#define CONFIG_LOG_MAXIMUM_LEVEL                3
#define CONFIG_LOG_DEFAULT_LEVEL                3
#define CONFIG_LOG_MODE_TEXT_EN                 1
#define CONFIG_LOG_DYNAMIC_LEVEL_CONTROL        1
#define CONFIG_LOG_TAG_LEVEL_IMPL_NONE          1

/* ---- PHY (esp_phy and its binary libraries) ---------------------------- */

/*
 * ESP-IDF's defaults, except that no calibration data is kept in NVS
 * (CONFIG_ESP_PHY_CALIBRATION_AND_DATA_STORAGE off): every start does a
 * full RF calibration.  The init data is the one compiled in
 * (esp32s3/phy_init_data.c; the phy_init partition is not read).
 * CONFIG_ESP_PHY_ENABLE_USB keeps the USB Serial/JTAG controller's PHY
 * clock on while the PHY is set up (the console on the USB-C connector).
 * CONFIG_ESP_WIFI_ENABLED: the PHY is enabled for Wi-Fi, which its PLL
 * tracking (esp_phy/src/phy_common.c) tests.
 */
#define CONFIG_ESP_PHY_ENABLED                  1
#define CONFIG_ESP_PHY_MAX_TX_POWER             20
#define CONFIG_ESP_PHY_MAX_WIFI_TX_POWER        20
#define CONFIG_ESP_PHY_ENABLE_USB               1
#define CONFIG_ESP_PHY_PLL_TRACK_PERIOD_MS      1000

/*
 * MAC addresses (mac_addr.c), ESP-IDF's default for the ESP32-S3: four
 * universally administered addresses from the factory base MAC in eFuse,
 * Wi-Fi station = base, SoftAP + 1, Bluetooth + 2, Ethernet + 3.
 */
#define CONFIG_ESP32S3_UNIVERSAL_MAC_ADDRESSES_FOUR 1
#define CONFIG_ESP32S3_UNIVERSAL_MAC_ADDRESSES  4
#define CONFIG_ESP_MAC_UNIVERSAL_MAC_ADDRESSES_FOUR 1
#define CONFIG_ESP_MAC_UNIVERSAL_MAC_ADDRESSES  4
#define CONFIG_ESP_MAC_ADDR_UNIVERSE_WIFI_STA   1
#define CONFIG_ESP_MAC_ADDR_UNIVERSE_WIFI_AP    1
#define CONFIG_ESP_MAC_ADDR_UNIVERSE_BT         1
#define CONFIG_ESP_MAC_ADDR_UNIVERSE_ETH        1

/* ---- Event loop (esp_event) -------------------------------------------- */

/*
 * ESP-IDF's defaults: events may be posted from interrupt handlers, the
 * default loop's queue holds 32 events, its task has 2304 bytes of stack
 * (esp_task.h adds 512).
 */
#define CONFIG_ESP_EVENT_POST_FROM_ISR          1
#define CONFIG_ESP_EVENT_POST_FROM_IRAM_ISR     1
#define CONFIG_ESP_SYSTEM_EVENT_QUEUE_SIZE      32
#define CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE 2304

/* ---- Wi-Fi (esp_wifi, its binary libraries, wpa_supplicant) ------------ */

/*
 * Station mode with WPA2-PSK and WPA3-SAE (with hash-to-element) and OWE,
 * the supplicant's crypto on mbedTLS (ESP-IDF's defaults), with these
 * differences:
 *   - no NVS (CONFIG_ESP_WIFI_NVS_ENABLED off): the driver keeps its
 *     configuration in RAM only, and nvs_flash is not built
 *     (port/esp_compat/nvs.c);
 *   - no SoftAP (CONFIG_ESP_WIFI_SOFTAP_SUPPORT off): wifi_init.c then
 *     supplies the stubs the libraries' SoftAP hooks take, and the
 *     supplicant's hostap part is not built; SAE for SoftAP, OWE for SoftAP
 *     and WPA3 compatibility mode go with it;
 *   - no enterprise (802.1X/EAP) and no SAE-PK, so no TLS for the
 *     supplicant;
 *   - CONFIG_ESP_WIFI_IRAM_OPT and CONFIG_ESP_WIFI_RX_IRAM_OPT off: the
 *     libraries' code stays in flash (see the linker script), which
 *     nothing here ever disables.
 * Buffers: 10 static and 32 dynamic receive buffers, 32 dynamic transmit
 * buffers, 5 static management receive buffers; A-MPDU both ways with a
 * block-ack window of 6.  The Wi-Fi task is not pinned (one core).
 */
#define CONFIG_ESP_WIFI_ENABLED                 1
#define CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM    10
#define CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM   32
#define CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER       1
#define CONFIG_ESP_WIFI_TX_BUFFER_TYPE          1
#define CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM   32
#define CONFIG_ESP_WIFI_STATIC_RX_MGMT_BUFFER   1
#define CONFIG_ESP_WIFI_DYNAMIC_RX_MGMT_BUF     0
#define CONFIG_ESP_WIFI_RX_MGMT_BUF_NUM_DEF     5
#define CONFIG_ESP_WIFI_AMPDU_TX_ENABLED        1
#define CONFIG_ESP_WIFI_TX_BA_WIN               6
#define CONFIG_ESP_WIFI_AMPDU_RX_ENABLED        1
#define CONFIG_ESP_WIFI_RX_BA_WIN               6
#define CONFIG_ESP_WIFI_SOFTAP_BEACON_MAX_LEN   752
#define CONFIG_ESP_WIFI_MGMT_SBUF_NUM           32
#define CONFIG_ESP_WIFI_ESPNOW_MAX_ENCRYPT_NUM  7
#define CONFIG_ESP_WIFI_STA_DISCONNECTED_PM_ENABLE 1
#define CONFIG_ESP_WIFI_SLP_DEFAULT_MIN_ACTIVE_TIME 50
#define CONFIG_ESP_WIFI_SLP_DEFAULT_MAX_ACTIVE_TIME 10
#define CONFIG_ESP_WIFI_SLP_DEFAULT_WAIT_BROADCAST_DATA_TIME 15
#define CONFIG_ESP_WIFI_TX_HETB_QUEUE_NUM       3
#define CONFIG_ESP_WIFI_RMAC_AUTO_RESET_INTERVAL 12
#define CONFIG_ESP_WIFI_MBEDTLS_CRYPTO          1
#define CONFIG_ESP_WIFI_ENABLE_WPA3_SAE         1
#define CONFIG_ESP_WIFI_ENABLE_SAE_H2E          1
#define CONFIG_ESP_WIFI_ENABLE_WPA3_OWE_STA     1
#define CONFIG_ESP_WIFI_GMAC_SUPPORT            1

/*
 * Coexistence: the adapter's part of esp_coex (esp_coex_adapter.c), which
 * the Wi-Fi adapter's table uses.  Software coexistence
 * (CONFIG_ESP_COEX_SW_COEXIST_ENABLE) needs Bluetooth and is off, so its
 * library (libcoexist) is not linked.
 */
#define CONFIG_ESP_COEX_ENABLED                 1

/* ---- lwIP (esp-lwip with ESP-IDF's port, lwipopts.h) ------------------- */

/*
 * ESP-IDF's defaults (components/lwip/Kconfig), with these differences:
 *   - IPv4 only (CONFIG_LWIP_IPV6 off; ESP-IDF: on);
 *   - no DHCP server (CONFIG_LWIP_DHCPS off; ESP-IDF: on), no SoftAP here;
 * and, as in ESP-IDF's defaults, no PPP, SLIP, NAPT, AutoIP, SNTP or core
 * locking.  The tcpip thread runs at FreeRTOS priority 18 (ThreadX 13)
 * with 3072 + 512 bytes of stack (esp_task.h).  The DHCP client checks an
 * offered address with ARP before it takes it.  Only the symbols the
 * compiled sources test are here: lwipopts.h, ESP-IDF's port, esp_task.h.
 * CONFIG_ESP_NETIF_TCPIP_LWIP says that lwIP is the TCP/IP stack
 * (wifi_init.c logs lwIP's configuration with it); esp_netif itself is not
 * built (port/net/wlanif.c is the network interface).
 */
#define CONFIG_ESP_NETIF_TCPIP_LWIP             1
#define CONFIG_LWIP_IPV4                        1
#define CONFIG_LWIP_TCPIP_TASK_PRIO             18
#define CONFIG_LWIP_TCPIP_TASK_STACK_SIZE       3072
#define CONFIG_LWIP_TCPIP_TASK_AFFINITY         0x7FFFFFFF
#define CONFIG_LWIP_TCPIP_RECVMBOX_SIZE         32
#define CONFIG_LWIP_DNS_SUPPORT_MDNS_QUERIES    1
#define CONFIG_LWIP_DNS_MAX_HOST_IP             1
#define CONFIG_LWIP_DNS_MAX_SERVERS             3
#define CONFIG_LWIP_TIMERS_ONDEMAND             1
#define CONFIG_LWIP_MAX_SOCKETS                 10
#define CONFIG_LWIP_SO_REUSE                    1
#define CONFIG_LWIP_SO_REUSE_RXTOALL            1
#define CONFIG_LWIP_IP_DEFAULT_TTL              64
#define CONFIG_LWIP_IP4_FRAG                    1
#define CONFIG_LWIP_IP_REASS_MAX_PBUFS          10
#define CONFIG_LWIP_ESP_GRATUITOUS_ARP          1
#define CONFIG_LWIP_GARP_TMR_INTERVAL           60
#define CONFIG_LWIP_DHCP_DOES_ARP_CHECK         1
#define CONFIG_LWIP_DHCP_DISABLE_VENDOR_CLASS_ID 1
#define CONFIG_LWIP_DHCP_OPTIONS_LEN            69
#define CONFIG_LWIP_DHCP_COARSE_TIMER_SECS      1
#define CONFIG_LWIP_NUM_NETIF_CLIENT_DATA       0
#define CONFIG_LWIP_NETIF_LOOPBACK              1
#define CONFIG_LWIP_LOOPBACK_MAX_PBUFS          8
#define CONFIG_LWIP_MAX_ACTIVE_TCP              16
#define CONFIG_LWIP_MAX_LISTENING_TCP           16
#define CONFIG_LWIP_TCP_MAXRTX                  12
#define CONFIG_LWIP_TCP_SYNMAXRTX               12
#define CONFIG_LWIP_TCP_MSS                     1440
#define CONFIG_LWIP_TCP_TMR_INTERVAL            250
#define CONFIG_LWIP_TCP_MSL                     60000
#define CONFIG_LWIP_TCP_FIN_WAIT_TIMEOUT        20000
#define CONFIG_LWIP_TCP_SND_BUF_DEFAULT         5760
#define CONFIG_LWIP_TCP_WND_DEFAULT             5760
#define CONFIG_LWIP_TCP_RECVMBOX_SIZE           6
#define CONFIG_LWIP_TCP_ACCEPTMBOX_SIZE         6
#define CONFIG_LWIP_TCP_QUEUE_OOSEQ             1
#define CONFIG_LWIP_TCP_OOSEQ_TIMEOUT           6
#define CONFIG_LWIP_TCP_OOSEQ_MAX_PBUFS         4
#define CONFIG_LWIP_TCP_OVERSIZE_MSS            1
#define CONFIG_LWIP_TCP_RTO_TIME                1500
#define CONFIG_LWIP_MAX_UDP_PCBS                16
#define CONFIG_LWIP_UDP_RECVMBOX_SIZE           6
#define CONFIG_LWIP_CHECKSUM_CHECK_ICMP         1
#define CONFIG_LWIP_ICMP                        1
#define CONFIG_LWIP_MAX_RAW_PCBS                16
#define CONFIG_LWIP_ESP_LWIP_ASSERT             1
#define CONFIG_LWIP_HOOK_TCP_ISN_DEFAULT        1
#define CONFIG_LWIP_HOOK_DHCP_EXTRA_OPTION_NONE 1
#define CONFIG_LWIP_HOOK_NETCONN_EXT_RESOLVE_NONE 1

/* ---- mbedTLS (the supplicant's crypto) ---------------------------------- */

/*
 * Read by mbedtls/port/include/mbedtls/esp_config.h.  The PSA crypto core
 * with the software drivers of what the supplicant uses for WPA2-PSK,
 * WPA3-SAE (with hash-to-element) and OWE: AES (CBC, CTR, CCM, GCM, CMAC),
 * SHA-1, SHA-256, SHA-384, SHA-512, MD5 (the ROM's, ESP-IDF's default),
 * PBKDF2, bignum and elliptic curves (ECDH, ECDSA) on the NIST curves P-256,
 * P-384 and P-521 (SAE groups 19 to 21), ASN.1 and keys (PK) for the curves'
 * public keys, HMAC-DRBG for deterministic ECDSA and CTR-DRBG for the random
 * generator, time for its timing; RSA with PKCS#1 v1.5 and v2.1, which the
 * PSA core's default algorithm set requires (and the supplicant's RSA
 * wrappers use).  Allocation goes to ESP-IDF's heap, internal RAM
 * (esp_mem.c, ESP-IDF's default).
 *
 * Different from ESP-IDF: no hardware acceleration (AES, SHA, MPI, ECC;
 * ESP-IDF's default uses the peripherals, through drivers that need
 * GDMA and the crypto locks), no TLS, no file system, no self tests, and
 * no threading (CONFIG_MBEDTLS_THREADING_C: ESP-IDF's is on pthread
 * mutexes, which are not here): the supplicant, in the Wi-Fi driver's
 * task, is the only user of mbedTLS (lwIP uses none of it without PPP),
 * and a second thread that calls mbedTLS (TLS, for one) needs it on
 * first.  X.509 certificate parsing is configured
 * only for the declarations the supplicant's RSA wrappers
 * (crypto_mbedtls-rsa.c) compile against: mbedTLS's X.509 and TLS
 * library is not built, and nothing reaches those wrappers without
 * enterprise authentication.
 */
#define CONFIG_MBEDTLS_HAVE_TIME                1
#define CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC       1
#define CONFIG_MBEDTLS_AES_C                    1
#define CONFIG_MBEDTLS_AES_ROM_TABLES           1
#define CONFIG_MBEDTLS_CIPHER_MODE_CBC          1
#define CONFIG_MBEDTLS_CIPHER_MODE_CTR          1
#define CONFIG_MBEDTLS_CCM_C                    1
#define CONFIG_MBEDTLS_GCM_C                    1
#define CONFIG_MBEDTLS_CMAC_C                   1
#define CONFIG_MBEDTLS_MD_C                     1
#define CONFIG_MBEDTLS_MD5_C                    1
#define CONFIG_MBEDTLS_ROM_MD5                  1
#define CONFIG_MBEDTLS_SHA1_C                   1
#define CONFIG_MBEDTLS_SHA256_C                 1
#define CONFIG_MBEDTLS_SHA384_C                 1
#define CONFIG_MBEDTLS_SHA512_C                 1
#define CONFIG_MBEDTLS_PKCS5_C                  1
#define CONFIG_MBEDTLS_ECP_C                    1
#define CONFIG_MBEDTLS_ECP_NIST_OPTIM           1
#define CONFIG_MBEDTLS_ECDH_C                   1
#define CONFIG_MBEDTLS_ECDSA_C                  1
#define CONFIG_MBEDTLS_ECDSA_DETERMINISTIC      1
#define CONFIG_MBEDTLS_ECP_DP_SECP256R1_ENABLED 1
#define CONFIG_MBEDTLS_ECP_DP_SECP384R1_ENABLED 1
#define CONFIG_MBEDTLS_ECP_DP_SECP521R1_ENABLED 1
#define CONFIG_MBEDTLS_ASN1_PARSE_C             1
#define CONFIG_MBEDTLS_ASN1_WRITE_C             1
#define CONFIG_MBEDTLS_PK_C                     1
#define CONFIG_MBEDTLS_PK_PARSE_C               1
#define CONFIG_MBEDTLS_PK_WRITE_C               1
#define CONFIG_MBEDTLS_PK_PARSE_EC_EXTENDED     1
#define CONFIG_MBEDTLS_PK_PARSE_EC_COMPRESSED   1
#define CONFIG_MBEDTLS_RSA_C                    1
#define CONFIG_MBEDTLS_PKCS1_V15                1
#define CONFIG_MBEDTLS_PKCS1_V21                1
#define CONFIG_MBEDTLS_X509_USE_C               1
#define CONFIG_MBEDTLS_X509_CRT_PARSE_C         1
#define CONFIG_MBEDTLS_CTR_DRBG_C               1
#define CONFIG_MBEDTLS_HMAC_DRBG_C              1

#endif
