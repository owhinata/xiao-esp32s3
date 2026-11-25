cmake_minimum_required(VERSION 3.22)

if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/cmake/xtensa-esp32s3-toolchain.cmake")
endif()

project(xiao_esp32s3_threadx C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Upstream sources.  Fetched into lib/ (not tracked) at exactly these commits.
#   ThreadX          tag v6.5.2.202603_rel
#   esp-hal-3rdparty branch sync/master.c, an unmodified mirror of ESP-IDF
#                    components at ESP-IDF master ad20ded6 (2026-11-15, v6.3
#                    development); the toolchain version in cmake/ goes with it
include("${CMAKE_CURRENT_LIST_DIR}/cmake/fetch_upstream.cmake")
fetch_upstream(threadx
    https://github.com/eclipse-threadx/threadx
    93387b0a60382a29579bbad5045c68328e3751df)
fetch_upstream(esp-hal-3rdparty
    https://github.com/espressif/esp-hal-3rdparty
    130cda986be9d69bf53c28ec1f815479dcdbf5db)
# The second-stage bootloader's image loader (esp_image_verify), its version
# record (esp_bootloader_format) and the flash command definitions of esp_mspi
# are not in the mirror.  They are taken from ESP-IDF itself, at the commit
# the mirror is a copy of (only these directories are downloaded).
fetch_upstream(esp-idf
    https://github.com/espressif/esp-idf
    ad20ded62a2d67a26e501fdc1905f43ba7ad6c81
    SPARSE
    components/esp_image_verify
    components/esp_bootloader_format
    components/esp_mspi/include
    components/esp_mspi/mspi_timing_tuning/include
    components/esp_mspi/esp_mspi_align
    components/esp_sleep/include
    components/nvs_flash/include
    components/esp_partition/include
    components/esp_netif/include)
# Git submodules of ESP-IDF that the mirror records only as commits (empty
# directories), fetched at those commits: the TLSF allocator of the heap
# component, and the PHY and Wi-Fi binary libraries (this chip's directory).
fetch_upstream(tlsf
    https://github.com/espressif/tlsf
    2867f6883a12920b1969ff9624c0ab0e4185c2ce)
fetch_upstream(esp-phy-lib
    https://github.com/espressif/esp-phy-lib
    11d2c35733d6ec14a5653930a4cd948a75cd2fde
    SPARSE esp32s3)
fetch_upstream(esp32-wifi-lib
    https://github.com/espressif/esp32-wifi-lib
    07e8d017cd5ed6a9d9fd7463fe254f0820a526da
    SPARSE esp32s3)

set(ESP_HAL  "${CMAKE_CURRENT_LIST_DIR}/lib/esp-hal-3rdparty/components")
set(ESP_IDF  "${CMAKE_CURRENT_LIST_DIR}/lib/esp-idf/components")
set(TX_DIR   "${CMAKE_CURRENT_LIST_DIR}/lib/threadx")
set(TLSF_DIR "${CMAKE_CURRENT_LIST_DIR}/lib/tlsf")
set(ESP_PHY_LIB "${CMAKE_CURRENT_LIST_DIR}/lib/esp-phy-lib/esp32s3")
set(TX_PORT  "${TX_DIR}/ports/xtensa/xcc")
set(PORT_DIR "${CMAKE_CURRENT_LIST_DIR}/port")

# ESP-IDF's version string, as the images record it (esp_app_desc_t.idf_ver,
# esp_bootloader_desc_t.idf_ver, the bootloader's banner).  ESP-IDF takes it
# from "git describe" of its own tree, which a depth-1 snapshot cannot answer;
# this is the version esp_idf_version.h names (6.3, in development) and the
# ESP-IDF commit of both snapshots.
set(IDF_VER "v6.3-dev-gad20ded6")

# -mlongcalls: code and ROM are further apart than a direct call reaches.
# -fstrict-volatile-bitfields: peripheral registers only answer 32-bit
#   accesses.  The HAL describes them as volatile bit-field structs, and
#   without this option GCC reads a 16-bit field with a 16-bit load, which
#   returns 0 (seen with the eFuse MAC).  ESP-IDF builds everything with it.
add_compile_options(
    -mlongcalls
    -fstrict-volatile-bitfields
    -ffunction-sections -fdata-sections
    -fno-common
    -Wall)

# ---------------------------------------------------------------------------
# esp-hal-3rdparty: headers, ROM/peripheral symbol tables, Xtensa HAL library
# ---------------------------------------------------------------------------
# ESP-IDF splits the HAL into one component per peripheral (esp_hal_*); each
# has a common include/ and a per-chip esp32s3/include/.  Only the ones whose
# headers this firmware reaches are listed.
set(ESP_HAL_PERIPHERALS
    esp_hal_ana_conv esp_hal_cache esp_hal_clock esp_hal_dma esp_hal_gpio esp_hal_gpspi
    esp_hal_regi2c esp_hal_rtc_timer esp_hal_systimer esp_hal_timg esp_hal_uart
    esp_hal_usb esp_hal_wdt)
set(ESP_HAL_PERIPHERAL_INCLUDES)
foreach(_c IN LISTS ESP_HAL_PERIPHERALS)
    foreach(_d "${ESP_HAL}/${_c}/include" "${ESP_HAL}/${_c}/esp32s3/include")
        if(EXISTS "${_d}")
            list(APPEND ESP_HAL_PERIPHERAL_INCLUDES "${_d}")
        endif()
    endforeach()
endforeach()

add_library(esp_hal INTERFACE)
target_include_directories(esp_hal INTERFACE
    "${PORT_DIR}/include"                       # sdkconfig.h
    "${ESP_HAL}/xtensa/esp32s3/include"
    "${ESP_HAL}/xtensa/include"
    "${ESP_HAL}/soc/include"
    "${ESP_HAL}/soc/esp32s3/include"
    "${ESP_HAL}/soc/esp32s3/register"
    "${ESP_HAL}/hal/platform_port/include"
    "${ESP_HAL}/hal/esp32s3/include"
    "${ESP_HAL}/hal/include"
    ${ESP_HAL_PERIPHERAL_INCLUDES}
    "${ESP_HAL}/esp_common/include"
    "${ESP_HAL}/esp_rom/include"
    "${ESP_HAL}/esp_rom/esp32s3"
    "${ESP_HAL}/esp_rom/esp32s3/include"
    "${ESP_HAL}/esp_rom/esp32s3/include/esp32s3"
    "${ESP_HAL}/esp_hw_support/include"
    "${ESP_HAL}/esp_hw_support/include/soc"
    "${ESP_HAL}/esp_hw_support/include/soc/esp32s3"
    "${ESP_HAL}/esp_hw_support/include/esp_private"
    "${ESP_HAL}/esp_hw_support/port/include"
    "${ESP_HAL}/esp_hw_support/port/esp32s3/include"
    "${ESP_HAL}/esp_system/include"
    "${ESP_HAL}/log/include"
    "${ESP_HAL}/heap/include"
    # Components that are not in esp-hal-3rdparty: headers from ESP-IDF
    # (lib/esp-idf): esp_sleep's (rtc_clk.c) and esp_mspi's (rtc_init.c,
    # heap_align_hw.c).
    "${ESP_IDF}/esp_sleep/include"
    "${ESP_IDF}/esp_mspi/mspi_timing_tuning/include"
    "${ESP_IDF}/esp_mspi/esp_mspi_align/include")
target_link_libraries(esp_hal INTERFACE "${ESP_HAL}/xtensa/esp32s3/libxt_hal.a")
target_link_options(esp_hal INTERFACE
    "-T${ESP_HAL}/soc/esp32s3/ld/esp32s3.peripherals.ld"
    "-T${ESP_HAL}/esp_rom/esp32s3/ld/esp32s3.rom.ld"
    "-T${ESP_HAL}/esp_rom/esp32s3/ld/esp32s3.rom.api.ld"
    # The analog I2C (regi2c) accessors are in ROM (ESP_ROM_HAS_REGI2C_IMPL).
    "-T${ESP_HAL}/esp_hal_regi2c/esp32s3/rom.regi2c.ld"
    # memcpy/memset/str* and the libgcc helpers in ROM: always reachable, even
    # with the cache disabled, and they cost no flash or RAM.
    # The libc list also names three things that belong to the ROM's own C
    # library state (sbrk, syscall_table_ptr, _global_impure_ptr), which this
    # firmware does not initialize: nothing may use them.
    "-T${ESP_HAL}/esp_rom/esp32s3/ld/esp32s3.rom.libc.ld"
    "-T${ESP_HAL}/esp_rom/esp32s3/ld/esp32s3.rom.libgcc.ld")

# ---------------------------------------------------------------------------
# Xtensa RTOS porting layer from esp-hal-3rdparty
# ---------------------------------------------------------------------------
# Vectors, interrupt dispatch, context save/restore and the coprocessor
# exception, shared by every RTOS on Espressif Xtensa chips.  It reaches the
# RTOS through "xtensa_rtos.h", supplied by port/threadx/inc.  _xt_panic comes
# from esp_system, and so does xt_highint4, the level-4 interrupt handler that
# takes the interrupt watchdog to panicHandler() (it replaces the layer's weak
# default).  The sources are used unmodified.
#
# portNUM_PROCESSORS sizes the layer's per-core tables; two of its files take
# it from the build rather than from xtensa_rtos.h.
add_library(xtensa_rtos OBJECT
    "${ESP_HAL}/xtensa/xtensa_context.S"
    "${ESP_HAL}/xtensa/xtensa_intr_asm.S"
    "${ESP_HAL}/xtensa/xtensa_intr.c"
    "${ESP_HAL}/xtensa/xtensa_vectors.S"
    "${ESP_HAL}/esp_system/port/arch/xtensa/panic_handler_asm.S"
    "${ESP_HAL}/esp_system/port/soc/esp32s3/highint_hdl.S")
target_include_directories(xtensa_rtos PUBLIC "${PORT_DIR}/threadx/inc")
target_compile_definitions(xtensa_rtos PUBLIC
    portNUM_PROCESSORS=1
    TX_INCLUDE_USER_DEFINE_FILE)
target_link_libraries(xtensa_rtos PUBLIC esp_hal)

# ---------------------------------------------------------------------------
# ThreadX kernel + port
# ---------------------------------------------------------------------------
# Kernel sources come from the submodule.  The architecture port is
# port/threadx: the ThreadX-specific half of upstream ports/xtensa/xcc,
# adapted to the layer above.  Options live in port/threadx/inc/tx_user.h.
file(GLOB TX_COMMON_SOURCES CONFIGURE_DEPENDS "${TX_DIR}/common/src/*.c")

add_library(threadx STATIC
    ${TX_COMMON_SOURCES}
    "${PORT_DIR}/threadx/src/tx_initialize_low_level.c"
    "${PORT_DIR}/threadx/src/tx_thread_context_restore.S"
    "${PORT_DIR}/threadx/src/tx_thread_context_save.S"
    "${PORT_DIR}/threadx/src/tx_thread_interrupt_control.c"
    "${PORT_DIR}/threadx/src/tx_thread_schedule.S"
    "${PORT_DIR}/threadx/src/tx_thread_stack_build.S"
    "${PORT_DIR}/threadx/src/tx_thread_system_return.S"
    "${PORT_DIR}/threadx/src/tx_timer_interrupt.S"
    "${PORT_DIR}/threadx/src/tx_port_asm.S"
    "${PORT_DIR}/threadx/src/tx_port_offset_check.c"
    "${PORT_DIR}/threadx/src/tx_clib_newlib.c"
    # Execution profile kit, unmodified: the run times of "ps" (tx_user.h).
    "${TX_DIR}/utility/execution_profile_kit/tx_execution_profile.c")
target_include_directories(threadx PUBLIC
    "${TX_DIR}/common/inc"
    "${TX_DIR}/utility/execution_profile_kit")
target_link_libraries(threadx PUBLIC xtensa_rtos)

# ---------------------------------------------------------------------------
# FreeRTOS API on ThreadX
# ---------------------------------------------------------------------------
# ThreadX's FreeRTOS compatibility layer, unmodified, configured by
# port/esp_compat/freertos/include/FreeRTOSConfig.h, and what ESP-IDF's
# FreeRTOS adds to the API (port/esp_compat/freertos): "freertos/FreeRTOS.h"
# and the other "freertos/" headers ESP-IDF sources include.  Only the
# targets that link this library see those headers; the Xtensa layer above
# must not (see freertos/FreeRTOS.h), and it does not link it.
set(TX_FREERTOS "${TX_DIR}/utility/rtos_compatibility_layers/FreeRTOS")
add_library(freertos STATIC
    "${TX_FREERTOS}/tx_freertos.c"
    "${PORT_DIR}/esp_compat/freertos/freertos_idf.c")
target_include_directories(freertos PUBLIC
    "${PORT_DIR}/esp_compat/freertos/include"
    "${TX_FREERTOS}")
target_link_libraries(freertos PUBLIC threadx esp_hal)
# pcTaskGetName() hands a "const char **" where tx_thread_info_get() takes a
# "CHAR **"; only the constness differs.
set_source_files_properties("${TX_FREERTOS}/tx_freertos.c"
    PROPERTIES COMPILE_OPTIONS "-Wno-incompatible-pointer-types")

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
set(LDSCRIPT "${CMAKE_CURRENT_LIST_DIR}/ldscript/xiao_esp32s3.ld")

add_executable(app
    src/main.c
    "${PORT_DIR}/startup.c"
    "${PORT_DIR}/board.c"
    "${PORT_DIR}/panic.c"
    "${PORT_DIR}/watchdog.c"
    "${PORT_DIR}/syscalls.c"
    "${PORT_DIR}/esp_log_ram.c"
    "${PORT_DIR}/net/wifi.c"
    "${PORT_DIR}/esp_compat/esp_sleep.c"
    "${PORT_DIR}/esp_compat/mspi_timing.c")

# ---------------------------------------------------------------------------
# Espressif sources this image needs, compiled as they are
# ---------------------------------------------------------------------------
# From esp-hal-3rdparty, and from ESP-IDF and its TLSF (lib/esp-idf,
# lib/tlsf) what the mirror does not carry.  An object library: its objects are linked as they are, nothing is dropped
# for being unreferenced before --gc-sections, so every ESP-IDF start-up hook
# they define (ESP_SYSTEM_INIT_FN() and friends, kept by the linker script)
# runs.  These sources are built as in ESP-IDF, with esp_libc's
# platform_include ahead of the C library's headers (ESP-IDF's _lock_t and
# _lock_*(), POSIX additions); this repository's own sources are not.
add_library(esp_idf OBJECT
    # The application description the second-stage bootloader reads from the
    # start of the read-only data (esp_app_desc_t; see the linker script).
    "${ESP_HAL}/esp_app_format/esp_app_desc.c"
    "${ESP_HAL}/esp_hal_wdt/wdt_hal_iram.c"
    "${ESP_HAL}/esp_rom/patches/esp_rom_sys.c"
    # Work-arounds for bugs in the ROM's cache suspend/write-back routines.
    "${ESP_HAL}/esp_rom/patches/esp_rom_cache_esp32s2_esp32s3.c"
    # Clocks and the RTC domain as ESP-IDF's start-up sets them up
    # (esp_rtc_init(), esp_clk_init()): regulators, RTC slow clock and its
    # calibration, CPU clock, the SAR peripherals' default state.
    "${ESP_HAL}/esp_system/port/soc/esp32s3/clk.c"
    "${ESP_HAL}/esp_hw_support/esp_clk.c"
    "${ESP_HAL}/esp_hw_support/port/esp32s3/rtc_clk.c"
    "${ESP_HAL}/esp_hw_support/port/esp32s3/rtc_init.c"
    "${ESP_HAL}/esp_hw_support/port/esp32s3/rtc_time.c"
    "${ESP_HAL}/esp_hw_support/regi2c_ctrl.c"
    "${ESP_HAL}/esp_hw_support/port/esp32s3/rtc_sleep.c"         # rtc_sleep_pu()
    "${ESP_HAL}/esp_hw_support/port/esp32s3/sar_periph_ctrl.c"
    "${ESP_HAL}/esp_hw_support/sar_periph_ctrl_common.c"
    # The temperature sensor, which the PHY reads (phy_override.c).
    "${ESP_HAL}/esp_hw_support/sar_tsens_ctrl.c"
    "${ESP_HAL}/esp_hw_support/adc_share_hw_ctrl.c"
    "${ESP_HAL}/esp_hal_ana_conv/temperature_sensor_hal.c"
    "${ESP_HAL}/esp_hal_ana_conv/esp32s3/temperature_sensor_periph.c"
    # The heap: heap_caps on TLSF over the regions of memory_layout.c, and
    # esp_libc's malloc() family on it (in place of newlib's).  The alignment
    # of DMA-capable allocations comes from the cache driver.
    "${ESP_HAL}/heap/heap_caps.c"
    "${ESP_HAL}/heap/heap_caps_base.c"
    "${ESP_HAL}/heap/heap_caps_init.c"
    "${ESP_HAL}/heap/multi_heap.c"
    "${ESP_HAL}/heap/port/memory_layout_utils.c"
    "${ESP_HAL}/heap/port/esp32s3/memory_layout.c"
    "${TLSF_DIR}/tlsf.c"
    "${ESP_HAL}/esp_hw_support/heap_align_hw.c"
    "${ESP_IDF}/esp_mspi/esp_mspi_align/esp_mspi_align.c"
    "${ESP_HAL}/esp_hw_support/esp_memory_utils.c"
    "${ESP_HAL}/esp_mm/esp_cache_msync.c"
    "${ESP_HAL}/esp_hal_cache/cache_hal.c"
    "${ESP_HAL}/esp_libc/src/heap.c"
    # ESP-IDF's log, version 1 (sdkconfig.h): the parts its ESP_LOGx() and
    # esp_log_write() reach; port/esp_log_ram.c is its output.
    "${ESP_HAL}/log/src/log.c"
    "${ESP_HAL}/log/src/os/log_timestamp.c"
    "${ESP_HAL}/log/src/os/log_write.c"
    "${ESP_HAL}/log/src/log_level/log_level.c"
    # esp_timer on SYSTIMER: timers, and the uptime clock (port/board.c).
    "${ESP_HAL}/esp_timer/src/esp_timer.c"
    "${ESP_HAL}/esp_timer/src/esp_timer_impl_common.c"
    "${ESP_HAL}/esp_timer/src/esp_timer_impl_systimer.c"
    "${ESP_HAL}/esp_timer/src/esp_timer_init.c"
    "${ESP_HAL}/esp_hal_systimer/systimer_hal.c"
    "${ESP_HAL}/esp_hw_support/port/esp32s3/systimer.c"
    # The interrupt allocator (esp_intr_alloc()) with the table of the CPU's
    # interrupts, and the reference-counted peripheral clock gates.
    "${ESP_HAL}/esp_hw_support/intr_alloc.c"
    "${ESP_HAL}/esp_hw_support/port/esp32s3/esp_cpu_intr.c"
    "${ESP_HAL}/soc/esp32s3/interrupts.c"                       # names in its messages
    "${ESP_HAL}/esp_hw_support/periph_ctrl.c"
    # eFuse: the chip's package (the PHY's init) and whether flash encryption
    # is on (the alignment of DMA-capable allocations).
    "${ESP_HAL}/efuse/src/esp_efuse_fields.c"
    "${ESP_HAL}/hal/efuse_hal.c"
    # The PHY: ESP-IDF's esp_phy component and the binary libraries below.
    "${ESP_HAL}/esp_phy/src/phy_init.c"
    "${ESP_HAL}/esp_phy/src/phy_common.c"
    "${ESP_HAL}/esp_phy/src/phy_override.c"
    "${ESP_HAL}/esp_phy/src/lib_printf.c"
    "${ESP_HAL}/esp_phy/esp32s3/phy_init_data.c"
    # UART0: the console on D6/D7 (port/board.c), and the table of IO MUX
    # registers that gpio_ll_pullup_en() looks the RX pin up in.
    "${ESP_HAL}/esp_hal_uart/uart_hal.c"
    "${ESP_HAL}/soc/esp32s3/gpio_periph.c")
target_include_directories(esp_idf PRIVATE
    "${ESP_HAL}/esp_libc/platform_include"
    "${ESP_HAL}/esp_timer/include"
    "${ESP_HAL}/esp_hw_support/etm/include"
    "${ESP_HAL}/esp_phy/include"
    "${ESP_HAL}/esp_phy/esp32s3/include"
    "${ESP_HAL}/efuse/include"
    "${ESP_HAL}/efuse/esp32s3/include"
    "${ESP_HAL}/esp_mm/include"
    "${ESP_HAL}/esp_system/port/include")
target_link_libraries(esp_idf PRIVATE freertos threadx esp_hal)
set_source_files_properties(
    "${ESP_HAL}/esp_timer/src/esp_timer.c"
    "${ESP_HAL}/esp_timer/src/esp_timer_impl_common.c"
    "${ESP_HAL}/esp_timer/src/esp_timer_impl_systimer.c"
    "${ESP_HAL}/esp_timer/src/esp_timer_init.c"
    PROPERTIES INCLUDE_DIRECTORIES "${ESP_HAL}/esp_timer/private_include")
set_source_files_properties(
    "${ESP_HAL}/heap/heap_caps.c"
    "${ESP_HAL}/heap/heap_caps_base.c"
    "${ESP_HAL}/heap/heap_caps_init.c"
    "${ESP_HAL}/heap/multi_heap.c"
    "${ESP_HAL}/heap/port/memory_layout_utils.c"
    "${ESP_HAL}/heap/port/esp32s3/memory_layout.c"
    "${TLSF_DIR}/tlsf.c"
    PROPERTIES
        INCLUDE_DIRECTORIES "${ESP_HAL}/heap/private_include;${ESP_HAL}/heap;${TLSF_DIR};${TLSF_DIR}/include"
        COMPILE_DEFINITIONS "MULTI_HEAP_FREERTOS")
set_source_files_properties(
    "${ESP_HAL}/log/src/log.c"
    "${ESP_HAL}/log/src/os/log_timestamp.c"
    "${ESP_HAL}/log/src/os/log_write.c"
    "${ESP_HAL}/log/src/log_level/log_level.c"
    PROPERTIES INCLUDE_DIRECTORIES "${ESP_HAL}/log/include/esp_private")
set_source_files_properties("${ESP_HAL}/efuse/src/esp_efuse_fields.c"
    PROPERTIES INCLUDE_DIRECTORIES "${ESP_HAL}/efuse/private_include;${ESP_HAL}/efuse/esp32s3/private_include")
# The PHY's sources include headers of components they are built with in
# ESP-IDF but that are not built here (nvs_flash and esp_partition, which
# needs esp_blockdev's, esp_wifi's private wifi.h with esp_event's and
# esp_netif's, the GPIO driver's types).
set_source_files_properties(
    "${ESP_HAL}/esp_phy/src/phy_init.c"
    "${ESP_HAL}/esp_phy/src/phy_common.c"
    "${ESP_HAL}/esp_phy/src/phy_override.c"
    "${ESP_HAL}/esp_phy/src/lib_printf.c"
    "${ESP_HAL}/esp_phy/esp32s3/phy_init_data.c"
    PROPERTIES INCLUDE_DIRECTORIES
        "${ESP_IDF}/nvs_flash/include;${ESP_IDF}/esp_partition/include;${ESP_HAL}/esp_blockdev/include;${ESP_HAL}/esp_wifi/include;${ESP_HAL}/esp_event/include;${ESP_IDF}/esp_netif/include;${ESP_HAL}/upper_hal_gpio/include")
# clk.c includes the second-stage bootloader's clock header for a declaration.
set_source_files_properties("${ESP_HAL}/esp_system/port/soc/esp32s3/clk.c"
    PROPERTIES INCLUDE_DIRECTORIES "${ESP_HAL}/bootloader_support/include")
# What ESP-IDF's esp_app_format component gives esp_app_desc.c: the project's
# name and version (ESP-IDF takes the version from "git describe" of the
# project, falling back to "1") and ESP-IDF's version.
set(PROJECT_VER "1")
execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_LIST_DIR}" describe --always --tags --dirty
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _ver ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
if(_rc EQUAL 0 AND _ver)
    string(SUBSTRING "${_ver}" 0 31 PROJECT_VER)
