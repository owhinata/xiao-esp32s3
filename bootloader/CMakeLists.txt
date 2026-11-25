# ---------------------------------------------------------------------------
# ESP-IDF's second-stage bootloader, built from unmodified sources
# ---------------------------------------------------------------------------
# What ESP-IDF builds from components/bootloader/subproject: bootloader_start.c
# (call_start_cpu0) and the components it links, with ESP-IDF's own linker
# scripts.  This file only lists sources and include directories and adds no
# code of its own.  For bootloader_support, esp_image_verify, log, esp_rom,
# esp_hw_support, efuse, hal and esp_hal_cache the sources are the ones the
# component's CMakeLists.txt compiles for a bootloader build (BOOTLOADER_BUILD)
# on this chip with this sdkconfig; of the other HAL components only the file
# the link needs is compiled (ESP-IDF builds them whole, and --gc-sections
# drops the rest there too).
#
# esp_image_verify and esp_bootloader_format are not in esp-hal-3rdparty and
# come from ESP-IDF itself (lib/esp-idf, see CMakeLists.txt).
#
# Each component becomes a static library with ESP-IDF's archive name
# (libbootloader_support.a, liblog.a, ...): the bootloader's linker script
# picks the code that must stay in "iram_loader_seg" by archive and object
# name, and that only works with the names ESP-IDF uses.
#
# Everything here shares port/include/sdkconfig.h with the application, as
# the two images do in ESP-IDF: flash geometry, crystal, MMU page size and
# partition table offset must be the same for both.

set(BL_DIR "${ESP_HAL}/bootloader/subproject/main")

