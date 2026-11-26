# Writes the header that gives esp_app_desc.c the project's version, from
# "git describe" of the repository (ESP-IDF's rule, falling back to "1"; at
# most the 31 characters esp_app_desc_t.version holds).  Run at every build
# (cmake -DGIT=<git> -DSOURCE_DIR=<repository> -DOUT=<header> -P
# app_version.cmake); the header is rewritten only when the version changes,
# so nothing is recompiled otherwise.
set(_ver "1")
execute_process(
    COMMAND "${GIT}" -C "${SOURCE_DIR}" describe --always --tags --dirty
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
if(_rc EQUAL 0 AND _out)
    string(SUBSTRING "${_out}" 0 31 _ver)
endif()
set(_text "#define PROJECT_VER \"${_ver}\"\n")
set(_old "")
if(EXISTS "${OUT}")
    file(READ "${OUT}" _old)
endif()
if(NOT _old STREQUAL _text)
    file(WRITE "${OUT}" "${_text}")
endif()