endif()
set_source_files_properties("${ESP_HAL}/esp_app_format/esp_app_desc.c" PROPERTIES
    INCLUDE_DIRECTORIES "${ESP_HAL}/esp_app_format/include"
    COMPILE_DEFINITIONS "PROJECT_VER=\"${PROJECT_VER}\";PROJECT_NAME=\"${PROJECT_NAME}\";IDF_VER_DEF=\"${IDF_VER}\"")

# The PHY's binary libraries (lib/esp-phy-lib): libphy, and libbtbb, which
# ESP-IDF links on chips with Bluetooth (libphy's debug code calls into it).
add_library(esp_phy_blobs INTERFACE)
target_link_libraries(esp_phy_blobs INTERFACE
    "${ESP_PHY_LIB}/libphy.a"
    "${ESP_PHY_LIB}/libbtbb.a")

set_target_properties(app PROPERTIES
    OUTPUT_NAME xiao_esp32s3_threadx
    SUFFIX      .elf
    LINK_DEPENDS "${LDSCRIPT}")
target_include_directories(app PRIVATE
    "${PORT_DIR}"
    "${ESP_HAL}/esp_timer/include"
    "${ESP_HAL}/esp_hw_support/etm/include"     # esp_timer.h includes esp_etm.h
    "${ESP_HAL}/esp_phy/include"
    "${ESP_HAL}/esp_system/port/include")      # esp_clk_internal.h