# ESP-IDF builds the bootloader for size (CONFIG_BOOTLOADER_COMPILER_
# OPTIMIZATION_SIZE, its default), with assertions, and without the stack
# protector.  The flags of the build type apply to the application only.
# -fmacro-prefix-map: the file names in assertion messages relative to the
# repository, not absolute (CONFIG_COMPILER_HIDE_PATHS_MACROS, its default).
foreach(_lang C ASM)
    foreach(_cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
        set(CMAKE_${_lang}_FLAGS_${_cfg} "")
    endforeach()
endforeach()
add_compile_options(-Os -ggdb -fno-stack-protector "-fmacro-prefix-map=${CMAKE_SOURCE_DIR}=.")
add_compile_definitions(
    BOOTLOADER_BUILD=1
    NON_OS_BUILD=1
    ESP_PLATFORM)

# Headers of every component the bootloader's sources reach.  ESP-IDF gives
# each component the public headers of the ones it requires; one list for all
# is the same set without the bookkeeping.
set(BL_INCLUDES
    "${PORT_DIR}/include"                       # sdkconfig.h
    "${ESP_HAL}/esp_libc/platform_include"      # assert.h (NON_OS build: headers only)
    "${BL_DIR}"
    "${ESP_HAL}/bootloader_support/include"
    "${ESP_HAL}/bootloader_support/bootloader_flash/include"
    "${ESP_HAL}/bootloader_support/private_include"
    "${ESP_IDF}/esp_bootloader_format/include"
    "${ESP_HAL}/esp_app_format/include"
    "${ESP_IDF}/esp_mspi/include"
    "${ESP_HAL}/spi_flash/include"
    "${ESP_HAL}/efuse/include"
    "${ESP_HAL}/efuse/esp32s3/include"
    "${ESP_HAL}/log/include"
    "${ESP_HAL}/xtensa/esp32s3/include"
    "${ESP_HAL}/xtensa/include"
    "${ESP_HAL}/soc/include"
    "${ESP_HAL}/soc/esp32s3/include"
    "${ESP_HAL}/soc/esp32s3/register"
    "${ESP_HAL}/hal/platform_port/include"
    "${ESP_HAL}/hal/esp32s3/include"
    "${ESP_HAL}/hal/include")
foreach(_c esp_hal_ana_conv esp_hal_cache esp_hal_clock esp_hal_debug_assist esp_hal_dma
           esp_hal_gpio esp_hal_gpspi esp_hal_mspi esp_hal_pmu esp_hal_regi2c
           esp_hal_rtc_timer esp_hal_security esp_hal_systimer esp_hal_timg esp_hal_uart
           esp_hal_usb esp_hal_wdt)
    list(APPEND BL_INCLUDES "${ESP_HAL}/${_c}/esp32s3/include" "${ESP_HAL}/${_c}/include")
endforeach()
list(APPEND BL_INCLUDES
    "${ESP_HAL}/esp_common/include"
    "${ESP_HAL}/esp_rom/include"
    "${ESP_HAL}/esp_rom/esp32s3/include"
    "${ESP_HAL}/esp_rom/esp32s3/include/esp32s3"
    "${ESP_HAL}/esp_rom/esp32s3"
    "${ESP_HAL}/esp_hw_support/include"
    "${ESP_HAL}/esp_hw_support/include/soc"
    "${ESP_HAL}/esp_hw_support/include/soc/esp32s3"
    "${ESP_HAL}/esp_hw_support/port/esp32s3/include"
    "${ESP_HAL}/esp_hw_support/port/esp32s3"
    "${ESP_HAL}/esp_hw_support/ldo/include"
    "${ESP_HAL}/esp_hw_support/debug_probe/include"
    "${ESP_HAL}/esp_hw_support/etm/include"
    "${ESP_HAL}/esp_hw_support/power_supply/include"
    "${ESP_HAL}/esp_hw_support/modem/include"
    "${ESP_HAL}/esp_system/include"
    # esp_sleep is not in esp-hal-3rdparty: the part rtc_clk.c includes
    "${PORT_DIR}/esp_compat/esp_sleep/include"
    # esp_mspi's timing-tuning header is not fetched: the declaration rtc_init.c includes
    "${PORT_DIR}/esp_compat/esp_mspi/include")

add_library(bl_includes INTERFACE)
target_include_directories(bl_includes INTERFACE ${BL_INCLUDES})

# bl_component(<target> <archive name> [PRIVATE_INCLUDES dir...] SOURCES src...)
function(bl_component target archive)
    cmake_parse_arguments(PARSE_ARGV 2 _arg "" "" "PRIVATE_INCLUDES;SOURCES")
    add_library(${target} STATIC ${_arg_SOURCES})
    set_target_properties(${target} PROPERTIES OUTPUT_NAME ${archive})
    target_link_libraries(${target} PRIVATE bl_includes)
    if(_arg_PRIVATE_INCLUDES)
        target_include_directories(${target} PRIVATE ${_arg_PRIVATE_INCLUDES})
    endif()
endfunction()

bl_component(bl_bootloader_support bootloader_support SOURCES
    "${ESP_HAL}/bootloader_support/src/bootloader_common.c"
    "${ESP_HAL}/bootloader_support/src/bootloader_common_loader.c"
    "${ESP_HAL}/bootloader_support/src/bootloader_clock_init.c"
    "${ESP_HAL}/bootloader_support/src/bootloader_mem.c"
    "${ESP_HAL}/bootloader_support/src/bootloader_random.c"
    "${ESP_HAL}/bootloader_support/src/bootloader_random_esp32s3.c"
    "${ESP_HAL}/bootloader_support/src/bootloader_efuse.c"
    "${ESP_HAL}/bootloader_support/src/flash_encrypt.c"
    "${ESP_HAL}/bootloader_support/src/secure_boot.c"
    "${ESP_HAL}/bootloader_support/bootloader_flash/src/bootloader_flash.c"
    "${ESP_HAL}/bootloader_support/bootloader_flash/src/flash_qio_mode.c"
    "${ESP_HAL}/bootloader_support/bootloader_flash/src/bootloader_flash_config_esp32s3.c"
    "${ESP_HAL}/bootloader_support/src/bootloader_utility.c"
    "${ESP_HAL}/bootloader_support/src/flash_partitions.c"
    "${ESP_HAL}/bootloader_support/src/bootloader_reset_common.c"
    "${ESP_HAL}/bootloader_support/src/bootloader_init.c"
    "${ESP_HAL}/bootloader_support/src/bootloader_clock_loader.c"
    "${ESP_HAL}/bootloader_support/src/bootloader_console.c"
    "${ESP_HAL}/bootloader_support/src/bootloader_console_loader.c"
    "${ESP_HAL}/bootloader_support/src/esp32s3/bootloader_soc.c"
    "${ESP_HAL}/bootloader_support/src/esp32s3/bootloader_esp32s3.c"
    "${ESP_HAL}/bootloader_support/src/bootloader_panic.c")

bl_component(bl_esp_image_verify esp_image_verify
    PRIVATE_INCLUDES "${ESP_IDF}/esp_image_verify/private_include"
    SOURCES
    "${ESP_IDF}/esp_image_verify/src/esp_image_format.c"
    "${ESP_IDF}/esp_image_verify/src/bootloader_sha.c"
    "${ESP_IDF}/esp_image_verify/src/bootloader_sha_flash.c")

bl_component(bl_esp_bootloader_format esp_bootloader_format SOURCES
    "${ESP_IDF}/esp_bootloader_format/esp_bootloader_desc.c")
set_source_files_properties("${ESP_IDF}/esp_bootloader_format/esp_bootloader_desc.c"
    PROPERTIES COMPILE_DEFINITIONS "IDF_VER_DEF=\"${IDF_VER}\"")

bl_component(bl_log log
    PRIVATE_INCLUDES "${ESP_HAL}/log/include/esp_private"
    SOURCES
    "${ESP_HAL}/log/src/noos/log_timestamp.c"
    "${ESP_HAL}/log/src/log_timestamp_common.c"
    "${ESP_HAL}/log/src/noos/log_lock.c"
    "${ESP_HAL}/log/src/buffer/log_buffers.c"
    "${ESP_HAL}/log/src/noos/util.c"
    "${ESP_HAL}/log/src/util.c"
    "${ESP_HAL}/log/src/log_format_text.c"
    "${ESP_HAL}/log/src/log_print.c"
    "${ESP_HAL}/log/src/log.c")

bl_component(bl_esp_rom esp_rom SOURCES
    "${ESP_HAL}/esp_rom/patches/esp_rom_sys.c"
    "${ESP_HAL}/esp_rom/patches/esp_rom_print.c"
    "${ESP_HAL}/esp_rom/patches/esp_rom_crc.c"
    "${ESP_HAL}/esp_rom/patches/esp_rom_serial_output.c"
    "${ESP_HAL}/esp_rom/patches/esp_rom_spiflash.c"
    "${ESP_HAL}/esp_rom/patches/esp_rom_efuse.c"
    "${ESP_HAL}/esp_rom/patches/esp_rom_gpio.c"
    "${ESP_HAL}/esp_rom/patches/esp_rom_longjmp.S"
    "${ESP_HAL}/esp_rom/patches/esp_rom_cache_esp32s2_esp32s3.c"
    "${ESP_HAL}/esp_rom/patches/esp_rom_cache_writeback_esp32s3.S")

bl_component(bl_esp_common esp_common SOURCES
    "${ESP_HAL}/esp_common/src/esp_err_to_name.c")

bl_component(bl_esp_system esp_system SOURCES
    "${ESP_HAL}/esp_system/esp_err.c"
    "${ESP_HAL}/esp_system/esp_check.c")

bl_component(bl_esp_hw_support esp_hw_support
    PRIVATE_INCLUDES
    "${ESP_HAL}/esp_hw_support/port/include"
    "${ESP_HAL}/esp_hw_support/include/esp_private"
    SOURCES
    "${ESP_HAL}/esp_hw_support/cpu.c"
    "${ESP_HAL}/esp_hw_support/port/esp32s3/esp_cpu_intr.c"
    "${ESP_HAL}/esp_hw_support/esp_memory_utils.c"
    "${ESP_HAL}/esp_hw_support/port/esp32s3/cpu_region_protect.c"
    "${ESP_HAL}/esp_hw_support/port/esp32s3/rtc_clk.c"
    "${ESP_HAL}/esp_hw_support/port/esp32s3/rtc_clk_init.c"
    "${ESP_HAL}/esp_hw_support/port/esp32s3/rtc_init.c"
    "${ESP_HAL}/esp_hw_support/port/esp32s3/rtc_sleep.c"
    "${ESP_HAL}/esp_hw_support/port/esp32s3/rtc_time.c"
    "${ESP_HAL}/esp_hw_support/port/esp32s3/chip_info.c")

bl_component(bl_efuse efuse
    PRIVATE_INCLUDES
    "${ESP_HAL}/efuse/private_include"
    "${ESP_HAL}/efuse/esp32s3/private_include"
    SOURCES
    "${ESP_HAL}/efuse/esp32s3/esp_efuse_table.c"
    "${ESP_HAL}/efuse/esp32s3/esp_efuse_fields.c"
    "${ESP_HAL}/efuse/esp32s3/esp_efuse_utility.c"
    "${ESP_HAL}/efuse/src/esp_efuse_api.c"
    "${ESP_HAL}/efuse/src/esp_efuse_dump.c"
    "${ESP_HAL}/efuse/src/esp_efuse_fields.c"
    "${ESP_HAL}/efuse/src/esp_efuse_utility.c"
    "${ESP_HAL}/efuse/src/efuse_controller/keys/with_key_purposes/esp_efuse_api_key.c")

bl_component(bl_hal hal SOURCES
    "${ESP_HAL}/hal/hal_utils.c"
    "${ESP_HAL}/hal/efuse_hal.c"
    "${ESP_HAL}/hal/esp32s3/efuse_hal.c")

bl_component(bl_esp_hal_cache esp_hal_cache SOURCES
    "${ESP_HAL}/esp_hal_cache/cache_hal.c"
    "${ESP_HAL}/esp_hal_cache/mmu_hal.c")

# The application uses the C implementation of the watchdog HAL, not the
# ROM's (CONFIG_HAL_WDT_USE_ROM_IMPL is not set); the bootloader the same.
bl_component(bl_esp_hal_wdt esp_hal_wdt SOURCES
    "${ESP_HAL}/esp_hal_wdt/wdt_hal_iram.c")

bl_component(bl_esp_hal_uart esp_hal_uart SOURCES
    "${ESP_HAL}/esp_hal_uart/esp32s3/uart_periph.c")

bl_component(bl_esp_hal_security esp_hal_security SOURCES
    "${ESP_HAL}/esp_hal_security/mpu_hal.c")

set(BL_LIBS
    bl_bootloader_support bl_esp_image_verify bl_esp_bootloader_format bl_log
    bl_esp_rom bl_esp_common bl_esp_system bl_esp_hw_support bl_efuse bl_hal
    bl_esp_hal_cache bl_esp_hal_wdt bl_esp_hal_uart
    bl_esp_hal_security)

# ---------------------------------------------------------------------------
# Linker scripts
# ---------------------------------------------------------------------------
# ESP-IDF runs its ".ld.in" scripts through the C preprocessor with
# sdkconfig.h on the include path.  The memory map and the section layout are
# the bootloader's own; the ROM and peripheral tables are those ESP-IDF adds
# for a bootloader build on this chip.
set(BL_LD_DIR "${CMAKE_CURRENT_BINARY_DIR}/ld")
set(BL_LD_OUT)
foreach(_in
        "${BL_DIR}/ld/esp32s3/bootloader.memory.ld.in"
        "${BL_DIR}/ld/esp32s3/bootloader.sections.ld.in")
    get_filename_component(_name "${_in}" NAME_WLE)
    set(_out "${BL_LD_DIR}/${_name}")
    add_custom_command(OUTPUT "${_out}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${BL_LD_DIR}"
        COMMAND "${CMAKE_C_COMPILER}" -C -P -x c -E -o "${_out}"
                -I "${PORT_DIR}/include" -I "${BL_DIR}/ld" -I "${ESP_HAL}/esp_system/ld"
                "${_in}"
        DEPENDS "${_in}" "${PORT_DIR}/include/sdkconfig.h"
                "${BL_DIR}/ld/bootloader.sections.common.ld"
        VERBATIM)
    list(APPEND BL_LD_OUT "${_out}")
endforeach()
add_custom_target(bl_linker_scripts DEPENDS ${BL_LD_OUT})

set(BL_ROM_LD "${ESP_HAL}/esp_rom/esp32s3/ld")
set(BL_LINKER_SCRIPTS
    ${BL_LD_OUT}
    "${ESP_HAL}/soc/esp32s3/ld/esp32s3.peripherals.ld"
    "${BL_ROM_LD}/esp32s3.rom.ld"
    "${BL_ROM_LD}/esp32s3.rom.api.ld"
    "${BL_ROM_LD}/esp32s3.rom.bt_funcs.ld"
    "${BL_ROM_LD}/esp32s3.rom.libgcc.ld"
    "${BL_ROM_LD}/esp32s3.rom.version.ld"
    "${BL_ROM_LD}/esp32s3.rom.libc.ld"
    "${BL_ROM_LD}/esp32s3.rom.newlib.ld"
    "${ESP_HAL}/esp_hal_regi2c/esp32s3/rom.regi2c.ld")

# The "main" component: call_start_cpu0.
add_executable(bootloader "${BL_DIR}/bootloader_start.c")
target_link_libraries(bootloader PRIVATE bl_includes)
set_target_properties(bootloader PROPERTIES
    SUFFIX .elf
    LINKER_LANGUAGE C
    LINK_DEPENDS "${BL_LD_OUT}")
add_dependencies(bootloader bl_linker_scripts)
set(_ld_opts)
foreach(_ld IN LISTS BL_LINKER_SCRIPTS)
    list(APPEND _ld_opts "-T${_ld}")
endforeach()
# -u: what ESP-IDF's components add for a bootloader build, so that the
# archive members are taken even though nothing calls them by name.
target_link_options(bootloader PRIVATE
    -nostartfiles
    ${_ld_opts}
    -Wl,--gc-sections
    -Wl,-u,esp_bootloader_desc
    -Wl,-u,abort
    "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/bootloader.map")
target_link_libraries(bootloader PRIVATE -Wl,--start-group ${BL_LIBS} -Wl,--end-group)
