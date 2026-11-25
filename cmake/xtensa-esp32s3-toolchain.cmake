# Toolchain file for the Espressif Xtensa GCC bare-metal toolchain (ESP32-S3).
#
# The toolchain is downloaded automatically into ./tools on first configure.
# Toolchain files are processed before the compiler check, so the download must
# happen here.
#
# The version is the crosstool-NG release that ESP-IDF's tools/tools.json
# names at the ESP-IDF commit the pinned esp-hal-3rdparty snapshot mirrors
# (master ad20ded6, 2026-11-15).  Move both together: newer HAL snapshots may
# need a newer compiler and vice versa.  Each version is unpacked into a
# directory of its own, so changing TC_VERSION fetches that version.

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR xtensa)

# --- Auto-download the toolchain --------------------------------------------
set(TC_VERSION  "16.1.0_20260609")
set(TC_ARCHIVE  "xtensa-esp-elf-${TC_VERSION}-x86_64-linux-gnu.tar.xz")
set(TC_URL      "https://github.com/espressif/crosstool-NG/releases/download/esp-${TC_VERSION}/${TC_ARCHIVE}")
set(TOOLS_DIR   "${CMAKE_CURRENT_LIST_DIR}/../tools")
set(GCC_DIR     "${TOOLS_DIR}/xtensa-esp-elf-${TC_VERSION}")
set(GCC_BIN     "${GCC_DIR}/bin")
set(TC_PREFIX   "${GCC_BIN}/xtensa-esp32s3-elf-")

if(NOT EXISTS "${TC_PREFIX}gcc")
    if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" OR
       NOT CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "x86_64")
        message(FATAL_ERROR
            "Automatic toolchain download only knows x86_64 Linux.  Unpack "
            "xtensa-esp-elf ${TC_VERSION} for your host into ${GCC_DIR}.")
    endif()
    message(STATUS "Downloading xtensa-esp-elf ${TC_VERSION} (~90 MB) ...")
    file(MAKE_DIRECTORY "${TOOLS_DIR}")
    set(_tarball "${TOOLS_DIR}/${TC_ARCHIVE}")
    if(NOT EXISTS "${_tarball}")
        file(DOWNLOAD "${TC_URL}" "${_tarball}" SHOW_PROGRESS STATUS _dl)
        list(GET _dl 0 _dl_code)
        if(NOT _dl_code EQUAL 0)
            file(REMOVE "${_tarball}")
            message(FATAL_ERROR "Toolchain download failed: ${_dl}")
        endif()
    endif()
    message(STATUS "Extracting toolchain ...")
    # The archive holds a single directory, xtensa-esp-elf/.
    set(_staging "${TOOLS_DIR}/.extract-${TC_VERSION}")
    file(REMOVE_RECURSE "${_staging}")
    file(MAKE_DIRECTORY "${_staging}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E tar xJf "${_tarball}"
        WORKING_DIRECTORY "${_staging}"
        RESULT_VARIABLE _untar)
    file(REMOVE "${_tarball}")
    if(NOT _untar EQUAL 0 OR NOT EXISTS "${_staging}/xtensa-esp-elf")
        file(REMOVE_RECURSE "${_staging}")
        message(FATAL_ERROR "Toolchain extraction failed")
    endif()
    file(REMOVE_RECURSE "${GCC_DIR}")
    file(RENAME "${_staging}/xtensa-esp-elf" "${GCC_DIR}")
    file(REMOVE_RECURSE "${_staging}")
endif()

# --- Compilers / tools ------------------------------------------------------
set(CMAKE_C_COMPILER   "${TC_PREFIX}gcc")
set(CMAKE_ASM_COMPILER "${TC_PREFIX}gcc")
set(CMAKE_CXX_COMPILER "${TC_PREFIX}g++")
set(CMAKE_OBJCOPY      "${TC_PREFIX}objcopy" CACHE FILEPATH "")
set(CMAKE_OBJDUMP      "${TC_PREFIX}objdump" CACHE FILEPATH "")
set(CMAKE_SIZE         "${TC_PREFIX}size"    CACHE FILEPATH "")
set(CMAKE_NM           "${TC_PREFIX}nm"      CACHE FILEPATH "")

# The compiler check links a host-style executable, which a bare-metal target
# cannot do without the project's linker script.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