# xtensa_rtos is an object library: the vectors are reached only through the
# linker script and VECBASE, so nothing would pull them out of an archive.
target_link_libraries(app PRIVATE shell esp_idf freertos threadx xtensa_rtos esp_phy_blobs esp_hal)
# -u esp_app_desc: nothing refers to it by name (esp_app_format does the same).
# heap.c's malloc() and phy_override.c's functions take precedence over
# newlib's and libphy's because they are linked as objects (esp_idf), before
# any archive is searched: ESP-IDF's "-u" options for them are not needed.
# --orphan-handling=warn: every input section has a place in the linker
# script (an orphan would end up wherever ld puts it).
target_link_options(app PRIVATE
    -nostartfiles
    "-T${LDSCRIPT}"
    -Wl,-u,esp_app_desc
    -Wl,--orphan-handling=warn
    -Wl,--gc-sections
    "-Wl,-Map=$<TARGET_FILE_DIR:app>/xiao_esp32s3_threadx.map")

# ---------------------------------------------------------------------------
# Shell
# ---------------------------------------------------------------------------
# shell/common is a copy of the part of rp2040-connect's common/ that the
# shell needs, at the same relative paths (shell/ORIGIN.md lists the files and
# says which differ from the original).  shell/port is what that code expects
# a board to write.
#
# An object library: the commands are reached only through the table in
# shell/port/commands.c, and the tick hook only through a weak reference.
set(SHELL_COMMON "${CMAKE_CURRENT_LIST_DIR}/shell/common")
set(SHELL_PORT   "${CMAKE_CURRENT_LIST_DIR}/shell/port")

