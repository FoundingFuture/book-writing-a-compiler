# Install llvm-mc, lld, llvm-ar, llvm-objdump and llvm-readobj from the
# pinned LLVM release for this host into <dir>/bin. llvm-mc assembles, lld
# links under the names ld.lld, ld64.lld and lld-link, and llvm-ar writes
# static libraries. llvm-objdump reads the format and architecture of an
# output, and llvm-readobj decodes the Windows unwind data for the tests.
#
#   cmake -DDEST=<dir> -P tools/get-llvm.cmake
#
# The release is the one in tools/llvm-version, and tools/llvm-pin names its
# archive per host. CMake downloads, checks the digest and unpacks, so the
# same script runs on macOS, Linux and Windows without a shell. Set ARCHIVE
# to a file that is already downloaded to skip the download. The digest is
# checked either way.
cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED DEST)
    message(FATAL_ERROR "usage: cmake -DDEST=<dir> -P tools/get-llvm.cmake")
endif()

set(TOOLS llvm-mc llvm-ar llvm-objdump llvm-readobj lld ld.lld ld64.lld
          lld-link)

file(READ "${CMAKE_CURRENT_LIST_DIR}/llvm-version" version)
string(STRIP "${version}" version)
# The host is named as the six targets of antic are, so one spelling
# serves the pin, the archive of the runtime and the option --target.
cmake_host_system_information(RESULT os QUERY OS_NAME)
cmake_host_system_information(RESULT platform QUERY OS_PLATFORM)
string(TOLOWER "${os}" os)
string(TOLOWER "${platform}" platform)
if(platform MATCHES "^(arm64|aarch64)$")
    set(platform arm64)
elseif(platform MATCHES "^(x86_64|amd64|x64)$")
    set(platform x86_64)
endif()
set(host "${os}-${platform}")

foreach(key url digest)
    file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/llvm-pin" line
         REGEX "^${host}-${key}=")
    string(REGEX REPLACE "^${host}-${key}=" "" ${key} "${line}")
    string(REPLACE "@VERSION@" "${version}" ${key} "${${key}}")
endforeach()
if(url STREQUAL "" OR digest STREQUAL "")
    message(FATAL_ERROR
            "tools/llvm-pin has no archive of LLVM ${version} for ${host}. "
            "Every host we publish has one, so this is a host we do not "
            "publish or a pin that lost a row.")
endif()
get_filename_component(asset "${url}" NAME)

file(MAKE_DIRECTORY "${DEST}/bin")
if(NOT DEFINED ARCHIVE)
    set(ARCHIVE "${DEST}/${asset}")
    file(DOWNLOAD "${url}" "${ARCHIVE}" STATUS status SHOW_PROGRESS)
    list(GET status 0 code)
    list(GET status 1 text)
    if(NOT code EQUAL 0)
        message(FATAL_ERROR "${asset}: ${text}")
    endif()
endif()
file(SHA256 "${ARCHIVE}" actual)
if(NOT actual STREQUAL digest)
    message(FATAL_ERROR "${ARCHIVE}: SHA-256 ${actual}, expected ${digest}")
endif()

# The archive holds every LLVM tool. Unpack the eight that antic needs and
# leave the rest in the archive. A pattern that matches nothing is an error,
# so the suffix of the host decides the names.
if(CMAKE_HOST_WIN32)
    set(exe ".exe")
endif()
# The pattern fits our archive, which holds bin/ at its root, and an
# upstream release archive, which holds one directory above it.
set(patterns "")
foreach(tool IN LISTS TOOLS)
    list(APPEND patterns "*bin/${tool}${exe}")
endforeach()
set(unpacked "${DEST}/unpacked")
file(REMOVE_RECURSE "${unpacked}")
file(ARCHIVE_EXTRACT INPUT "${ARCHIVE}" DESTINATION "${unpacked}"
     PATTERNS ${patterns})
file(GLOB found "${unpacked}/bin/*" "${unpacked}/*/bin/*")
if(found STREQUAL "")
    message(FATAL_ERROR "${ARCHIVE} holds no tool of ${TOOLS}")
endif()
file(COPY ${found} DESTINATION "${DEST}/bin")
file(REMOVE_RECURSE "${unpacked}")

# lld answers to its four names through argv[0]. Our archive carries one
# copy, because Windows has no symbolic link without a privilege.
foreach(name ld.lld ld64.lld lld-link)
    if(NOT EXISTS "${DEST}/bin/${name}${exe}")
        execute_process(COMMAND "${CMAKE_COMMAND}" -E copy
                                "${DEST}/bin/lld${exe}"
                                "${DEST}/bin/${name}${exe}"
                        RESULT_VARIABLE copied)
        if(NOT copied EQUAL 0)
            message(FATAL_ERROR "${name}${exe}: the copy of lld failed")
        endif()
    endif()
endforeach()

execute_process(COMMAND "${CMAKE_COMMAND}" "-DLLVM_BIN=${DEST}/bin"
                        -P "${CMAKE_CURRENT_LIST_DIR}/check-llvm.cmake"
                RESULT_VARIABLE checked)
if(NOT checked EQUAL 0)
    message(FATAL_ERROR "the tools in ${DEST}/bin are not LLVM ${version}")
endif()
