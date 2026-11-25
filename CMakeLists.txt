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
    components/esp_mspi/include)

set(ESP_HAL  "${CMAKE_CURRENT_LIST_DIR}/lib/esp-hal-3rdparty/components")
set(ESP_IDF  "${CMAKE_CURRENT_LIST_DIR}/lib/esp-idf/components")
set(TX_DIR   "${CMAKE_CURRENT_LIST_DIR}/lib/threadx")
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
    esp_hal_cache esp_hal_clock esp_hal_gpio esp_hal_gpspi esp_hal_regi2c
    esp_hal_systimer esp_hal_timg esp_hal_uart esp_hal_usb esp_hal_wdt)
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
    "${PORT_DIR}/esp_compat/esp_stubs.c"
    # esp-hal-3rdparty sources this image needs, compiled as they are
    # The application description the second-stage bootloader reads from the
    # start of the read-only data (esp_app_desc_t; see the linker script).
    "${ESP_HAL}/esp_app_format/esp_app_desc.c"
    "${ESP_HAL}/esp_hal_wdt/wdt_hal_iram.c"
    "${ESP_HAL}/esp_hw_support/port/esp32s3/rtc_clk.c"
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
    # UART0: the console on D6/D7 (port/board.c), and the table of IO MUX
    # registers that gpio_ll_pullup_en() looks the RX pin up in.
    "${ESP_HAL}/esp_hal_uart/uart_hal.c"
    "${ESP_HAL}/soc/esp32s3/gpio_periph.c")
# These use FreeRTOS only for critical sections; give them the stand-in header.
# It is kept off the global include path on purpose: the Xtensa layer switches
# to its FreeRTOS code paths as soon as it can see a "freertos/FreeRTOS.h".
set_source_files_properties(
    "${ESP_HAL}/esp_hw_support/regi2c_ctrl.c"
    PROPERTIES INCLUDE_DIRECTORIES "${PORT_DIR}/esp_compat/include")
# esp_mspi's timing-tuning header is not fetched: rtc_init.c includes it for
# one declaration (port/esp_compat/esp_mspi).
set_source_files_properties(
    "${ESP_HAL}/esp_hw_support/port/esp32s3/rtc_init.c"
    PROPERTIES INCLUDE_DIRECTORIES "${PORT_DIR}/esp_compat/esp_mspi/include")
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

set_target_properties(app PROPERTIES
    OUTPUT_NAME xiao_esp32s3_threadx
    SUFFIX      .elf
    LINK_DEPENDS "${LDSCRIPT}")
target_include_directories(app PRIVATE "${PORT_DIR}")
# xtensa_rtos is an object library: the vectors are reached only through the
# linker script and VECBASE, so nothing would pull them out of an archive.
target_link_libraries(app PRIVATE shell threadx xtensa_rtos esp_hal)
# -u esp_app_desc: nothing refers to it by name (esp_app_format does the same).
target_link_options(app PRIVATE
    -nostartfiles
    "-T${LDSCRIPT}"
    -Wl,-u,esp_app_desc
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
