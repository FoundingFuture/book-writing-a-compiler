# Build the archive of LLVM tools for every host and write tools/llvm-pin.
#
#   cmake -DDEST=<dir> -DLLVM_BIN=<dir> \
#         -DBUILT_linux-arm64=<dir> -DBUILT_linux-x86_64=<dir> \
#         -P tools/pack-llvm.cmake
#
# For a host that tools/llvm-upstream names with a URL it downloads the
# LLVM release archive, checks its digest, and takes the eight tools that
# antic needs with the LLVM licence. A host it names as built takes them
# from BUILT_<host>, the build directory of tools/build-llvm.cmake.
#
# It writes anti-llvm-<version>-<host>.tar.xz into DEST, the rows of
# tools/llvm-pin for the hosts it packed, and a SHA256SUMS of every host
# the pin names. HOSTS packs a subset. A user downloads one archive
# instead of the whole release, 46 MB instead of 1.6 GB on macOS. Upload
# what DEST holds, and the burden of a new LLVM version stays here.
cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED DEST)
    message(FATAL_ERROR "usage: cmake -DDEST=<dir> -P tools/pack-llvm.cmake")
endif()

set(TOOLS llvm-mc llvm-ar llvm-objdump llvm-readobj lld ld.lld ld64.lld
          lld-link)
# tools/build-llvm.cmake writes one lld, and this script links the three
# other names to it.
set(BUILT_TOOLS lld llvm-mc llvm-ar llvm-objdump llvm-readobj)

set(tools_dir "${CMAKE_CURRENT_LIST_DIR}")
file(READ "${tools_dir}/llvm-version" version)
string(STRIP "${version}" version)
file(STRINGS "${tools_dir}/download-base" base REGEX "^https://")
set(RELEASE "${base}/llvm/@VERSION@")

# Download url to file and stop unless its digest is the expected one.
function(fetch url file digest)
    if(NOT EXISTS "${file}")
        file(DOWNLOAD "${url}" "${file}" STATUS status SHOW_PROGRESS)
        list(GET status 0 code)
        list(GET status 1 text)
        if(NOT code EQUAL 0)
            file(REMOVE "${file}")
            message(FATAL_ERROR "${url}: ${text}")
        endif()
    endif()
    file(SHA256 "${file}" actual)
    if(NOT actual STREQUAL digest)
        message(FATAL_ERROR "${file}: SHA-256 ${actual}, expected ${digest}")
    endif()
endfunction()

file(STRINGS "${tools_dir}/llvm-upstream" rows
     REGEX "^[a-z]+-[a-z0-9_]+-(url|built)=")
if(NOT DEFINED HOSTS)
    set(HOSTS "all")