add_library(shell OBJECT
    # The Zephyr-compatibility kernel layer on ThreadX
    "${SHELL_COMMON}/compat/kernel_generic/zephyr_kernel.c"
    "${SHELL_COMMON}/compat/kernel_generic/zephyr_mutex.c"
    "${SHELL_COMMON}/compat/port/threadx/zephyr_kernel_os.c"
    # RAM log
    "${SHELL_COMMON}/compat/log/log_ram.c"
    "${SHELL_COMMON}/compat/log/log_ring.c"
    # Shell engine
    "${SHELL_COMMON}/compat/shell_engine/cli.c"
    "${SHELL_COMMON}/compat/shell_engine/getopt_state.c"
    "${SHELL_COMMON}/compat/shell_engine/shell_argv.c"
    "${SHELL_COMMON}/compat/shell_engine/shell_console.c"
    "${SHELL_COMMON}/compat/shell_engine/shell_engine.c"
    "${SHELL_COMMON}/compat/shell_engine/shell_serial.c"
    "${SHELL_COMMON}/shell/zshell.c"
    "${SHELL_COMMON}/shell/subcmd.c"
    "${SHELL_COMMON}/compat/subsys/portability/posix/c_lib_ext/fnmatch.c"
    "${SHELL_COMMON}/compat/lib/utils/getopt/getopt.c"
    "${SHELL_COMMON}/compat/lib/utils/getopt/getopt_long.c"
    # Commands
    "${SHELL_COMMON}/shell/kernel_shell.c"
    "${SHELL_COMMON}/shell/dmesg_shell.c"
    # This board's part
    "${SHELL_PORT}/commands.c"
    "${SHELL_PORT}/fault_record.c"
    "${SHELL_PORT}/shell_port.c"
    "${SHELL_PORT}/zephyr_sys_clock.c")
