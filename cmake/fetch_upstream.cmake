# Upstream source trees, fetched at configure time.
#
# fetch_upstream(<name> <url> <commit> [SPARSE <path>...]) makes lib/<name> a
# checkout of exactly <commit> (a full SHA-1).  lib/ is not tracked by this
# repository; the commits named in CMakeLists.txt are what pins the versions.
#
# Only that one commit is downloaded (a depth-1 fetch by SHA, which GitHub
# serves).  A tree that is already at the commit is left alone, so configuring
# again costs nothing and works offline.  A tree with local modifications is
# never overwritten: the upstream sources are meant to be used unmodified, and
# silently discarding an experiment would be worse than stopping.
#
# SPARSE: only the listed directories (paths from the repository root) are
# checked out, and only their files are downloaded (a partial clone without
# blobs; git fetches the blobs of the sparse paths when it checks them out).
# Changing the list updates the existing checkout.

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
    cmake_parse_arguments(PARSE_ARGV 3 _arg "" "" "SPARSE")
    set(dir "${CMAKE_SOURCE_DIR}/lib/${name}")

    # Sparse patterns: each directory and everything below it.
    set(_patterns)
    foreach(_p IN LISTS _arg_SPARSE)
        list(APPEND _patterns "/${_p}/")
    endforeach()

    if(EXISTS "${dir}/.git")
        _fetch_upstream_git("${dir}" rev-parse HEAD)
        if(_git_rc EQUAL 0 AND _git_out STREQUAL "${commit}")
            if(NOT _patterns)
                return()
            endif()
            _fetch_upstream_git("${dir}" sparse-checkout list)
            string(REPLACE "\n" ";" _have "${_git_out}")
            if(_git_rc EQUAL 0 AND "${_have}" STREQUAL "${_patterns}")
                return()
            endif()
            message(STATUS "Updating the sparse checkout of ${name} ...")
            _fetch_upstream_git("${dir}" sparse-checkout set --no-cone ${_patterns})
            if(NOT _git_rc EQUAL 0)
                message(FATAL_ERROR "Updating the sparse checkout of lib/${name} failed: ${_git_err}")
            endif()
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

    set(_filter)
    if(_patterns)
        # The remote serves the missing blobs on demand (partial clone).
        _fetch_upstream_git("${dir}" config remote.origin.promisor true)
        _fetch_upstream_git("${dir}" config remote.origin.partialclonefilter blob:none)
        _fetch_upstream_git("${dir}" sparse-checkout set --no-cone ${_patterns})
        if(NOT _git_rc EQUAL 0)
            message(FATAL_ERROR "Setting up the sparse checkout of lib/${name} failed: ${_git_err}")
        endif()
        set(_filter --filter=blob:none)
    endif()

    message(STATUS "Fetching ${name} ${commit} ...")
    _fetch_upstream_git("${dir}" fetch --quiet --depth 1 ${_filter} origin "${commit}")
    if(NOT _git_rc EQUAL 0)
        message(FATAL_ERROR "Fetching ${url} ${commit} failed: ${_git_err}")
    endif()
    _fetch_upstream_git("${dir}" -c advice.detachedHead=false checkout --quiet --detach FETCH_HEAD)
    if(NOT _git_rc EQUAL 0)
        message(FATAL_ERROR "Checking out ${commit} in lib/${name} failed: ${_git_err}")
    endif()
endfunction()