endif()
set(pin "")
set(packed_hosts "")
foreach(row IN LISTS rows)
    string(REGEX MATCH "^([a-z]+-[a-z0-9_]+)-(url|built)=(.*)$" matched "${row}")
    set(host "${CMAKE_MATCH_1}")
    set(kind "${CMAKE_MATCH_2}")
    set(url "${CMAKE_MATCH_3}")
    if(NOT HOSTS STREQUAL "all" AND NOT host IN_LIST HOSTS)
        continue()
    endif()

    set(work "${DEST}/work/${host}")
    file(REMOVE_RECURSE "${work}")
    file(MAKE_DIRECTORY "${work}")
    set(exe "")
    if(host MATCHES "^windows-")
        set(exe ".exe")
    endif()

    if(kind STREQUAL "built")
        # The tools of a Linux host come from tools/build-llvm.cmake, and
        # BUILT_<host> names the directory it wrote. The archive carries
        # one lld under three more names, as the release archives do.
        set(built "${BUILT_${host}}")
        if(built STREQUAL "")
            message(FATAL_ERROR
                    "${host} is built here, so pass -DBUILT_${host}=<dir>, "
                    "the build directory of tools/build-llvm.cmake")
        endif()
        file(MAKE_DIRECTORY "${work}/bin")
        foreach(tool IN LISTS BUILT_TOOLS)
            if(NOT EXISTS "${built}/bin/${tool}")
                message(FATAL_ERROR "${built}/bin/${tool} does not exist")
            endif()
            file(COPY "${built}/bin/${tool}" DESTINATION "${work}/bin"
                 FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                                  GROUP_READ GROUP_EXECUTE
                                  WORLD_READ WORLD_EXECUTE)
        endforeach()
        foreach(name ld.lld ld64.lld lld-link)
            file(CREATE_LINK lld "${work}/bin/${name}" SYMBOLIC)
        endforeach()
    else()
        string(REPLACE "@VERSION@" "${version}" url "${url}")
        file(STRINGS "${tools_dir}/llvm-upstream" line REGEX "^${host}-digest=")
        string(REGEX REPLACE "^${host}-digest=" "" digest "${line}")

        get_filename_component(asset "${url}" NAME)
        fetch("${url}" "${DEST}/upstream/${asset}" "${digest}")

        set(patterns "")
        foreach(tool IN LISTS TOOLS)
            list(APPEND patterns "*/bin/${tool}${exe}")
        endforeach()
        file(ARCHIVE_EXTRACT INPUT "${DEST}/upstream/${asset}"
             DESTINATION "${work}" PATTERNS ${patterns})
        file(GLOB found "${work}/*/bin/*")
        list(LENGTH found count)
        list(LENGTH TOOLS wanted)
        if(NOT count EQUAL wanted)
            message(FATAL_ERROR
                    "${asset} holds ${count} of the ${wanted} tools")
        endif()
        file(COPY ${found} DESTINATION "${work}/bin")
    endif()

    # DESIGN: a tool we publish must run on a machine that holds nothing
    # but its own system libraries. The official Linux build of LLVM needs
    # ICU of one Ubuntu release, which no other distribution carries, and
    # that failure reaches a user rather than us.
    if(host MATCHES "^macos-" AND kind STREQUAL "built")
        file(GLOB binaries "${work}/bin/*")
        foreach(binary IN LISTS binaries)
            execute_process(COMMAND "${LLVM_BIN}/llvm-objdump" --macho
                                    --dylibs-used "${binary}"
                            OUTPUT_VARIABLE used ERROR_QUIET)
            string(REGEX MATCHALL "/usr/lib/[^ \n]+\\.dylib" libraries "${used}")
            foreach(library IN LISTS libraries)
                if(NOT library MATCHES "^/usr/lib/(libSystem\\.B|libc\\+\\+\\.1)\\.dylib$")
                    get_filename_component(name "${binary}" NAME)
                    message(FATAL_ERROR
                            "${name} of ${host} needs ${library}, which a "
                            "Mac may not hold. Publish a tool that needs "
                            "the libraries of the system alone.")
                endif()
            endforeach()
        endforeach()
    endif()
    if(host MATCHES "^linux-")
        file(GLOB binaries "${work}/bin/*")
        foreach(binary IN LISTS binaries)
            execute_process(COMMAND "${LLVM_BIN}/llvm-readobj" --needed-libs
                                    "${binary}"
                            OUTPUT_VARIABLE needed ERROR_QUIET)
            string(REGEX MATCHALL "lib[^ \n]+\\.so[^ \n]*" libraries
                   "${needed}")
            foreach(library IN LISTS libraries)
                if(NOT library MATCHES
                   "^(libc|libm|libdl|libpthread|librt|libgcc_s|libstdc\\+\\+|libz)\\.so")
                    get_filename_component(name "${binary}" NAME)
                    message(FATAL_ERROR
                            "${name} of ${host} needs ${library}, which a "
                            "machine may not hold. Publish a tool that "
                            "needs the system libraries alone.")
                endif()
            endforeach()
        endforeach()
    endif()

    # lld answers to its four names through argv[0], and the Windows
    # release ships four copies of one binary. Keep one, and let
    # get-llvm.cmake write the other names.
    if(NOT exe STREQUAL "")
        file(SHA256 "${work}/bin/lld${exe}" one)
        foreach(name ld.lld ld64.lld lld-link)
            file(SHA256 "${work}/bin/${name}${exe}" other)
            if(NOT other STREQUAL one)
                message(FATAL_ERROR "${name}${exe} differs from lld${exe}")
            endif()
            file(REMOVE "${work}/bin/${name}${exe}")
        endforeach()
    endif()

    # Apache 2.0 with LLVM exceptions asks for the licence beside the
    # binaries it covers. The release archives hold no copy of it.
    file(STRINGS "${tools_dir}/llvm-upstream" line REGEX "^license-url=")
    string(REGEX REPLACE "^license-url=" "" license "${line}")
    string(REPLACE "@VERSION@" "${version}" license "${license}")
    file(STRINGS "${tools_dir}/llvm-upstream" line REGEX "^license-digest=")
    string(REGEX REPLACE "^license-digest=" "" license_digest "${line}")
    fetch("${license}" "${work}/bin/LICENSE.TXT" "${license_digest}")

    # cmake -E tar from the work directory, so the archive holds bin/ and
    # not the path of this machine. file(ARCHIVE_CREATE) stores the path it
    # is given.
    set(packed "${DEST}/anti-llvm-${version}-${host}.tar.xz")
    file(REMOVE "${packed}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E chdir "${work}"
                            "${CMAKE_COMMAND}" -E tar cJf "${packed}" bin
                    RESULT_VARIABLE tarred)
    if(NOT tarred EQUAL 0)
        message(FATAL_ERROR "${packed}: cmake -E tar failed")
    endif()
    file(SHA256 "${packed}" packed_digest)
    file(SIZE "${packed}" size)
    message(STATUS "${packed} ${size} bytes")
    get_filename_component(packed_name "${packed}" NAME)
    string(REPLACE "${version}" "@VERSION@" packed_name "${packed_name}")
    string(APPEND pin "${host}-url=${RELEASE}/${packed_name}\n")
    string(APPEND pin "${host}-digest=${packed_digest}\n")
    list(APPEND packed_hosts "${host}")
    file(REMOVE_RECURSE "${work}")