# The order is rp2040-connect's (common/compat/port/pico_sdk/compat.cmake):
# the board's and the OS's parts first, then the parts for an OS without its
# own model, then the layer's and the shell's headers.
target_include_directories(shell PUBLIC
    "${SHELL_PORT}"
    "${SHELL_COMMON}/compat/port/threadx"
    "${SHELL_COMMON}/compat/port/generic"
    "${SHELL_COMMON}/compat/include"
    "${SHELL_COMMON}/compat/port"
    "${SHELL_COMMON}/compat/internal"
    "${SHELL_COMMON}/shell/include"
    "${SHELL_COMMON}/shell/port"
    "${SHELL_COMMON}/shell/port/generic"
    "${SHELL_COMMON}/compat/lib/utils/getopt"
    "${PORT_DIR}")                              # board.h
# Z_SHELL_CMD_ROOT_GLOBAL: SHELL_CMD_REGISTER() makes a global root entry for
# the table.  CONFIG_GETOPT_LONG changes a structure every command sees.
target_compile_definitions(shell PUBLIC
    Z_SHELL_CMD_ROOT_GLOBAL
    CONFIG_GETOPT_LONG=1)
target_link_libraries(shell PUBLIC threadx)

# ---------------------------------------------------------------------------
# Second-stage bootloader (bootloader/CMakeLists.txt)
# ---------------------------------------------------------------------------
add_subdirectory(bootloader)

