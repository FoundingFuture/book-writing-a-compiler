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
    # DESIGN: the C runtime of Microsoft deprecates fopen, getenv and the
    # other functions that C11 defines, and the warnings are errors here.
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Werror
        -Wshadow -Wconversion -Wstrict-prototypes)
    # DESIGN: debug information and __FILE__ carry the absolute path of
    # this checkout into every file a user receives. The map replaces it
    # with a dot, so a program that links anti_rt names no directory of
    # the machine that built it. The test no_paths checks the result.
    add_compile_options("-ffile-prefix-map=${CMAKE_BINARY_DIR}=."
                        "-ffile-prefix-map=${CMAKE_SOURCE_DIR}=.")
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
    src/selfpath.c
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
# DESIGN: one list of runtime sources. The host library and the cross
# builds both read it, so a new source reaches every target at once.
# license_stub.c is not here, because it sits beside the library.
set(ANTIC_RUNTIME_SOURCES assert atomic cast errno init io license object
    reflect signal start text threads time toml utf)
set(anti_rt_files "")
foreach(source IN LISTS ANTIC_RUNTIME_SOURCES)
    list(APPEND anti_rt_files "rt/${source}.c")
endforeach()
add_library(anti_rt STATIC ${anti_rt_files})
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

# The licence stub of a bundled archive lies beside the runtime library as
# an object of its own, so that a program never links it instead of the
# licence text of rt/license.c.
add_library(anti_rt_license_stub OBJECT rt/license_stub.c)
if(APPLE)
    target_compile_options(anti_rt_license_stub PRIVATE
        -mmacosx-version-min=11.0)
endif()
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(anti_rt_license_stub PRIVATE -fno-sanitize=all)
endif()
set_target_properties(anti_rt_license_stub PROPERTIES C_VISIBILITY_PRESET hidden)
add_custom_target(anti_rt_license_stub_copy ALL
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${ANTIC_RUNTIME_DIR}/lib/${ANTIC_HOST_TARGET}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_OBJECTS:anti_rt_license_stub>"
        "${ANTIC_RUNTIME_DIR}/lib/${ANTIC_HOST_TARGET}/anti_rt_license_stub${CMAKE_C_OUTPUT_EXTENSION}"
    DEPENDS anti_rt_license_stub
    COMMAND_EXPAND_LISTS
    VERBATIM)

# The sysroot of each target that lld links against, which
# tools/get-sysroot.cmake installs, goes to sysroot/<target> of the runtime
# archive with the licences of its components.
set(ANTIC_SYSROOT_DIR "${CMAKE_SOURCE_DIR}/build/sysroot" CACHE PATH
    "sysroots from tools/get-sysroot.cmake")
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

