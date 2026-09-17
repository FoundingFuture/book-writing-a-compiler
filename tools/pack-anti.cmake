# Build the package that a user installs, one per host.
#
#   cmake -DDEST=<dir> -DCLANG=<clang> -DLLVM_BIN=<dir> -DSYSROOT=<dir>
#         -DRUNTIME=<dir> -DHOSTS=<host>[;<host>] -P tools/pack-anti.cmake
#
# CLANG is clang of the pinned LLVM release, LLVM_BIN its tools, SYSROOT
# the directory of tools/get-sysroot.cmake and RUNTIME the runtime that the
# CMake build wrote. For each host it compiles antic for that host and lays
# the package out around it:
#
#   bin/        antic and the five LLVM tools of the host
#   lib/<t>/    the runtime library of all six targets
#   std/        the standard library
#   sysroot/    the two Linux sysroots, which are ours to redistribute
#   tools/      the scripts that install the sysroot of the host
#   licenses/   one file per component
#
# The result is anti-<version>-<host>.tar.xz in DEST, with its digest in
# SHA256SUMS. The macOS SDK stubs and the Microsoft CRT stay out, because
# neither licence allows redistribution. The installer adds them.
cmake_minimum_required(VERSION 3.20)

foreach(name DEST CLANG LLVM_BIN LLVM_PACKS SYSROOT RUNTIME HOSTS)
    if(NOT DEFINED ${name})
        message(FATAL_ERROR "usage: cmake -DDEST=<dir> -DCLANG=<clang> "
                            "-DLLVM_BIN=<dir> -DLLVM_PACKS=<dir> "
                            "-DSYSROOT=<dir> -DRUNTIME=<dir> "
                            "-DHOSTS=<host>[;<host>] -P tools/pack-anti.cmake")
    endif()
endforeach()

set(TOOLS llvm-mc llvm-ar llvm-objdump llvm-readobj lld ld.lld ld64.lld
          lld-link)
set(TARGETS macos-arm64 macos-x86_64 linux-x86_64 linux-arm64
            windows-x86_64 windows-arm64)

set(tools_dir "${CMAKE_CURRENT_LIST_DIR}")
get_filename_component(root "${tools_dir}/.." ABSOLUTE)
file(READ "${root}/tools/llvm-version" llvm_version)
string(STRIP "${llvm_version}" llvm_version)
file(STRINGS "${root}/CMakeLists.txt" line REGEX "^    VERSION ")
string(REGEX REPLACE "^    VERSION " "" version "${line}")
execute_process(COMMAND "${CLANG}" -print-resource-dir
                OUTPUT_VARIABLE resource OUTPUT_STRIP_TRAILING_WHITESPACE)