# ---------------------------------------------------------------------------
# Images, partition table and download targets (esptool from ./.venv)
# ---------------------------------------------------------------------------
# The standard ESP-IDF flash layout:
#   0x0      bootloader.bin            ROM loads it (CONFIG_BOOTLOADER_OFFSET_IN_FLASH)
#   0x8000   partition-table.bin       from partitions.csv (CONFIG_PARTITION_TABLE_OFFSET)
#   0x10000  xiao_esp32s3_threadx.bin  the "factory" partition; the bootloader loads it
#
# gen_esp32part.py, parttool.py and check_sizes.py are ESP-IDF's
# (components/partition_table).  They need rich_click and esp_pylib, which
# esptool brings into the venv; no other package is needed.
set(ESPTOOL "${CMAKE_CURRENT_LIST_DIR}/.venv/bin/esptool" CACHE FILEPATH "esptool executable")
set(ESP_PYTHON "${CMAKE_CURRENT_LIST_DIR}/.venv/bin/python" CACHE FILEPATH
    "Python with esptool installed, for ESP-IDF's partition table scripts")
set(ESPPORT "" CACHE STRING
    "Serial port for the flash targets (empty = the XIAO's USB Serial/JTAG, found by its USB ID)")
# Through a USB-serial adapter on UART0 the transfer runs at this rate, ESP-IDF's
# default flash baud rate (idf.py, CONFIG_ESPTOOLPY_BAUD); the ROM and the shell
# are reached at the console's rate first.
set(ESPBAUD 460800 CACHE STRING "Baud rate of a flash transfer over a USB-serial adapter")

