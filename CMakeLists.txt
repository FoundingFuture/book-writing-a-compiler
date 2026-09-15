cmake_minimum_required(VERSION 3.20)

project(antic
    VERSION 0.1.0
    DESCRIPTION "Compiler for the Anti language"
    LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# One warning set for clang, gcc and MSVC. Warnings are errors so that
# the code in the book compiles without noise on every platform.
if(MSVC)
    add_compile_options(/W4 /WX /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Werror
        -Wshadow -Wconversion -Wstrict-prototypes)
endif()

# The host target name, in the form of the runtime archive directories
# lib/<os>-<cpu>. The file src/target.c detects the same name from the
# compiler's predefined macros. The antic_host_target test compares both.
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(ANTIC_HOST_OS macos)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(ANTIC_HOST_OS linux)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(ANTIC_HOST_OS windows)
else()
    message(FATAL_ERROR "antic does not support the host system ${CMAKE_SYSTEM_NAME}")
endif()
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64)$")
    set(ANTIC_HOST_CPU arm64)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    set(ANTIC_HOST_CPU x86_64)
else()
    message(FATAL_ERROR "antic does not support the host processor ${CMAKE_SYSTEM_PROCESSOR}")
endif()
set(ANTIC_HOST_TARGET "${ANTIC_HOST_OS}-${ANTIC_HOST_CPU}")

# The compiler's code is a library so that the unit tests link the same
# objects as the antic executable.
add_library(antic_core STATIC
    src/antl.c
    src/arm64.c
    src/arena.c
    src/ast_dump.c
    src/diagnostic.c
    src/driver.c
    src/emit.c
    src/header.c
    src/ir.c
    src/ir_print.c
    src/ir_verify.c
    src/layout.c
    src/lexer.c
    src/linker.c
    src/lower.c
    src/mach.c
    src/modpath.c
    src/notice.c
    src/optimize.c
    src/parser.c
    src/process.c
    src/regalloc.c
    src/select.c
    src/sema.c
    src/target.c
    src/text.c
    src/types.c
    src/x86_64.c)
target_include_directories(antic_core PUBLIC src)

add_executable(antic
    src/main.c)
target_link_libraries(antic PRIVATE antic_core)

target_compile_definitions(antic PRIVATE
    ANTIC_VERSION="${PROJECT_VERSION}")
target_compile_definitions(antic_core PRIVATE
    ANTIC_VERSION="${PROJECT_VERSION}")

# anti_rt for the host, laid out as the runtime archive: lib/<target>/ with
# libanti_rt.a, or anti_rt.lib on Windows, and licenses/ with the licence
# of each component.
set(ANTIC_RUNTIME_DIR "${CMAKE_BINARY_DIR}/runtime")
add_library(anti_rt STATIC
    rt/init.c
    rt/io.c
    rt/license.c
    rt/start.c
    rt/text.c
    rt/utf.c)
set_target_properties(anti_rt PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${ANTIC_RUNTIME_DIR}/lib/${ANTIC_HOST_TARGET}"
    C_VISIBILITY_PRESET hidden)
if(APPLE)
    # The minimum macOS version of both macOS targets, the same value as
    # MACOS_MIN_MAJOR and MACOS_MIN_MINOR in src/target.h. The linker warns
    # when an object was built for a newer version than the executable.
    target_compile_options(anti_rt PRIVATE -mmacosx-version-min=11.0)
endif()
# DESIGN: sanitizers check antic. Anti programs link the runtime without a
# sanitizer runtime, so the runtime carries no sanitizer instrumentation,
# whatever CMAKE_C_FLAGS holds in a sanitizer configuration.
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(anti_rt PRIVATE -fno-sanitize=all)
endif()
configure_file(rt/LICENSE "${ANTIC_RUNTIME_DIR}/licenses/anti_rt.txt" COPYONLY)

# The sysroot of each target that lld links against, which
# tools/get-sysroot.sh installs, goes to sysroot/<target> of the runtime
# archive with the licences of its components.
set(ANTIC_SYSROOT_DIR "${CMAKE_SOURCE_DIR}/build/sysroot" CACHE PATH
    "sysroots from tools/get-sysroot.sh")
find_program(ANTIC_LLVM_AR NAMES llvm-ar
    HINTS "${CMAKE_SOURCE_DIR}/build/toolchain/bin"
    DOC "llvm-ar from LLVM 23.1.1")
set(ANTIC_TARGETS linux-x86_64 linux-arm64 macos-x86_64 macos-arm64
    windows-x86_64 windows-arm64)
foreach(target IN LISTS ANTIC_TARGETS)
    if(EXISTS "${ANTIC_SYSROOT_DIR}/${target}")
        file(COPY "${ANTIC_SYSROOT_DIR}/${target}"
            DESTINATION "${ANTIC_RUNTIME_DIR}/sysroot")
    endif()