# The SDK that a cross compile for macOS reads. Apple clang has a default
# and a clang of the LLVM release has none, so xcrun names it.
if(APPLE)
    set(ANTIC_MACOS_SDK "${CMAKE_OSX_SYSROOT}")
    if(ANTIC_MACOS_SDK STREQUAL "")
        execute_process(COMMAND xcrun --show-sdk-path
                        OUTPUT_VARIABLE ANTIC_MACOS_SDK
                        OUTPUT_STRIP_TRAILING_WHITESPACE)
    endif()
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
    foreach(source IN LISTS ANTIC_RUNTIME_SOURCES)
        set(object "${work}/${source}.o")
        add_custom_command(OUTPUT "${object}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${work}"
            COMMAND "${CMAKE_C_COMPILER}" --target=${triple} -std=c11 -O2
                -Wall -Wextra -Wpedantic -Werror -fvisibility=hidden
                "-ffile-prefix-map=${CMAKE_SOURCE_DIR}=." ${ARGN}
                -c "${PROJECT_SOURCE_DIR}/rt/${source}.c" -o "${object}"
            DEPENDS "${PROJECT_SOURCE_DIR}/rt/${source}.c"
                "${PROJECT_SOURCE_DIR}/rt/rt.h" "${PROJECT_SOURCE_DIR}/rt/std.h"
                "${PROJECT_SOURCE_DIR}/rt/object.h"
                "${PROJECT_SOURCE_DIR}/rt/atomic.h"
                "${PROJECT_SOURCE_DIR}/rt/reflect.h"
                "${PROJECT_SOURCE_DIR}/rt/signal.h"
                "${PROJECT_SOURCE_DIR}/rt/toml.h"
                "${PROJECT_SOURCE_DIR}/rt/utf.h"
            VERBATIM)
        list(APPEND objects "${object}")
    endforeach()
    if(target MATCHES "^windows")
        set(stub "${ANTIC_RUNTIME_DIR}/lib/${target}/anti_rt_license_stub.obj")
    else()
        set(stub "${ANTIC_RUNTIME_DIR}/lib/${target}/anti_rt_license_stub.o")
    endif()
    add_custom_command(OUTPUT "${stub}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${ANTIC_RUNTIME_DIR}/lib/${target}"
        COMMAND "${CMAKE_C_COMPILER}" --target=${triple} -std=c11 -O2
            -Wall -Wextra -Wpedantic -Werror -fvisibility=hidden
            "-ffile-prefix-map=${CMAKE_SOURCE_DIR}=." ${ARGN}
            -c "${PROJECT_SOURCE_DIR}/rt/license_stub.c" -o "${stub}"
        DEPENDS "${PROJECT_SOURCE_DIR}/rt/license_stub.c"
            "${PROJECT_SOURCE_DIR}/rt/std.h"
        VERBATIM)
    # The stub stays beside the library, never a member of it.
    add_custom_command(OUTPUT "${library}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${ANTIC_RUNTIME_DIR}/lib/${target}"
        COMMAND "${ANTIC_LLVM_AR}" rcs "${library}" ${objects}
        DEPENDS ${objects}
        VERBATIM)
    add_custom_target(anti_rt_${target} ALL DEPENDS "${library}" "${stub}")
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
            # DESIGN: a clang from the LLVM release has no default SDK, so
            # the cross compile names the one of this Mac.
            anti_cross_runtime(${target} x86_64-apple-macos11
                -isysroot "${ANTIC_MACOS_SDK}")
        elseif(APPLE AND target STREQUAL "macos-arm64")
            anti_cross_runtime(${target} arm64-apple-macos11
                -isysroot "${ANTIC_MACOS_SDK}")
        elseif(target MATCHES "^windows")
            string(REPLACE "windows-arm64" "aarch64" arch "${target}")
            string(REPLACE "windows-x86_64" "x86_64" arch "${arch}")
            # DESIGN: the link line takes the Universal CRT of the machine
            # with ucrt.lib, which MSVC spells /MD. The objects of the
            # runtime declare the same model, so that the C library of the
            # image is one library rather than two.
            anti_cross_runtime(${target} ${arch}-pc-windows-msvc
                -fms-runtime-lib=dll
                -isystem "${sysroot}/crt/include"
                -isystem "${sysroot}/sdk/include/ucrt"
                -isystem "${sysroot}/sdk/include/um"
                -isystem "${sysroot}/sdk/include/shared")
        endif()
    endforeach()
endif()

# The standard library in std/, written as library files into std/ of the
# runtime archive, where antic finds the modules under anti. without -I.
set(ANTIC_STD_MODULES io text license error time os reflect random collection toml args json log)
set(std_files "")
foreach(module IN LISTS ANTIC_STD_MODULES)
    set(output "${ANTIC_RUNTIME_DIR}/std/anti/${module}.antl")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${ANTIC_RUNTIME_DIR}/std/anti"
        COMMAND antic -c --anti-internal -I "${PROJECT_SOURCE_DIR}/std"
            -I "${ANTIC_RUNTIME_DIR}/std"
            --package-name anti --package-version "${PROJECT_VERSION}"
            --license 0BSD --license-text "${PROJECT_SOURCE_DIR}/std/LICENSE"
            -o "${output}" "${PROJECT_SOURCE_DIR}/std/anti/${module}.anti"
        DEPENDS antic "${PROJECT_SOURCE_DIR}/std/anti/${module}.anti"
            ${std_files}
        VERBATIM)
    list(APPEND std_files "${output}")
endforeach()
add_custom_target(anti_std ALL DEPENDS ${std_files})

enable_testing()
add_subdirectory(tests)
