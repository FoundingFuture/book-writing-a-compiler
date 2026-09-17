# Download the pinned raylib release source into <dir>/raylib-<version>.
#
#   cmake -DDEST=<dir> -P tools/get-raylib.cmake
#
# CMake downloads, checks the digest and unpacks the archive itself, so the
# same script runs on macOS, Linux and Windows without a shell.
cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED DEST)
    message(FATAL_ERROR "usage: cmake -DDEST=<dir> -P tools/get-raylib.cmake")
endif()

foreach(key VERSION DIGEST)
    file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/raylib-pin" line
         REGEX "^RAYLIB_${key}=")
    string(REGEX REPLACE "^RAYLIB_${key}=" "" RAYLIB_${key} "${line}")
    if(RAYLIB_${key} STREQUAL "")
        message(FATAL_ERROR "tools/raylib-pin has no RAYLIB_${key}")
    endif()
endforeach()

set(url
    "https://github.com/raysan5/raylib/archive/refs/tags/${RAYLIB_VERSION}.tar.gz")
set(archive "${DEST}/raylib-${RAYLIB_VERSION}.tar.gz")
file(MAKE_DIRECTORY "${DEST}")
file(DOWNLOAD "${url}" "${archive}" STATUS status SHOW_PROGRESS
     EXPECTED_HASH "SHA256=${RAYLIB_DIGEST}")
list(GET status 0 code)
list(GET status 1 text)
if(NOT code EQUAL 0)
    message(FATAL_ERROR "${url}: ${text}")
endif()

file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${DEST}")
message(STATUS "${DEST}/raylib-${RAYLIB_VERSION}")