endforeach()
if(EXISTS "${ANTIC_SYSROOT_DIR}/licenses")
    file(COPY "${ANTIC_SYSROOT_DIR}/licenses/" DESTINATION
        "${ANTIC_RUNTIME_DIR}/licenses")
endif()

# DESIGN: clang compiles the runtime for every other target that has a
# sysroot, so that antic links a program for each of them from this host.
# Linux takes the headers of musl, macOS the SDK of the host and Windows
# the headers that xwin installs.
function(anti_cross_runtime target triple)
    set(objects "")
    set(work "${CMAKE_BINARY_DIR}/cross/${target}")
    if(target MATCHES "^windows")
        set(library "${ANTIC_RUNTIME_DIR}/lib/${target}/anti_rt.lib")
    else()
        set(library "${ANTIC_RUNTIME_DIR}/lib/${target}/libanti_rt.a")
    endif()
    foreach(source init io license start text utf)
        set(object "${work}/${source}.o")
        add_custom_command(OUTPUT "${object}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${work}"
            COMMAND "${CMAKE_C_COMPILER}" --target=${triple} -std=c11 -O2
                -Wall -Wextra -Wpedantic -Werror -fvisibility=hidden ${ARGN}
                -c "${PROJECT_SOURCE_DIR}/rt/${source}.c" -o "${object}"
            DEPENDS "${PROJECT_SOURCE_DIR}/rt/${source}.c"
                "${PROJECT_SOURCE_DIR}/rt/rt.h" "${PROJECT_SOURCE_DIR}/rt/std.h"
                "${PROJECT_SOURCE_DIR}/rt/utf.h"
            VERBATIM)
        list(APPEND objects "${object}")
    endforeach()
    add_custom_command(OUTPUT "${library}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${ANTIC_RUNTIME_DIR}/lib/${target}"
        COMMAND "${ANTIC_LLVM_AR}" rcs "${library}" ${objects}
        DEPENDS ${objects}
        VERBATIM)
    add_custom_target(anti_rt_${target} ALL DEPENDS "${library}")
endfunction()

if(CMAKE_C_COMPILER_ID MATCHES "Clang" AND ANTIC_LLVM_AR)
    execute_process(COMMAND "${CMAKE_C_COMPILER}" -print-resource-dir
        OUTPUT_VARIABLE clang_resource OUTPUT_STRIP_TRAILING_WHITESPACE)
    foreach(target IN LISTS ANTIC_TARGETS)
        set(sysroot "${ANTIC_RUNTIME_DIR}/sysroot/${target}")
        if(target STREQUAL ANTIC_HOST_TARGET OR NOT EXISTS "${sysroot}")
            continue()
        endif()
        if(target STREQUAL "linux-x86_64" OR target STREQUAL "linux-arm64")
            string(REPLACE "linux-arm64" "aarch64" arch "${target}")
            string(REPLACE "linux-x86_64" "x86_64" arch "${arch}")
            anti_cross_runtime(${target} ${arch}-linux-musl -fPIC -nostdinc
                -isystem "${sysroot}/usr/include"
                -isystem "${clang_resource}/include")
        elseif(APPLE AND target STREQUAL "macos-x86_64")
            anti_cross_runtime(${target} x86_64-apple-macos11)
        elseif(APPLE AND target STREQUAL "macos-arm64")
            anti_cross_runtime(${target} arm64-apple-macos11)
        elseif(target MATCHES "^windows")
            string(REPLACE "windows-arm64" "aarch64" arch "${target}")
            string(REPLACE "windows-x86_64" "x86_64" arch "${arch}")
            anti_cross_runtime(${target} ${arch}-pc-windows-msvc
                -isystem "${sysroot}/crt/include"
                -isystem "${sysroot}/sdk/include/ucrt"
                -isystem "${sysroot}/sdk/include/um"
                -isystem "${sysroot}/sdk/include/shared")
        endif()
    endforeach()
endif()

# The standard library in std/, written as library files into std/ of the
# runtime archive, where antic finds the modules under anti. without -I.
set(ANTIC_STD_MODULES io text license)
set(std_files "")
foreach(module IN LISTS ANTIC_STD_MODULES)
    set(output "${ANTIC_RUNTIME_DIR}/std/anti/${module}.antl")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${ANTIC_RUNTIME_DIR}/std/anti"
        COMMAND antic -c --anti-internal -I "${PROJECT_SOURCE_DIR}/std"
            --package-name anti.std --package-version "${PROJECT_VERSION}"
            --license 0BSD --license-text "${PROJECT_SOURCE_DIR}/std/LICENSE"
            -o "${output}" "${PROJECT_SOURCE_DIR}/std/anti/${module}.anti"
        DEPENDS antic "${PROJECT_SOURCE_DIR}/std/anti/${module}.anti"
        VERBATIM)
    list(APPEND std_files "${output}")
endforeach()
add_custom_target(anti_std ALL DEPENDS ${std_files})

enable_testing()
add_subdirectory(tests)