# The two offsets and the console's baud rate are read from sdkconfig.h, which
# the bootloader is built with.
function(sdkconfig_value name out)
    file(STRINGS "${PORT_DIR}/include/sdkconfig.h" _line REGEX "^#define ${name}[ \t]")
    string(REGEX REPLACE "^#define ${name}[ \t]+([^ \t]+).*" "\\1" _value "${_line}")
    if(NOT _value)
        message(FATAL_ERROR "${name} is not in sdkconfig.h")
    endif()
    set(${out} "${_value}" PARENT_SCOPE)
endfunction()
sdkconfig_value(CONFIG_BOOTLOADER_OFFSET_IN_FLASH BOOTLOADER_OFFSET)
sdkconfig_value(CONFIG_PARTITION_TABLE_OFFSET PARTITION_TABLE_OFFSET)
sdkconfig_value(CONFIG_ESP_CONSOLE_UART_BAUDRATE CONSOLE_BAUD)

# The shell's prompt (src/main.c), which script/flash.py waits for on a UART.
# A change to either file re-runs the configure step that reads these values.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${PORT_DIR}/include/sdkconfig.h" "${CMAKE_CURRENT_LIST_DIR}/src/main.c")
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/src/main.c" _line REGEX "^#define PROMPT[ \t]")
string(REGEX REPLACE "^#define PROMPT[ \t]+\"([^\"]*)\".*" "\\1" SHELL_PROMPT "${_line}")
if(NOT SHELL_PROMPT OR SHELL_PROMPT STREQUAL _line)
    message(FATAL_ERROR "PROMPT is not in src/main.c")
endif()

set(PARTITION_TOOLS "${ESP_HAL}/partition_table")
set(PARTITION_CSV "${CMAKE_CURRENT_LIST_DIR}/partitions.csv")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PARTITION_CSV}")

# Where the bootloader will look for the application (the partition it boots
# by default), as ESP-IDF's partition_table_get_partition_info() finds it.
execute_process(
    COMMAND "${ESP_PYTHON}" "${PARTITION_TOOLS}/parttool.py" -q
            --partition-table-offset ${PARTITION_TABLE_OFFSET}
            --primary-bootloader-offset ${BOOTLOADER_OFFSET}
            --partition-table-file "${PARTITION_CSV}"
            get_partition_info --partition-boot-default --info offset
    RESULT_VARIABLE _rc OUTPUT_VARIABLE APP_OFFSET ERROR_VARIABLE _err
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _rc EQUAL 0 OR NOT APP_OFFSET)
    message(FATAL_ERROR "parttool.py could not read ${PARTITION_CSV}: ${_err}")
