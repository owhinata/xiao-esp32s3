# Upstream source trees, fetched at configure time.
#
# fetch_upstream(<name> <url> <commit>) makes lib/<name> a checkout of exactly
# <commit> (a full SHA-1).  lib/ is not tracked by this repository; the
# commits named in CMakeLists.txt are what pins the versions.
#
# Only that one commit is downloaded (a depth-1 fetch by SHA, which GitHub
# serves).  A tree that is already at the commit is left alone, so configuring
# again costs nothing and works offline.  A tree with local modifications is
# never overwritten: the upstream sources are meant to be used unmodified, and
# silently discarding an experiment would be worse than stopping.

find_package(Git REQUIRED)

function(_fetch_upstream_git dir)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${dir}" ${ARGN}
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(_git_rc "${_rc}" PARENT_SCOPE)
    set(_git_out "${_out}" PARENT_SCOPE)
    set(_git_err "${_err}" PARENT_SCOPE)
endfunction()

function(fetch_upstream name url commit)
    set(dir "${CMAKE_SOURCE_DIR}/lib/${name}")

    if(EXISTS "${dir}/.git")
        _fetch_upstream_git("${dir}" rev-parse HEAD)
        if(_git_rc EQUAL 0 AND _git_out STREQUAL "${commit}")
            return()
        endif()
        _fetch_upstream_git("${dir}" status --porcelain)
        if(NOT _git_rc EQUAL 0 OR NOT _git_out STREQUAL "")
            message(FATAL_ERROR
                "lib/${name} is not at ${commit} and has local changes.  "
                "Clean it up or remove the directory, then configure again.")
        endif()
    else()
        if(EXISTS "${dir}")
            file(GLOB _entries "${dir}/*")
            if(_entries)
                message(FATAL_ERROR
                    "lib/${name} exists but is not a git checkout.  Remove it and configure again.")
            endif()
        endif()
        file(MAKE_DIRECTORY "${dir}")
        _fetch_upstream_git("${dir}" init --quiet)
        if(NOT _git_rc EQUAL 0)
            message(FATAL_ERROR "git init in lib/${name} failed: ${_git_err}")
        endif()
        _fetch_upstream_git("${dir}" remote add origin "${url}")
    endif()

    message(STATUS "Fetching ${name} ${commit} ...")
    _fetch_upstream_git("${dir}" fetch --quiet --depth 1 origin "${commit}")
    if(NOT _git_rc EQUAL 0)
        message(FATAL_ERROR "Fetching ${url} ${commit} failed: ${_git_err}")
    endif()
    _fetch_upstream_git("${dir}" -c advice.detachedHead=false checkout --quiet --detach FETCH_HEAD)
    if(NOT _git_rc EQUAL 0)
        message(FATAL_ERROR "Checking out ${commit} in lib/${name} failed: ${_git_err}")
    endif()
endfunction()
