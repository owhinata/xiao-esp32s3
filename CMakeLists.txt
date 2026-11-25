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

set(ESP_HAL  "${CMAKE_CURRENT_LIST_DIR}/lib/esp-hal-components/components")
set(TX_DIR   "${CMAKE_CURRENT_LIST_DIR}/lib/threadx")
set(TX_PORT  "${TX_DIR}/ports/xtensa/xcc")
set(PORT_DIR "${CMAKE_CURRENT_LIST_DIR}/port")

foreach(_sentinel "${ESP_HAL}/hal" "${TX_DIR}/common/src")
    if(NOT EXISTS "${_sentinel}")
        message(FATAL_ERROR
            "Missing ${_sentinel}.  Run: git submodule update --init --depth 1")
    endif()
endforeach()

# -mlongcalls: code and ROM are further apart than a direct call reaches.
add_compile_options(
    -mlongcalls
    -ffunction-sections -fdata-sections
    -fno-common
    -Wall)

# ---------------------------------------------------------------------------
# esp-hal-components: headers, ROM/peripheral symbol tables, Xtensa HAL library
# ---------------------------------------------------------------------------
add_library(esp_hal INTERFACE)
target_include_directories(esp_hal INTERFACE
    "${PORT_DIR}/include"                       # sdkconfig.h, Xtensa Tools stand-ins
    "${ESP_HAL}/xtensa/esp32s3/include"
    "${ESP_HAL}/xtensa/include"
    "${ESP_HAL}/soc/include"
    "${ESP_HAL}/soc/esp32s3/include"
    "${ESP_HAL}/soc/esp32s3/register"
    "${ESP_HAL}/hal/include"
    "${ESP_HAL}/hal/esp32s3/include"
    "${ESP_HAL}/hal/platform_port/include"
    "${ESP_HAL}/esp_common/include"
    "${ESP_HAL}/esp_rom/include"
    "${ESP_HAL}/esp_rom/esp32s3"
    "${ESP_HAL}/esp_rom/esp32s3/include"
    "${ESP_HAL}/esp_rom/esp32s3/include/esp32s3"
    "${ESP_HAL}/esp_hw_support/include"
    "${ESP_HAL}/esp_hw_support/include/soc"
    "${ESP_HAL}/esp_hw_support/include/esp_private"
    "${ESP_HAL}/esp_hw_support/port/include"
    "${ESP_HAL}/esp_hw_support/port/esp32s3"
    "${ESP_HAL}/esp_hw_support/port/esp32s3/include"
    "${ESP_HAL}/esp_system/include"
    "${ESP_HAL}/log/include")
target_link_libraries(esp_hal INTERFACE "${ESP_HAL}/xtensa/esp32s3/libxt_hal.a")
target_link_options(esp_hal INTERFACE
    "-T${ESP_HAL}/soc/esp32s3/ld/esp32s3.peripherals.ld"
    "-T${ESP_HAL}/esp_rom/esp32s3/ld/esp32s3.rom.ld"
    "-T${ESP_HAL}/esp_rom/esp32s3/ld/esp32s3.rom.api.ld")

# ---------------------------------------------------------------------------
# Xtensa RTOS porting layer from esp-hal-components
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
    "${PORT_DIR}/threadx/src/tx_clib_newlib.c")
target_include_directories(threadx PUBLIC "${TX_DIR}/common/inc")
target_link_libraries(threadx PUBLIC xtensa_rtos)

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
set(LDSCRIPT "${CMAKE_CURRENT_LIST_DIR}/ldscript/xiao_esp32s3_ram.ld")

add_executable(app
    src/main.c
    "${PORT_DIR}/startup.c"
    "${PORT_DIR}/board.c"
    "${PORT_DIR}/panic.c"
    "${PORT_DIR}/syscalls.c"
    # esp-hal-components sources this image needs, compiled as they are
    "${ESP_HAL}/hal/wdt_hal_iram.c"
    # Only rtc_clk_cpu_freq_get_config() is used; the functions that program
    # the clock tree need more of esp_hw_support and are dropped by --gc-sections.
    "${ESP_HAL}/esp_hw_support/port/esp32s3/rtc_clk.c"
    "${ESP_HAL}/esp_rom/patches/esp_rom_sys.c")
set_target_properties(app PROPERTIES
    OUTPUT_NAME xiao_esp32s3_threadx
    SUFFIX      .elf
    LINK_DEPENDS "${LDSCRIPT}")
target_include_directories(app PRIVATE "${PORT_DIR}")
# xtensa_rtos is an object library: the vectors are reached only through the
# linker script and VECBASE, so nothing would pull them out of an archive.
target_link_libraries(app PRIVATE threadx xtensa_rtos esp_hal)
target_link_options(app PRIVATE
    -nostartfiles
    "-T${LDSCRIPT}"
    -Wl,--gc-sections
    "-Wl,-Map=$<TARGET_FILE_DIR:app>/xiao_esp32s3_threadx.map")

# ---------------------------------------------------------------------------
# Image and download targets (esptool from ./.venv, see README)
# ---------------------------------------------------------------------------
set(ESPTOOL "${CMAKE_CURRENT_LIST_DIR}/.venv/bin/esptool" CACHE FILEPATH "esptool executable")
set(ESPPORT "" CACHE STRING "Serial port for the flash/run targets (empty = esptool auto-detect)")
set(APP_BIN "${CMAKE_CURRENT_BINARY_DIR}/xiao_esp32s3_threadx.bin")
set(_port_arg "")
if(ESPPORT)
    set(_port_arg --port "${ESPPORT}")
endif()

# The XIAO ESP32S3 carries 8 MB of flash.  Mode and frequency only tell the
# ROM how to read the image; nothing runs from flash.
add_custom_command(TARGET app POST_BUILD
    COMMAND "${CMAKE_SIZE}" "$<TARGET_FILE:app>"
    COMMAND "${ESPTOOL}" --chip esp32s3 elf2image
            --flash-mode dio --flash-freq 80m --flash-size 8MB
            -o "${APP_BIN}" "$<TARGET_FILE:app>"
    COMMAND_EXPAND_LISTS VERBATIM)

# Load into RAM and run.  Does not touch flash; the board must be in ROM
# download mode (hold B, tap R).
add_custom_target(run
    COMMAND "${ESPTOOL}" --chip esp32s3 ${_port_arg} --no-stub load-ram "${APP_BIN}"
    DEPENDS app USES_TERMINAL COMMAND_EXPAND_LISTS VERBATIM)

# Write to flash offset 0, where the ESP32-S3 ROM looks for its boot image.
# This replaces whatever bootloader is on the board.
add_custom_target(flash
    COMMAND "${ESPTOOL}" --chip esp32s3 ${_port_arg} write-flash
            --flash-mode dio --flash-freq 80m --flash-size 8MB 0x0 "${APP_BIN}"
    DEPENDS app USES_TERMINAL COMMAND_EXPAND_LISTS VERBATIM)