endif()

# Flash parameters written into both image headers: 8 MB, DIO, 80 MHz (the
# same as sdkconfig.h).  The bootloader programs the flash interface from its
# own header.  --min-rev-full/--max-rev-full: the chip revisions the images
# accept, ESP-IDF's defaults (CONFIG_ESP_REV_MIN_FULL 0, CONFIG_ESP_REV_MAX_FULL 99).
set(ESPTOOL_FLASH_ARGS --flash-mode dio --flash-freq 80m --flash-size 8MB)
set(ELF2IMAGE_ARGS --chip esp32s3 elf2image ${ESPTOOL_FLASH_ARGS}
    --min-rev-full 0 --max-rev-full 99)

set(BOOTLOADER_BIN "${CMAKE_CURRENT_BINARY_DIR}/bootloader.bin")
set(PARTITION_BIN  "${CMAKE_CURRENT_BINARY_DIR}/partition-table.bin")
set(APP_BIN        "${CMAKE_CURRENT_BINARY_DIR}/xiao_esp32s3_threadx.bin")

# Bootloader image, checked against the space before the partition table.
add_custom_command(OUTPUT "${BOOTLOADER_BIN}"
    COMMAND "${ESPTOOL}" ${ELF2IMAGE_ARGS} -o "${BOOTLOADER_BIN}" "$<TARGET_FILE:bootloader>"
    COMMAND "${ESP_PYTHON}" "${PARTITION_TOOLS}/check_sizes.py" --offset ${PARTITION_TABLE_OFFSET}
            bootloader ${BOOTLOADER_OFFSET} "${BOOTLOADER_BIN}"
    DEPENDS bootloader
    COMMAND_EXPAND_LISTS VERBATIM)

add_custom_command(OUTPUT "${PARTITION_BIN}"
    COMMAND "${ESP_PYTHON}" "${PARTITION_TOOLS}/gen_esp32part.py" -q
            --offset ${PARTITION_TABLE_OFFSET} --primary-bootloader-offset ${BOOTLOADER_OFFSET}
            --flash-size 8MB -- "${PARTITION_CSV}" "${PARTITION_BIN}"
    DEPENDS "${PARTITION_CSV}"
    VERBATIM)

# Application image, as ESP-IDF makes it: the SHA-256 of the image appended
# (esptool's default), the SHA-256 of the ELF file written into
# esp_app_desc_t.app_elf_sha256 (file offset 0xb0: the descriptor starts at
# 0x20, right behind the image and segment headers), the MMU page size
# recorded; then checked against the size of the partition it goes to.
add_custom_command(OUTPUT "${APP_BIN}"
    COMMAND "${CMAKE_SIZE}" -A "$<TARGET_FILE:app>"
    COMMAND "${ESPTOOL}" ${ELF2IMAGE_ARGS} --elf-sha256-offset 0xb0 --flash-mmu-page-size 64KB
            -o "${APP_BIN}" "$<TARGET_FILE:app>"
    COMMAND "${ESP_PYTHON}" "${PARTITION_TOOLS}/check_sizes.py" --offset ${PARTITION_TABLE_OFFSET}
            partition --type app "${PARTITION_BIN}" "${APP_BIN}"
    DEPENDS app "${PARTITION_BIN}"
    COMMAND_EXPAND_LISTS VERBATIM)

add_custom_target(images ALL DEPENDS "${BOOTLOADER_BIN}" "${PARTITION_BIN}" "${APP_BIN}")

# script/flash.py finds the port, gets the chip into the ROM's download mode
# (through USB Serial/JTAG as esptool does, or through the shell's "bootsel"
# on a UART) and runs esptool with the write-flash arguments after "--".
set(FLASH_TOOL "${ESP_PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/script/flash.py"
    --esptool "${ESPTOOL}" --prompt "${SHELL_PROMPT}"
    --console-baud ${CONSOLE_BAUD} --flash-baud ${ESPBAUD})
if(ESPPORT)
    list(APPEND FLASH_TOOL --port "${ESPPORT}")
endif()

# flash: all three images.  This replaces whatever is at the start of the
# flash, the board's previous bootloader included.
add_custom_target(flash
    COMMAND ${FLASH_TOOL} -- ${ESPTOOL_FLASH_ARGS}
            ${BOOTLOADER_OFFSET} "${BOOTLOADER_BIN}"
            ${PARTITION_TABLE_OFFSET} "${PARTITION_BIN}"
            ${APP_OFFSET} "${APP_BIN}"
    DEPENDS images USES_TERMINAL COMMAND_EXPAND_LISTS VERBATIM)

# app-flash: the application only, once the bootloader and the partition
# table are on the board (ESP-IDF's "idf.py app-flash").
add_custom_target(app-flash
    COMMAND ${FLASH_TOOL} -- ${ESPTOOL_FLASH_ARGS}
            ${APP_OFFSET} "${APP_BIN}"
    DEPENDS images USES_TERMINAL COMMAND_EXPAND_LISTS VERBATIM)
