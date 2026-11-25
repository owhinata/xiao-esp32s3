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

set(ESP_HAL  "${CMAKE_CURRENT_LIST_DIR}/lib/esp-hal-3rdparty/components")
set(TX_DIR   "${CMAKE_CURRENT_LIST_DIR}/lib/threadx")
set(TX_PORT  "${TX_DIR}/ports/xtensa/xcc")
set(PORT_DIR "${CMAKE_CURRENT_LIST_DIR}/port")

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
    esp_hal_cache esp_hal_clock esp_hal_gpio esp_hal_gpspi esp_hal_regi2c
    esp_hal_systimer esp_hal_timg esp_hal_usb esp_hal_wdt)
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
    "${ESP_HAL}/hal/include"
    "${ESP_HAL}/hal/esp32s3/include"
    "${ESP_HAL}/hal/platform_port/include"
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
    # esp_sleep is not in esp-hal-3rdparty; the part rtc_clk.c needs
    "${PORT_DIR}/esp_compat/esp_sleep/include")
target_link_libraries(esp_hal INTERFACE "${ESP_HAL}/xtensa/esp32s3/libxt_hal.a")
target_link_options(esp_hal INTERFACE
    "-T${ESP_HAL}/soc/esp32s3/ld/esp32s3.peripherals.ld"
    "-T${ESP_HAL}/esp_rom/esp32s3/ld/esp32s3.rom.ld"
    "-T${ESP_HAL}/esp_rom/esp32s3/ld/esp32s3.rom.api.ld"
    # The analog I2C (regi2c) accessors are in ROM (ESP_ROM_HAS_REGI2C_IMPL).
    "-T${ESP_HAL}/esp_hal_regi2c/esp32s3/rom.regi2c.ld"
    # memcpy/memset/str* and the libgcc helpers in ROM: always reachable, even
    # before the flash is mapped, and they cost no flash or RAM.
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
# from esp_system.  The sources are used unmodified.
#
# portNUM_PROCESSORS sizes the layer's per-core tables; two of its files take
# it from the build rather than from xtensa_rtos.h.
add_library(xtensa_rtos OBJECT
    "${ESP_HAL}/xtensa/xtensa_context.S"
    "${ESP_HAL}/xtensa/xtensa_intr_asm.S"
    "${ESP_HAL}/xtensa/xtensa_intr.c"
    "${ESP_HAL}/xtensa/xtensa_vectors.S"
    "${ESP_HAL}/esp_system/port/arch/xtensa/panic_handler_asm.S")
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
    "${PORT_DIR}/threadx/src/tx_xtensa_stack_error_handler.c"
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
# Application
# ---------------------------------------------------------------------------
set(LDSCRIPT "${CMAKE_CURRENT_LIST_DIR}/ldscript/xiao_esp32s3.ld")

add_executable(app
    src/main.c
    "${PORT_DIR}/startup.c"
    "${PORT_DIR}/board.c"
    "${PORT_DIR}/panic.c"
    "${PORT_DIR}/syscalls.c"
    "${PORT_DIR}/flash_map.c"
    "${PORT_DIR}/esp_compat/esp_stubs.c"
    # esp-hal-3rdparty sources this image needs, compiled as they are
    "${ESP_HAL}/esp_hal_wdt/wdt_hal_iram.c"
    "${ESP_HAL}/esp_hal_cache/cache_hal.c"
    "${ESP_HAL}/esp_hal_cache/mmu_hal.c"
    "${ESP_HAL}/esp_hw_support/port/esp32s3/rtc_clk.c"
    "${ESP_HAL}/esp_hw_support/port/esp32s3/rtc_clk_init.c"
    # rtc_init.c: only for the regulator bias variables rtc_clk.c reads;
    # rtc_init() itself is not called yet (see README) and is dropped by
    # --gc-sections together with what it would need.
    "${ESP_HAL}/esp_hw_support/port/esp32s3/rtc_init.c"
    "${ESP_HAL}/esp_hw_support/regi2c_ctrl.c"
    "${ESP_HAL}/esp_rom/patches/esp_rom_sys.c"
    # Work-arounds for bugs in the ROM's cache suspend/write-back routines.
    "${ESP_HAL}/esp_rom/patches/esp_rom_cache_esp32s2_esp32s3.c"
    # SYSTIMER: the uptime clock (port/board.c).
    "${ESP_HAL}/esp_hal_systimer/systimer_hal.c"
    "${ESP_HAL}/esp_hw_support/port/esp32s3/systimer.c"
    # The second-stage bootloader's clock and flash-interface set-up, called
    # by _start and flash_map_image().  Of the flash file only the three
    # functions flash_map_image() calls are linked; the rest (flash ID, QIO,
    # write protection, logging) is dropped by --gc-sections.
    "${ESP_HAL}/bootloader_support/src/bootloader_clock_init.c"
    "${ESP_HAL}/bootloader_support/bootloader_flash/src/bootloader_flash_config_esp32s3.c")
# These use FreeRTOS only for critical sections; give them the stand-in header.
# It is kept off the global include path on purpose: the Xtensa layer switches
# to its FreeRTOS code paths as soon as it can see a "freertos/FreeRTOS.h".
set_source_files_properties(
    "${ESP_HAL}/esp_hw_support/regi2c_ctrl.c"
    PROPERTIES INCLUDE_DIRECTORIES "${PORT_DIR}/esp_compat/include")
# esp_mspi is not in esp-hal-3rdparty: the two of its headers that rtc_init.c
# and bootloader_flash_config_esp32s3.c include, reduced to what they name.
set_source_files_properties(
    "${ESP_HAL}/esp_hw_support/port/esp32s3/rtc_init.c"
    "${ESP_HAL}/bootloader_support/bootloader_flash/src/bootloader_flash_config_esp32s3.c"
    PROPERTIES INCLUDE_DIRECTORIES "${PORT_DIR}/esp_compat/esp_mspi/include")
# bootloader_support headers, for its own sources and the two callers.
set(BOOTLOADER_SUPPORT_INCLUDES
    "${ESP_HAL}/bootloader_support/include"
    "${ESP_HAL}/bootloader_support/bootloader_flash/include"
    "${ESP_HAL}/bootloader_support/private_include")
set_property(SOURCE
    "${PORT_DIR}/startup.c"
    "${PORT_DIR}/flash_map.c"
    "${ESP_HAL}/bootloader_support/src/bootloader_clock_init.c"
    "${ESP_HAL}/bootloader_support/bootloader_flash/src/bootloader_flash_config_esp32s3.c"
    APPEND PROPERTY INCLUDE_DIRECTORIES "${BOOTLOADER_SUPPORT_INCLUDES}")

set_target_properties(app PROPERTIES
    OUTPUT_NAME xiao_esp32s3_threadx
    SUFFIX      .elf
    LINK_DEPENDS "${LDSCRIPT}")
target_include_directories(app PRIVATE "${PORT_DIR}")
# xtensa_rtos is an object library: the vectors are reached only through the
# linker script and VECBASE, so nothing would pull them out of an archive.
target_link_libraries(app PRIVATE shell threadx xtensa_rtos esp_hal)
target_link_options(app PRIVATE
    -nostartfiles
    "-T${LDSCRIPT}"
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
# Image and download targets (esptool from ./.venv, see README)
# ---------------------------------------------------------------------------
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(ESPTOOL "${CMAKE_CURRENT_LIST_DIR}/.venv/bin/esptool" CACHE FILEPATH "esptool executable")
set(ESPPORT "" CACHE STRING "Serial port for the flash/run targets (empty = esptool auto-detect)")
set(APP_BIN "${CMAKE_CURRENT_BINARY_DIR}/xiao_esp32s3_threadx.bin")
set(_port_arg "")
if(ESPPORT)
    set(_port_arg --port "${ESPPORT}")
endif()

# The XIAO ESP32S3 carries 8 MB of flash.  --ram-only-header: the ROM loads
# the RAM segments only; _start maps the flash segments (port/flash_map.c).
add_custom_command(TARGET app POST_BUILD
    COMMAND "${CMAKE_SIZE}" -A "$<TARGET_FILE:app>"
    COMMAND "${ESPTOOL}" --chip esp32s3 elf2image --ram-only-header
            --flash-mode dio --flash-freq 80m --flash-size 8MB
            -o "${APP_BIN}" "$<TARGET_FILE:app>"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/script/add_boot_digest.py" "${APP_BIN}"
    COMMAND_EXPAND_LISTS VERBATIM)

# Write to flash offset 0, where the ESP32-S3 ROM looks for its boot image.
# This replaces whatever bootloader is on the board.
add_custom_target(flash
    COMMAND "${ESPTOOL}" --chip esp32s3 ${_port_arg} write-flash
            --flash-mode dio --flash-freq 80m --flash-size 8MB 0x0 "${APP_BIN}"
    DEPENDS app USES_TERMINAL COMMAND_EXPAND_LISTS VERBATIM)