endforeach()

# The rows of the hosts that were packed replace their old ones, and
# every other host keeps the archive it already has. A host is packed
# alone when only its tools changed, and the others are not rebuilt.
file(STRINGS "${tools_dir}/llvm-pin" old_rows)
set(written "")
foreach(line IN LISTS old_rows)
    if(line MATCHES "^([a-z]+-[a-z0-9_]+)-(url|digest)=")
        if(CMAKE_MATCH_1 IN_LIST packed_hosts)
            continue()
        endif()
    endif()
    string(APPEND written "${line}\n")
endforeach()
string(APPEND written "${pin}")
file(WRITE "${tools_dir}/llvm-pin" "${written}")
message(STATUS "tools/llvm-pin holds ${packed_hosts}")

# DESIGN: the manifest covers the directory on the server, so it is
# written from the pin rather than from this run. A run of one host would
# otherwise publish a manifest that names one archive of five.
set(sums "")
file(STRINGS "${tools_dir}/llvm-pin" pin_rows REGEX "^[a-z]+-[a-z0-9_]+-url=")
foreach(line IN LISTS pin_rows)
    string(REGEX MATCH "^([a-z]+-[a-z0-9_]+)-url=(.*)$" matched "${line}")
    set(host "${CMAKE_MATCH_1}")
    get_filename_component(name "${CMAKE_MATCH_2}" NAME)
    string(REPLACE "@VERSION@" "${version}" name "${name}")
    file(STRINGS "${tools_dir}/llvm-pin" line REGEX "^${host}-digest=")
    string(REGEX REPLACE "^${host}-digest=" "" digest "${line}")
    string(APPEND sums "${digest}  ${name}\n")
endforeach()
file(WRITE "${DEST}/SHA256SUMS" "${sums}")