function(triple_of host out)
    set(triples
        "macos-arm64=arm64-apple-macos11"
        "macos-x86_64=x86_64-apple-macos11"
        "linux-x86_64=x86_64-unknown-linux-musl"
        "linux-arm64=aarch64-unknown-linux-musl"
        "windows-x86_64=x86_64-pc-windows-msvc"
        "windows-arm64=aarch64-pc-windows-msvc")
    foreach(row IN LISTS triples)
        if(row MATCHES "^${host}=(.*)$")
            set("${out}" "${CMAKE_MATCH_1}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    message(FATAL_ERROR "unknown host ${host}")
endfunction()

# Compile and link antic for one host. Each family of targets reads its
# headers from a different place: the SDK of this Mac, the musl sysroot, or
# the Microsoft headers that xwin wrote.
function(build_antic host output)
    triple_of("${host}" triple)
    file(GLOB sources "${root}/src/*.c")
    set(common --target=${triple} -std=c11 -O2 -Wall -Wextra -Wpedantic
               -Werror "-ffile-prefix-map=${root}=."
               "-DANTIC_VERSION=\"${version}\"" -I "${root}/src")
    set(link "")
    if(host MATCHES "^macos-")
        execute_process(COMMAND xcrun --show-sdk-path OUTPUT_VARIABLE sdk
                        OUTPUT_STRIP_TRAILING_WHITESPACE)
        list(APPEND common -isysroot "${sdk}")
        set(link --ld-path=${LLVM_BIN}/ld64.lld)
    elseif(host MATCHES "^linux-")
        list(APPEND common --sysroot "${SYSROOT}/${host}")
    else()
        set(win "${SYSROOT}/${host}")
        # antic is a Windows program too, and it takes the C runtime of
        # the machine as the programs it compiles do.
        list(APPEND common -D_CRT_SECURE_NO_WARNINGS -fms-runtime-lib=dll
             -isystem "${resource}/include" -isystem "${win}/crt/include"
             -isystem "${win}/sdk/include/ucrt"
             -isystem "${win}/sdk/include/um"
             -isystem "${win}/sdk/include/shared")
        set(arch x86_64)
        if(host STREQUAL "windows-arm64")
            set(arch aarch64)
        endif()
        set(link -fuse-ld=lld -B "${LLVM_BIN}"
                 -L "${win}/crt/lib/${arch}" -L "${win}/sdk/lib/ucrt/${arch}"
                 -L "${win}/sdk/lib/um/${arch}")
    endif()
    if(host MATCHES "^linux-")
        # The clang driver looks for the start files of gcc on Linux, so
        # the objects go to ld.lld with the musl ones instead.
        set(objects "")
        foreach(source IN LISTS sources)
            get_filename_component(name "${source}" NAME_WE)
            set(object "${DEST}/work/${host}/${name}.o")
            execute_process(COMMAND "${CLANG}" ${common} -c -o "${object}"
                                    "${source}" RESULT_VARIABLE failed)
            if(failed)
                message(FATAL_ERROR "${host}: ${name}.c did not compile")
            endif()
            list(APPEND objects "${object}")
        endforeach()
        set(lib "${SYSROOT}/${host}/usr/lib")
        execute_process(
            COMMAND "${LLVM_BIN}/ld.lld" -static -pie --no-dynamic-linker
                    -o "${output}" "${lib}/rcrt1.o" "${lib}/crti.o" ${objects}
                    "${lib}/libc.a" "${lib}/libclang_rt.builtins.a"
                    "${lib}/crtn.o"
            RESULT_VARIABLE failed)
    else()
        execute_process(COMMAND "${CLANG}" ${common} ${link} -o "${output}"
                                ${sources} RESULT_VARIABLE failed)
    endif()
    if(failed)
        message(FATAL_ERROR "${host}: antic did not link")
    endif()
endfunction()

set(sums "")
foreach(host IN LISTS HOSTS)
    set(work "${DEST}/work/${host}")
    set(tree "${work}/anti")
    file(REMOVE_RECURSE "${work}")
    file(MAKE_DIRECTORY "${tree}/bin" "${tree}/tools" "${tree}/licenses")

    set(suffix "")
    if(host MATCHES "^windows-")
        set(suffix ".exe")
    endif()
    build_antic("${host}" "${tree}/bin/antic${suffix}")

    # The LLVM tools of the host come from the archive of that host, not
    # from the tools of this machine. On Windows lld is one binary under
    # four names, and the archive carries one copy.
    set(pack "${LLVM_PACKS}/anti-llvm-${llvm_version}-${host}.tar.xz")
    if(EXISTS "${pack}")
        file(ARCHIVE_EXTRACT INPUT "${pack}" DESTINATION "${work}/llvm")
        file(GLOB tools "${work}/llvm/bin/*")
        file(COPY ${tools} DESTINATION "${tree}/bin")
        file(RENAME "${tree}/bin/LICENSE.TXT" "${tree}/licenses/llvm.txt")
        # On Windows lld answers to four names as four copies of one
        # binary, which no compressor folds back together. The archive
        # carries one, and the installer writes the other names.
    else()
        message(FATAL_ERROR
                "${pack} is missing. Every host has an archive of the tools, "
                "so run tools/pack-llvm.cmake for ${host} first.")
    endif()
    foreach(target IN LISTS TARGETS)
        file(COPY "${RUNTIME}/lib/${target}" DESTINATION "${tree}/lib")
    endforeach()
    file(COPY "${RUNTIME}/std" DESTINATION "${tree}")
    file(COPY "${RUNTIME}/licenses/" DESTINATION "${tree}/licenses")
    foreach(target linux-x86_64 linux-arm64)
        file(COPY "${SYSROOT}/${target}" DESTINATION "${tree}/sysroot")
    endforeach()
    file(COPY "${SYSROOT}/licenses/" DESTINATION "${tree}/licenses")
    foreach(name get-sysroot.cmake sysroot-pins cmake-pin cmake-version
            llvm-version package-api)
        file(COPY "${root}/tools/${name}" DESTINATION "${tree}/tools")
    endforeach()
    file(COPY "${root}/LICENSE" DESTINATION "${tree}")

    set(packed "${DEST}/anti-${version}-${host}.tar.xz")
    file(REMOVE "${packed}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E chdir "${work}"
                            "${CMAKE_COMMAND}" -E tar cJf "${packed}" anti
                    RESULT_VARIABLE failed)
    if(failed)
        message(FATAL_ERROR "${packed}: cmake -E tar failed")
    endif()
    file(SHA256 "${packed}" digest)
    file(SIZE "${packed}" size)
    get_filename_component(name "${packed}" NAME)
    string(APPEND sums "${digest}  ${name}\n")
    message(STATUS "${packed} ${size} bytes")
    file(REMOVE_RECURSE "${work}")
endforeach()
# DESIGN: the manifest covers the directory on the server. A run of one
# host keeps the lines of the packages that DEST already holds, so that
# publishing one host does not drop the other five from SHA256SUMS.
if(EXISTS "${DEST}/SHA256SUMS")
    file(STRINGS "${DEST}/SHA256SUMS" old_lines)
    set(kept "")
    foreach(line IN LISTS old_lines)
        string(REGEX REPLACE "^[0-9a-f]+  " "" name "${line}")
        if(NOT sums MATCHES "  ${name}\n")
            string(APPEND kept "${line}\n")
        endif()
    endforeach()
    set(sums "${kept}${sums}")
endif()
file(WRITE "${DEST}/SHA256SUMS" "${sums}")
