# Generate version.h with the current git commit hash and date.
# Only rewrites the file if the content changed (avoids rebuild cascade).
# Usage: cmake -DSRC_DIR=... -DOUTPUT=... -P version.cmake
# Optional: cmake -DACE_VERSION_HASH=... -DACE_VERSION_DATE=...

if(NOT ACE_VERSION_HASH)
    execute_process(
        COMMAND git rev-parse --short HEAD
        WORKING_DIRECTORY "${SRC_DIR}"
        OUTPUT_VARIABLE GIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE GIT_RESULT
    )
    if(NOT GIT_RESULT EQUAL 0)
        set(GIT_HASH "unknown")
    endif()
else()
    set(GIT_HASH "${ACE_VERSION_HASH}")
endif()

if(NOT ACE_VERSION_DATE)
    execute_process(
        COMMAND git show -s --format=%cs HEAD
        WORKING_DIRECTORY "${SRC_DIR}"
        OUTPUT_VARIABLE GIT_DATE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE DATE_RESULT
    )
    if(NOT DATE_RESULT EQUAL 0)
        set(GIT_DATE "unknown")
    endif()
else()
    set(GIT_DATE "${ACE_VERSION_DATE}")
endif()

set(CONTENT "#pragma once\n#define ACE_VERSION \"${GIT_HASH} (${GIT_DATE})\"\n")

if(EXISTS "${OUTPUT}")
    file(READ "${OUTPUT}" EXISTING)
    if("${EXISTING}" STREQUAL "${CONTENT}")
        return()
    endif()
endif()

file(WRITE "${OUTPUT}" "${CONTENT}")
