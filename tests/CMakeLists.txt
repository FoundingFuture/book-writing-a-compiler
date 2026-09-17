# Unit tests link the compiler's code directly. Program tests in
# tests/programs compile .anti files with antic and run the result.

add_executable(antic_unit_tests
    unit/test_main.c
    unit/test_target.c
    unit/test_text.c
    unit/test_lexer.c
    unit/test_parser.c
    unit/test_types.c
    unit/test_sema.c
    unit/test_ir.c
    unit/test_layout.c
    unit/test_lower.c
    unit/test_modules.c
    unit/test_header.c
    unit/test_optimize.c
    unit/test_select.c
    unit/test_regalloc.c
    unit/test_x86_64.c
    unit/test_arm64.c
    unit/test_emit.c
    unit/test_link.c
    unit/test_float.c
    unit/test_struct.c
    unit/test_utf.c
    ../rt/utf.c)
target_link_libraries(antic_unit_tests PRIVATE antic_core)
target_include_directories(antic_unit_tests PRIVATE ../rt)
target_compile_definitions(antic_unit_tests PRIVATE
    ANTIC_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_test(NAME unit COMMAND antic_unit_tests)

# The runtime archive holds the licence text of every component.
add_test(NAME runtime_licenses
    COMMAND "${CMAKE_COMMAND}" -E cat "${ANTIC_RUNTIME_DIR}/licenses/anti_rt.txt")
set_tests_properties(runtime_licenses PROPERTIES
    PASS_REGULAR_EXPRESSION "^BSD Zero Clause License")

# CI runs only by hand: every workflow triggers on workflow_dispatch alone.
add_test(NAME workflows_dispatch_only
    COMMAND "${CMAKE_COMMAND}"
        "-DWORKFLOWS=${PROJECT_SOURCE_DIR}/.github/workflows"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_workflows.cmake")

add_test(NAME antic_version COMMAND antic --version)
set_tests_properties(antic_version PROPERTIES
    PASS_REGULAR_EXPRESSION "^antic [0-9]+\\.[0-9]+\\.[0-9]+")

# The host target name has two spellings: ANTIC_HOST_TARGET here and the
# detection in src/target.c. This test fails when they disagree.
add_test(NAME antic_host_target COMMAND antic --print-host-target)
set_tests_properties(antic_host_target PROPERTIES
    PASS_REGULAR_EXPRESSION "^${ANTIC_HOST_TARGET}\n$")

# One test per program in tests/programs
find_program(ANTIC_LLVM_MC NAMES llvm-mc DOC "llvm-mc from LLVM 23.1.1")

# The runtime archive carries llvm-mc, lld and llvm-ar of the LLVM release
# in tools/llvm-version, which the build copies to runtime/bin.
if(ANTIC_LLVM_MC)
    get_filename_component(ANTIC_LLVM_BIN "${ANTIC_LLVM_MC}" DIRECTORY)
    add_custom_target(llvm_tools ALL
        COMMAND "${CMAKE_COMMAND}" "-DLLVM_BIN=${ANTIC_LLVM_BIN}"
            "-DDEST=${ANTIC_RUNTIME_DIR}/bin"
            -P "${PROJECT_SOURCE_DIR}/tools/check-llvm.cmake")
    add_test(NAME toolchain_pin
        COMMAND "${CMAKE_COMMAND}" "-DLLVM_BIN=${ANTIC_RUNTIME_DIR}/bin"
            -P "${PROJECT_SOURCE_DIR}/tools/check-llvm.cmake")
endif()

file(GLOB program_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/programs/*.anti")
foreach(source IN LISTS program_sources)
    get_filename_component(name "${source}" NAME_WE)
    add_test(NAME program_${name}
        COMMAND "${CMAKE_COMMAND}"
            "-DANTIC=$<TARGET_FILE:antic>"
            "-DLLVM_MC=${ANTIC_LLVM_MC}"
            "-DRUNTIME=${ANTIC_RUNTIME_DIR}"
            "-DSOURCE=${source}"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/programs"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run_program.cmake")
endforeach()

# The same C files compiled for macos-x86_64, which the ABI programs of
# that target link. The host runs them, so antic meets clang on the System
# V rules of x86_64 as well.
function(abi_x86_64_object name source)
    add_library(${name} OBJECT "${source}")
    target_compile_options(${name} PRIVATE -arch x86_64
        -mmacosx-version-min=11.0 -fno-sanitize=all)
endfunction()

# A macos-arm64 host runs a macos-x86_64 program as well, so every program
# runs for that target too. The other four targets need their own machine.
if(ANTIC_HOST_TARGET STREQUAL "macos-arm64")
    abi_x86_64_object(abi_structs_x86_64 abi/structs.c)
    abi_x86_64_object(abi_wchar_x86_64 abi/wchar.c)
    foreach(case structs wchar)
        add_test(NAME program_abi_${case}_macos-x86_64
            COMMAND "${CMAKE_COMMAND}"
                "-DANTIC=$<TARGET_FILE:antic>"
                "-DLLVM_MC=${ANTIC_LLVM_MC}"
                "-DRUNTIME=${ANTIC_RUNTIME_DIR}"
                "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/abi/abi_${case}.anti"
                "-DOBJECTS=$<JOIN:$<TARGET_OBJECTS:abi_${case}_x86_64>,,>"
                "-DOPTIONS=--target,macos-x86_64"
                "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/abi-x86_64"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/run_program.cmake")
    endforeach()
    if(EXISTS "${ANTIC_RAYLIB_DIR}/src/raymath.h")
        abi_x86_64_object(abi_raymath_x86_64 abi/raymath.c)
        target_include_directories(abi_raymath_x86_64 PRIVATE
            "${ANTIC_RAYLIB_DIR}/src")
        add_test(NAME program_abi_raymath_macos-x86_64
            COMMAND "${CMAKE_COMMAND}"
                "-DANTIC=$<TARGET_FILE:antic>"
                "-DLLVM_MC=${ANTIC_LLVM_MC}"
                "-DRUNTIME=${ANTIC_RUNTIME_DIR}"
                "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/abi/abi_raymath.anti"
                "-DOBJECTS=$<JOIN:$<TARGET_OBJECTS:abi_raymath_x86_64>,,>"
                "-DOPTIONS=--target,macos-x86_64"
                "-DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/abi/abi_raymath.macos-x86_64.expected"
                "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/abi-x86_64"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/run_program.cmake")
    endif()
    foreach(source IN LISTS program_sources)
        get_filename_component(name "${source}" NAME_WE)
        add_test(NAME program_${name}_macos-x86_64
            COMMAND "${CMAKE_COMMAND}"
                "-DANTIC=$<TARGET_FILE:antic>"
                "-DLLVM_MC=${ANTIC_LLVM_MC}"
                "-DRUNTIME=${ANTIC_RUNTIME_DIR}"
                "-DSOURCE=${source}"
                "-DOPTIONS=--target,macos-x86_64"
                "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/programs-x86_64"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/run_program.cmake")
    endforeach()
    set_tests_properties(program_args_macos-x86_64 PROPERTIES
        ENVIRONMENT "ANTI_TEST_GREETING=héllo")
endif()

# The environment variable that tests/programs/args.anti looks for.
set_tests_properties(program_args PROPERTIES
    ENVIRONMENT "ANTI_TEST_GREETING=héllo")

# The modules of the standard library in std/, which the build writes as
# library files into std/ of the runtime archive. Each test program imports
# one of them.
file(GLOB std_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/std/*.anti")
foreach(source IN LISTS std_sources)
    get_filename_component(name "${source}" NAME_WE)
    add_test(NAME std_${name}
        COMMAND "${CMAKE_COMMAND}"
            "-DANTIC=$<TARGET_FILE:antic>"
            "-DLLVM_MC=${ANTIC_LLVM_MC}"
            "-DRUNTIME=${ANTIC_RUNTIME_DIR}"
            "-DSOURCE=${source}"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/std"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run_program.cmake")
endforeach()

# Struct passing checked against C: the functions of tests/abi/structs.c,
# compiled by the C compiler of the build, take and return structs by value.
add_library(abi_structs OBJECT abi/structs.c)
if(APPLE)
    target_compile_options(abi_structs PRIVATE -mmacosx-version-min=11.0)
endif()
# Anti programs link these objects without a sanitizer runtime, like the
# runtime library itself.
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(abi_structs PRIVATE -fno-sanitize=all)
endif()
add_test(NAME program_abi_structs
    COMMAND "${CMAKE_COMMAND}"
        "-DANTIC=$<TARGET_FILE:antic>"
        "-DLLVM_MC=${ANTIC_LLVM_MC}"
        "-DRUNTIME=${ANTIC_RUNTIME_DIR}"
        "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/abi/abi_structs.anti"
        "-DOBJECTS=$<JOIN:$<TARGET_OBJECTS:abi_structs>,,>"
        "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/abi"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_program.cmake")

# c_wchar above 0x7FFFFFFF converts and compares with the signedness of
# wchar_t on the host, as clang does. The expected output holds the values
# of macOS, where wchar_t is a signed int.
add_library(abi_wchar OBJECT abi/wchar.c)
if(APPLE)
    target_compile_options(abi_wchar PRIVATE -mmacosx-version-min=11.0)
endif()
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(abi_wchar PRIVATE -fno-sanitize=all)
endif()
# DESIGN: wchar_t is signed on macOS and on Linux x86_64, and unsigned on
# Linux ARM64. The program prints what the host gives, so a host whose
# signedness differs from the shared file brings one of its own.
set(abi_wchar_expected "${CMAKE_CURRENT_SOURCE_DIR}/abi/abi_wchar.expected")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/abi/abi_wchar.${ANTIC_HOST_TARGET}.expected")
    set(abi_wchar_expected
        "${CMAKE_CURRENT_SOURCE_DIR}/abi/abi_wchar.${ANTIC_HOST_TARGET}.expected")
endif()
add_test(NAME program_abi_wchar
    COMMAND "${CMAKE_COMMAND}"
        "-DANTIC=$<TARGET_FILE:antic>"
        "-DLLVM_MC=${ANTIC_LLVM_MC}"
        "-DRUNTIME=${ANTIC_RUNTIME_DIR}"
        "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/abi/abi_wchar.anti"
        "-DOBJECTS=$<JOIN:$<TARGET_OBJECTS:abi_wchar>,,>"
        "-DEXPECTED=${abi_wchar_expected}"
        "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/abi"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_program.cmake")

# A small raylib binding: the raymath functions of the pinned raylib
# release, which tools/get-raylib.cmake downloads. tools/raylib-pin holds
# the version, so the default directory below cannot name another one.
# A user receives these files. None of them may name a directory of the
# machine that built it.
find_program(ANTIC_STRINGS strings)
if(ANTIC_STRINGS)
    add_test(NAME no_paths
        COMMAND "${CMAKE_COMMAND}" "-DROOT=${CMAKE_SOURCE_DIR}"
                "-DRUNTIME=${ANTIC_RUNTIME_DIR}" "-DSTRINGS=${ANTIC_STRINGS}"
                "-DANTIC=$<TARGET_FILE:antic>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/run_no_paths.cmake")
endif()

foreach(name llvm cmake)
    add_test(NAME ${name}_pin
        COMMAND "${CMAKE_COMMAND}" "-DROOT=${CMAKE_SOURCE_DIR}" "-DNAME=${name}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/run_pin.cmake")
endforeach()
add_test(NAME raylib_pin
    COMMAND "${CMAKE_COMMAND}" "-DROOT=${CMAKE_SOURCE_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run_raylib_pin.cmake")
add_test(NAME package_api
    COMMAND "${CMAKE_COMMAND}" "-DROOT=${CMAKE_SOURCE_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run_package_api.cmake")
add_test(NAME line_endings
    COMMAND "${CMAKE_COMMAND}" "-DROOT=${CMAKE_SOURCE_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run_line_endings.cmake")
add_test(NAME installer_options
    COMMAND "${CMAKE_COMMAND}" "-DROOT=${CMAKE_SOURCE_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run_installer_options.cmake")

# The index of the excerpts leaves out a fence that no file of the
# repository holds, and the test of that fence then stops running. The
# script says so with its exit code, and this test reads it.
find_program(ANTIC_PYTHON NAMES python3 python)
if(ANTIC_PYTHON)
    add_test(NAME excerpts_complete
        COMMAND "${CMAKE_COMMAND}" "-DROOT=${CMAKE_SOURCE_DIR}"
                "-DPYTHON=${ANTIC_PYTHON}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/run_excerpts_complete.cmake")
endif()
file(STRINGS "${CMAKE_SOURCE_DIR}/tools/raylib-pin" raylib_version
     REGEX "^RAYLIB_VERSION=")
string(REGEX REPLACE "^RAYLIB_VERSION=" "" raylib_version "${raylib_version}")
set(ANTIC_RAYLIB_DIR "${CMAKE_BINARY_DIR}/raylib/raylib-${raylib_version}"
    CACHE PATH "raylib release source from tools/get-raylib.cmake")
if(EXISTS "${ANTIC_RAYLIB_DIR}/src/raymath.h")
    add_library(abi_raymath OBJECT abi/raymath.c)
    target_include_directories(abi_raymath PRIVATE "${ANTIC_RAYLIB_DIR}/src")
    if(APPLE)
        target_compile_options(abi_raymath PRIVATE -mmacosx-version-min=11.0)
    endif()
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(abi_raymath PRIVATE -fno-sanitize=all)
    endif()
    # DESIGN: raymath calls the maths library of the host, and two of its
    # twenty-one values differ in the ninth digit between Apple's libm and
    # glibc. A host whose values differ brings an expected file of its own.
    set(raymath_expected
        "${CMAKE_CURRENT_SOURCE_DIR}/abi/abi_raymath.expected")
    if(EXISTS
       "${CMAKE_CURRENT_SOURCE_DIR}/abi/abi_raymath.${ANTIC_HOST_TARGET}.expected")
        set(raymath_expected
            "${CMAKE_CURRENT_SOURCE_DIR}/abi/abi_raymath.${ANTIC_HOST_TARGET}.expected")
    endif()
    add_test(NAME program_abi_raymath
        COMMAND "${CMAKE_COMMAND}"
            "-DANTIC=$<TARGET_FILE:antic>"
            "-DLLVM_MC=${ANTIC_LLVM_MC}"
            "-DRUNTIME=${ANTIC_RUNTIME_DIR}"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/abi/abi_raymath.anti"
            "-DOBJECTS=$<JOIN:$<TARGET_OBJECTS:abi_raymath>,,>"
            "-DEXPECTED=${raymath_expected}"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/abi"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run_program.cmake")
else()
    # Fail rather than skip, so a missing raylib cannot pass unnoticed.
    add_test(NAME program_abi_raymath
        COMMAND "${CMAKE_COMMAND}" -E echo
            "raylib not found in ${ANTIC_RAYLIB_DIR}: run tools/get-raylib.cmake")
    set_tests_properties(program_abi_raymath PROPERTIES
        FAIL_REGULAR_EXPRESSION "raylib not found")
endif()

# Diagnostics. Each test passes when antic's output matches the pattern.
add_test(NAME error_missing_semicolon
    COMMAND antic -S -o "${CMAKE_CURRENT_BINARY_DIR}/errors/missing_semicolon.s"
            "${CMAKE_CURRENT_SOURCE_DIR}/errors/missing_semicolon.anti")
set_tests_properties(error_missing_semicolon PROPERTIES
    PASS_REGULAR_EXPRESSION "missing_semicolon.anti:4:1: error: expected `[;]`")
# A ';' in a property value separates list elements, so a pattern writes
# the semicolon as [;].

# The runtime's module path belongs to no compilation, the anti root needs
# --anti-internal for a library, and a library path of one segment warns.
add_test(NAME error_runtime_module
    COMMAND antic -S -I "${CMAKE_CURRENT_SOURCE_DIR}/errors"
            -o "${CMAKE_CURRENT_BINARY_DIR}/errors/rt.s"
            "${CMAKE_CURRENT_SOURCE_DIR}/errors/anti/rt.anti")
set_tests_properties(error_runtime_module PROPERTIES
    PASS_REGULAR_EXPRESSION "the module path `anti.rt` is the runtime's")

add_test(NAME error_reserved_library
    COMMAND antic -c -I "${CMAKE_CURRENT_SOURCE_DIR}/modules"
            -o "${CMAKE_CURRENT_BINARY_DIR}/anti_text.antl"
            "${CMAKE_CURRENT_SOURCE_DIR}/modules/anti/text.anti")
set_tests_properties(error_reserved_library PROPERTIES
    PASS_REGULAR_EXPRESSION "the module path `anti.text` is reserved for the language's own libraries")

add_test(NAME antl_internal
    COMMAND antic -c --anti-internal -I "${CMAKE_CURRENT_SOURCE_DIR}/modules"
            -o "${CMAKE_CURRENT_BINARY_DIR}/anti_text.antl"
            "${CMAKE_CURRENT_SOURCE_DIR}/modules/anti/text.anti")

# Doc warnings come only with --doc-warnings, which anti check passes.
add_test(NAME warning_note
    COMMAND antic --doc-warnings -S -o "${CMAKE_CURRENT_BINARY_DIR}/note.s"
            "${CMAKE_CURRENT_SOURCE_DIR}/errors/note.anti")
set_tests_properties(warning_note PROPERTIES
    PASS_REGULAR_EXPRESSION "note.anti:2:8: warning: the pub item `warm` has a `//#` note and no `///` comment")
add_test(NAME silent_note
    COMMAND antic -S -o "${CMAKE_CURRENT_BINARY_DIR}/note_silent.s"
            "${CMAKE_CURRENT_SOURCE_DIR}/errors/note.anti")
set_tests_properties(silent_note PROPERTIES FAIL_REGULAR_EXPRESSION "warning")

add_test(NAME warning_dropped
    COMMAND antic --doc-warnings -S -o "${CMAKE_CURRENT_BINARY_DIR}/dropped.s"
            "${CMAKE_CURRENT_SOURCE_DIR}/errors/dropped.anti")
set_tests_properties(warning_dropped PROPERTIES
    PASS_REGULAR_EXPRESSION "dropped.anti:3:2: warning: the `/\\*\\*` comment is dropped, because no item or field follows it")
add_test(NAME silent_dropped
    COMMAND antic -S -o "${CMAKE_CURRENT_BINARY_DIR}/dropped_silent.s"
            "${CMAKE_CURRENT_SOURCE_DIR}/errors/dropped.anti")
set_tests_properties(silent_dropped PROPERTIES FAIL_REGULAR_EXPRESSION "warning")

add_test(NAME warning_one_segment
    COMMAND antic -c -o "${CMAKE_CURRENT_BINARY_DIR}/geometry.antl"
            "${CMAKE_CURRENT_SOURCE_DIR}/errors/geometry.anti")
set_tests_properties(warning_one_segment PROPERTIES
    PASS_REGULAR_EXPRESSION "geometry.anti: warning: the module path `geometry` has one segment, which is for a program's own files")

add_test(NAME error_no_main
    COMMAND antic --runtime "${ANTIC_RUNTIME_DIR}"
            -o "${CMAKE_CURRENT_BINARY_DIR}/errors/no_main"
            "${CMAKE_CURRENT_SOURCE_DIR}/errors/no_main.anti")
set_tests_properties(error_no_main PROPERTIES
    PASS_REGULAR_EXPRESSION "no_main.anti: the program has no function `main`")

# lld links a program for every target on this host. The test of a
# target without a sysroot in the runtime archive is skipped. The second
# program holds an aggregate constant with an address in it, which lands
# in the section each format keeps for data the loader writes.
foreach(case "linux-x86_64|elf64-x86-64" "linux-arm64|elf64-littleaarch64"
        "macos-x86_64|mach-o 64-bit x86-64" "macos-arm64|mach-o arm64"
        "windows-x86_64|coff-x86-64" "windows-arm64|coff-arm64")
    string(REPLACE "|" ";" parts "${case}")
    list(GET parts 0 target)
    list(GET parts 1 format)
    foreach(program return42 const_data)
        set(suffix "")
        if(NOT program STREQUAL "return42")
            set(suffix "_${program}")
        endif()
        add_test(NAME cross_link_${target}${suffix}
            COMMAND "${CMAKE_COMMAND}"
                "-DANTIC=$<TARGET_FILE:antic>"
                "-DLLVM_MC=${ANTIC_LLVM_MC}"
                "-DLLVM_OBJDUMP=${ANTIC_RUNTIME_DIR}/bin/llvm-objdump"
                "-DRUNTIME=${ANTIC_RUNTIME_DIR}"
                "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/programs/${program}.anti"
                "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/cross"
                "-DTARGET=${target}"
                "-DFORMAT=${format}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cross_link.cmake")
        set_tests_properties(cross_link_${target}${suffix} PROPERTIES
            SKIP_REGULAR_EXPRESSION "SKIP: ")
    endforeach()
endforeach()

# The platform linker of the host links the first executable of chapter 3.
if(NOT WIN32)
    add_test(NAME program_platform_linker
        COMMAND "${CMAKE_COMMAND}"
            "-DANTIC=$<TARGET_FILE:antic>"
            "-DLLVM_MC=${ANTIC_LLVM_MC}"
            "-DRUNTIME=${ANTIC_RUNTIME_DIR}"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/programs/return42.anti"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/platform"
            "-DOPTIONS=--linker,platform"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run_program.cmake")
endif()

# The platform linker links only for the operating system it runs on.
if(ANTIC_HOST_OS STREQUAL "linux")
    set(foreign_target macos-arm64)
else()
    set(foreign_target linux-x86_64)
endif()
add_test(NAME error_foreign_link
    COMMAND antic --target ${foreign_target} --linker platform
            --runtime "${ANTIC_RUNTIME_DIR}" --llvm-mc "${ANTIC_LLVM_MC}"
            -o "${CMAKE_CURRENT_BINARY_DIR}/errors/return42"
            "${CMAKE_CURRENT_SOURCE_DIR}/programs/return42.anti")
set_tests_properties(error_foreign_link PROPERTIES
    PASS_REGULAR_EXPRESSION "linking for ${foreign_target} with the platform linker needs a host with the same operating system")

# The back end folds an array length from size_of and names the target.
add_test(NAME error_array_length
    COMMAND antic -S --target linux-x86_64
            -o "${CMAKE_CURRENT_BINARY_DIR}/errors/zero_length.s"
            "${CMAKE_CURRENT_SOURCE_DIR}/errors/zero_length.anti")
set_tests_properties(error_array_length PROPERTIES
    PASS_REGULAR_EXPRESSION "antic: the array length `size_of\\(Pair\\) - 8` is 0 on linux-x86_64, and an array length is at least 1")

add_test(NAME error_missing_runtime
    COMMAND antic -o "${CMAKE_CURRENT_BINARY_DIR}/errors/return42"
            "${CMAKE_CURRENT_SOURCE_DIR}/programs/return42.anti")
set_tests_properties(error_missing_runtime PROPERTIES
    PASS_REGULAR_EXPRESSION "antic: linking needs --runtime")

# Dumps of compiler stages, compared with expected files in tests/dump.
# scale.tokens is the token listing that chapter 1 prints.
add_test(NAME dump_tokens_scale
    COMMAND "${CMAKE_COMMAND}"
        "-DANTIC=$<TARGET_FILE:antic>"
        "-DOPTION=--dump-tokens"
        "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/dump/scale.anti"
        "-DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/dump/scale.tokens"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_dump.cmake")

# A doc comment is one token, printed with its first source line.
add_test(NAME dump_tokens_docs
    COMMAND "${CMAKE_COMMAND}"
        "-DANTIC=$<TARGET_FILE:antic>"
        "-DOPTION=--dump-tokens"
        "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/dump/docs.anti"
        "-DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/dump/docs.tokens"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_dump.cmake")

add_test(NAME error_lexical
    COMMAND antic -S -o "${CMAKE_CURRENT_BINARY_DIR}/errors/lexical.s"
            "${CMAKE_CURRENT_SOURCE_DIR}/errors/lexical.anti")
set_tests_properties(error_lexical PROPERTIES
    PASS_REGULAR_EXPRESSION "lexical.anti:3:14: error: unknown escape `\\\\q`\n[^\n]*lexical.anti:4:10: error: a character literal holds one character\n")

# scale.ast is the syntax tree listing that chapter 1 prints.
add_test(NAME dump_ast_scale
    COMMAND "${CMAKE_COMMAND}"
        "-DANTIC=$<TARGET_FILE:antic>"
        "-DOPTION=--dump-ast"
        "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/dump/scale.anti"
        "-DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/dump/scale.ast"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_dump.cmake")

# Two mistakes produce exactly two messages.
add_test(NAME error_syntax
    COMMAND antic -S -o "${CMAKE_CURRENT_BINARY_DIR}/errors/syntax.s"
            "${CMAKE_CURRENT_SOURCE_DIR}/errors/syntax.anti")
set_tests_properties(error_syntax PROPERTIES
    PASS_REGULAR_EXPRESSION "^[^\n]*syntax.anti:3:10: error: expected an expression\n[^\n]*syntax.anti:5:2: error: expected `[;]`\n$")

# scale.types is the typed tree listing that chapter 1 prints.
add_test(NAME dump_types_scale
    COMMAND "${CMAKE_COMMAND}"
        "-DANTIC=$<TARGET_FILE:antic>"
        "-DOPTION=--dump-types"
        "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/dump/scale.anti"
        "-DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/dump/scale.types"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_dump.cmake")

# main.ir is the IR listing that chapter 1 prints for main.anti.
add_test(NAME dump_ir_main
    COMMAND "${CMAKE_COMMAND}"
        "-DANTIC=$<TARGET_FILE:antic>"
        "-DOPTION=--dump-ir"
        "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/dump/main.anti"
        "-DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/dump/main.ir"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_dump.cmake")

# Library files. scale.antl.hex lists the bytes that every host writes for
# scale.anti. main.ir is the whole program after loading two libraries,
# which the search root supplies, beside the build directory's copies.
set(MODULES "${CMAKE_CURRENT_SOURCE_DIR}/modules")
set(LIBS "${CMAKE_CURRENT_BINARY_DIR}/modules")
file(MAKE_DIRECTORY "${LIBS}/com/example")

add_test(NAME antl_scale
    COMMAND "${CMAKE_COMMAND}"
        "-DANTIC=$<TARGET_FILE:antic>"
        "-DSOURCE=${MODULES}/com/example/scale.anti"
        "-DROOT=${MODULES}"
        "-DOPTIONS=--package-name|com.example|--package-version|1.2.4|--license|MIT|--license-text|${MODULES}/LICENSE.txt|--attribution|Copyright 2026 Example|--dependency|com.example.vec,^1.2,https://anti.example.com/repo"
        "-DOUTPUT=${LIBS}/com/example/scale.antl"
        "-DEXPECTED=${MODULES}/scale.antl.hex"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_antl.cmake")
set_tests_properties(antl_scale PROPERTIES FIXTURES_SETUP scale_antl)

# The line form and the block form of the doc comments write one file.
add_test(NAME antl_docs_lines
    COMMAND antic -c -I "${MODULES}/lines" -o "${LIBS}/lines.antl"
            "${MODULES}/lines/com/example/geo.anti")
set_tests_properties(antl_docs_lines PROPERTIES FIXTURES_SETUP docs_lines)
add_test(NAME antl_docs_blocks
    COMMAND antic -c -I "${MODULES}/blocks" -o "${LIBS}/blocks.antl"
            "${MODULES}/blocks/com/example/geo.anti")
set_tests_properties(antl_docs_blocks PROPERTIES FIXTURES_SETUP docs_blocks)
add_test(NAME antl_docs_equivalent
    COMMAND "${CMAKE_COMMAND}" -E compare_files "${LIBS}/lines.antl"
            "${LIBS}/blocks.antl")
set_tests_properties(antl_docs_equivalent PROPERTIES
    FIXTURES_REQUIRED "docs_lines;docs_blocks")

add_test(NAME antl_twice
    COMMAND antic -c -I "${MODULES}" -I "${LIBS}"
            -o "${LIBS}/com/example/twice.antl"
            "${MODULES}/com/example/twice.anti")
set_tests_properties(antl_twice PROPERTIES
    FIXTURES_REQUIRED scale_antl FIXTURES_SETUP twice_antl)

add_test(NAME dump_ir_modules
    COMMAND "${CMAKE_COMMAND}"
        "-DANTIC=$<TARGET_FILE:antic>"
        "-DCOMMAND=--dump-ir,-I,${LIBS},${MODULES}/main.anti"
        "-DEXPECTED=${MODULES}/main.ir"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_dump.cmake")
set_tests_properties(dump_ir_modules PROPERTIES FIXTURES_REQUIRED twice_antl)

# Dev mode: one object per module, linked into the same program.
if(NOT WIN32)
    add_test(NAME dev_modules
        COMMAND "${CMAKE_COMMAND}"
            "-DANTIC=$<TARGET_FILE:antic>"
            "-DLLVM_MC=${ANTIC_LLVM_MC}"
            "-DRUNTIME=${ANTIC_RUNTIME_DIR}"
            "-DMODULES=${MODULES}"
            "-DLIBS=${LIBS}"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/dev"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run_dev.cmake")
    set_tests_properties(dev_modules PROPERTIES FIXTURES_REQUIRED twice_antl)
endif()

add_test(NAME error_missing_library
    COMMAND antic --dump-ir "${MODULES}/main.anti"
            "${LIBS}/com/example/twice.antl")
set_tests_properties(error_missing_library PROPERTIES
    FIXTURES_REQUIRED twice_antl
    PASS_REGULAR_EXPRESSION "twice.antl needs module `com.example.scale`")

add_test(NAME error_library_cycle
    COMMAND antic -c -I "${MODULES}" -o "${LIBS}/cycle.antl"
            "${MODULES}/com/example/scale.anti"
            "${LIBS}/com/example/twice.antl")
set_tests_properties(error_library_cycle PROPERTIES
    FIXTURES_REQUIRED twice_antl
    PASS_REGULAR_EXPRESSION "twice.antl depends on `com.example.scale`, so the import forms a cycle")

add_test(NAME antl_one
    COMMAND antic -c -I "${MODULES}" -o "${LIBS}/com/example/one.antl"
            "${MODULES}/com/example/one.anti")
set_tests_properties(antl_one PROPERTIES FIXTURES_SETUP one_antl)

# The first library loads, and the second one is stuck on the cycle.
add_test(NAME error_library_cycle_later
    COMMAND antic -c -I "${MODULES}" -o "${LIBS}/cycle.antl"
            "${MODULES}/com/example/scale.anti"
            "${LIBS}/com/example/one.antl" "${LIBS}/com/example/twice.antl")
set_tests_properties(error_library_cycle_later PROPERTIES
    FIXTURES_REQUIRED "one_antl;twice_antl"
    PASS_REGULAR_EXPRESSION "twice.antl depends on `com.example.scale`, so the import forms a cycle")

add_test(NAME error_duplicate_library
    COMMAND antic --dump-ir "${MODULES}/main.anti"
            "${LIBS}/com/example/scale.antl" "${LIBS}/com/example/scale.antl")
set_tests_properties(error_duplicate_library PROPERTIES
    FIXTURES_REQUIRED scale_antl
    PASS_REGULAR_EXPRESSION "scale.antl and [^ ]*scale.antl both hold module `com.example.scale`")

# The ABI probe compares the layouts of C and antic on the host.
if(NOT WIN32)
    add_test(NAME abi_probe
        COMMAND "${CMAKE_COMMAND}"
            "-DANTIC=$<TARGET_FILE:antic>"
            "-DLLVM_MC=${ANTIC_LLVM_MC}"
            "-DRUNTIME=${ANTIC_RUNTIME_DIR}"
            "-DSOURCES=${CMAKE_CURRENT_SOURCE_DIR}/abi"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/probe"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run_probe.cmake")
endif()

# A linked program holds the licence notice of its packages.
if(NOT WIN32)
    add_test(NAME program_licenses
        COMMAND "${CMAKE_COMMAND}"
            "-DANTIC=$<TARGET_FILE:antic>"
            "-DLLVM_MC=${ANTIC_LLVM_MC}"
            "-DRUNTIME=${ANTIC_RUNTIME_DIR}"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/programs/return42.anti"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/licenses"
            "-DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/dump/licenses.notice"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run_licenses.cmake")
endif()

# A program that imports a standard library module carries the package of
# the library, which is the reserved root anti.
if(NOT WIN32)
    add_test(NAME program_licenses_std
        COMMAND "${CMAKE_COMMAND}"
            "-DANTIC=$<TARGET_FILE:antic>"
            "-DLLVM_MC=${ANTIC_LLVM_MC}"
            "-DRUNTIME=${ANTIC_RUNTIME_DIR}"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/std/io.anti"
            "-DWANTED=ANTI_LICENSES_BEGIN|package anti.rt [0-9.]+ 0BSD|package anti [0-9.]+ 0BSD|package com.example.hello 2.0.0 MIT"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/licenses-std"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run_licenses.cmake")
endif()

# Libraries for C, built with antic --lib and used from C programs on the
# host. Windows needs the MSVC toolchain and is left to the CI runners.
find_program(ANTIC_LLVM_AR NAMES llvm-ar
    HINTS "${CMAKE_SOURCE_DIR}/build/toolchain/bin"
    DOC "llvm-ar from LLVM 23.1.1")
if(NOT WIN32 AND ANTIC_LLVM_AR)
    foreach(case static shared exports two loader header bundle)
        add_test(NAME clib_${case}
            COMMAND "${CMAKE_COMMAND}"
                "-DANTIC=$<TARGET_FILE:antic>"
                "-DLLVM_MC=${ANTIC_LLVM_MC}"
                "-DLLVM_AR=${ANTIC_LLVM_AR}"
                "-DRUNTIME=${ANTIC_RUNTIME_DIR}"
                "-DSOURCES=${CMAKE_CURRENT_SOURCE_DIR}/clib"
                "-DDUMP=${CMAKE_CURRENT_SOURCE_DIR}/dump"
                "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/clib"
                "-DCASE=${case}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/run_clib.cmake")
    endforeach()
endif()

# The early code of chapters 1, 3, 4 and 5, which the finished compiler no
# longer holds, lives in the chapter folders as programs of their own. A
# chapter tag without the folder builds and tests none of it.
set(BOOK "${PROJECT_SOURCE_DIR}/writing-a-compiler")
set(DIRECT_03 "${BOOK}/03-setup-and-first-executable/direct-path")
set(DIRECT_WORK "${CMAKE_CURRENT_BINARY_DIR}/direct-path")
if(EXISTS "${DIRECT_03}" AND NOT MSVC)
    # The runtime of chapter 3, the first rt/start.c alone.
    add_library(direct_path_rt STATIC "${DIRECT_03}/start.c")
    set_target_properties(direct_path_rt PROPERTIES
        OUTPUT_NAME anti_rt
        ARCHIVE_OUTPUT_DIRECTORY "${DIRECT_WORK}/runtime/lib/${ANTIC_HOST_TARGET}")
    if(APPLE)
        target_compile_options(direct_path_rt PRIVATE -mmacosx-version-min=11.0)
    endif()
    target_compile_options(direct_path_rt PRIVATE -fno-sanitize=all)
    # Chapter 3 recognises the program in the characters, chapter 4 in the
    # tokens of the lexer and chapter 5 in the syntax tree. All three share
    # the driver, the emitter and the target table of chapter 3.
    set(direct_chapters "03|${DIRECT_03}|"
        "04|${BOOK}/04-lexer/direct-path|arena.c,diagnostic.c,lexer.c"
        "05|${BOOK}/05-parser-and-syntax-tree/direct-path|arena.c,diagnostic.c,lexer.c,parser.c")
    foreach(entry IN LISTS direct_chapters)
        string(REPLACE "|" ";" parts "${entry}")
        list(GET parts 0 chapter)
        list(GET parts 1 folder)
        list(GET parts 2 core)
        if(NOT EXISTS "${folder}")
            continue()
        endif()
        string(REPLACE "," ";" core "${core}")
        list(TRANSFORM core PREPEND "${PROJECT_SOURCE_DIR}/src/")
        add_executable(direct_path_${chapter}
            "${folder}/direct.c"
            "${DIRECT_03}/driver.c"
            "${DIRECT_03}/emit_direct.c"
            "${DIRECT_03}/main.c"
            "${DIRECT_03}/target.c"
            "${PROJECT_SOURCE_DIR}/src/process.c"
            "${PROJECT_SOURCE_DIR}/src/text.c"
            ${core})
        target_include_directories(direct_path_${chapter} PRIVATE
            "${folder}" "${DIRECT_03}" "${PROJECT_SOURCE_DIR}/src")
        target_compile_definitions(direct_path_${chapter} PRIVATE
            ANTIC_VERSION="${PROJECT_VERSION}")
        add_test(NAME direct_path_${chapter}_assembly
            COMMAND "${CMAKE_COMMAND}"
                "-DANTIC=$<TARGET_FILE:direct_path_${chapter}>"
                "-DCOMMAND=-S,--target,macos-arm64,-o,${DIRECT_WORK}/${chapter}/assembly/return42.s,${CMAKE_CURRENT_SOURCE_DIR}/programs/return42.anti"
                "-DOUTPUT=${DIRECT_WORK}/${chapter}/assembly/return42.s"
                "-DEXPECTED=${DIRECT_03}/return42.s"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/run_dump.cmake")
        add_test(NAME direct_path_${chapter}_missing_semicolon
            COMMAND "${CMAKE_COMMAND}"
                "-DANTIC=$<TARGET_FILE:direct_path_${chapter}>"
                "-DDIRECTORY=${CMAKE_CURRENT_SOURCE_DIR}/errors"
                "-DCOMMAND=-S,-o,${DIRECT_WORK}/${chapter}/error/missing_semicolon.s,missing_semicolon.anti"
                "-DEXPECTED=${DIRECT_03}/missing_semicolon.err"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/run_error.cmake")
        # Each test writes into a directory of its own, because the tests
        # of one chapter run in parallel and would share return42.s.
        file(MAKE_DIRECTORY "${DIRECT_WORK}/${chapter}/assembly"
            "${DIRECT_WORK}/${chapter}/error")
        # The direct path links with ld of the Command Line Tools.
        if(ANTIC_HOST_TARGET STREQUAL "macos-arm64" AND ANTIC_LLVM_MC)
            foreach(program return42 return_pieces)
                add_test(NAME direct_path_${chapter}_${program}
                    COMMAND "${CMAKE_COMMAND}"
                        "-DANTIC=$<TARGET_FILE:direct_path_${chapter}>"
                        "-DLLVM_MC=${ANTIC_LLVM_MC}"
                        "-DRUNTIME=${DIRECT_WORK}/runtime"
                        "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/programs/${program}.anti"
                        "-DWORK=${DIRECT_WORK}/${chapter}/run-${program}"
                        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_program.cmake")
            endforeach()
        endif()
    endforeach()
endif()
# The first rt/start.c of chapter 1 runs the program of antic.
set(START_01 "${BOOK}/01-parts-of-a-compiler/start.c")
if(EXISTS "${START_01}" AND NOT WIN32 AND ANTIC_LLVM_MC)
    add_test(NAME start_01_return42
        COMMAND "${CMAKE_COMMAND}"
            "-DANTIC=$<TARGET_FILE:antic>"
            "-DLLVM_MC=${ANTIC_LLVM_MC}"
            "-DSTART=${START_01}"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/programs/return42.anti"
            "-DWORK=${DIRECT_WORK}/01"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run_start.cmake")
endif()

# The Windows unwind data of chapter 16, decoded by llvm-readobj from the
# object that llvm-mc writes.
if(ANTIC_LLVM_MC)
    foreach(target_triple "windows-x86_64,x86_64-pc-windows-msvc"
            "windows-arm64,aarch64-pc-windows-msvc")
        string(REPLACE "," ";" parts "${target_triple}")
        list(GET parts 0 target)
        list(GET parts 1 triple)
        add_test(NAME unwind_${target}
            COMMAND "${CMAKE_COMMAND}"
                "-DANTIC=$<TARGET_FILE:antic>"
                "-DLLVM_MC=${ANTIC_LLVM_MC}"
                "-DLLVM_READOBJ=${ANTIC_RUNTIME_DIR}/bin/llvm-readobj"
                "-DTARGET=${target}"
                "-DTRIPLE=${triple}"
                "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/dump/unwind.anti"
                "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/unwind"
                "-DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/dump/unwind.${target}.unwind"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/run_unwind.cmake")
    endforeach()
endif()

# Every code excerpt of the book appears verbatim in its file. The file
# tests/excerpts.txt lists them, and tools/scripts/excerpts.py writes it.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/excerpts.txt" excerpts)
foreach(excerpt IN LISTS excerpts)
    string(REPLACE "|" ";" parts "${excerpt}")
    list(GET parts 0 bundle)
    list(GET parts 1 index)
    list(GET parts 2 source)
    list(GET parts 3 label)
    string(SUBSTRING "${bundle}" 0 2 chapter)
    add_test(NAME excerpt_${chapter}_${index}
        COMMAND "${CMAKE_COMMAND}"
            "-DCHAPTER=${PROJECT_SOURCE_DIR}/writing-a-compiler/${bundle}/index.md"
            "-DINDEX=${index}"
            "-DSOURCE=${PROJECT_SOURCE_DIR}/${source}"
            "-DLABEL=${label}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run_excerpt.cmake")
endforeach()

# targets.txt is the target matrix that chapter 11 prints.
add_test(NAME print_targets
    COMMAND "${CMAKE_COMMAND}"
        "-DANTIC=$<TARGET_FILE:antic>"
        "-DCOMMAND=--print-targets"
        "-DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/dump/targets.txt"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_dump.cmake")

# main.opt is the optimized IR listing that chapter 1 prints.
add_test(NAME dump_opt_main
    COMMAND "${CMAKE_COMMAND}"
        "-DANTIC=$<TARGET_FILE:antic>"
        "-DOPTION=--dump-opt"
        "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/dump/main.anti"
        "-DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/dump/main.opt"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_dump.cmake")

# The selected machine code of main.anti that chapter 12 prints.
add_test(NAME dump_select_arm64
    COMMAND "${CMAKE_COMMAND}"
        "-DANTIC=$<TARGET_FILE:antic>"
        "-DCOMMAND=--dump-select,--target,macos-arm64,${CMAKE_CURRENT_SOURCE_DIR}/dump/main.anti"
        "-DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/dump/main.arm64.sel"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_dump.cmake")
add_test(NAME dump_select_x86_64
    COMMAND "${CMAKE_COMMAND}"
        "-DANTIC=$<TARGET_FILE:antic>"
        "-DCOMMAND=--dump-select,--target,linux-x86_64,${CMAKE_CURRENT_SOURCE_DIR}/dump/main.anti"
        "-DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/dump/main.x86_64.sel"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_dump.cmake")

# The allocated machine code of main.anti that chapter 13 prints.
add_test(NAME dump_alloc_arm64
    COMMAND "${CMAKE_COMMAND}"
        "-DANTIC=$<TARGET_FILE:antic>"
        "-DCOMMAND=--dump-alloc,--target,macos-arm64,${CMAKE_CURRENT_SOURCE_DIR}/dump/main.anti"
        "-DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/dump/main.arm64.alloc"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_dump.cmake")
add_test(NAME dump_alloc_x86_64
    COMMAND "${CMAKE_COMMAND}"
        "-DANTIC=$<TARGET_FILE:antic>"
        "-DCOMMAND=--dump-alloc,--target,linux-x86_64,${CMAKE_CURRENT_SOURCE_DIR}/dump/main.anti"
        "-DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/dump/main.x86_64.alloc"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_dump.cmake")

add_test(NAME dump_alloc_twice_arm64
    COMMAND "${CMAKE_COMMAND}"
        "-DANTIC=$<TARGET_FILE:antic>"
        "-DCOMMAND=--dump-alloc,--target,macos-arm64,${CMAKE_CURRENT_SOURCE_DIR}/dump/twice.anti"
        "-DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/dump/twice.arm64.alloc"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_dump.cmake")
add_test(NAME dump_alloc_cell_arm64
    COMMAND "${CMAKE_COMMAND}"
        "-DANTIC=$<TARGET_FILE:antic>"
        "-DCOMMAND=--dump-alloc,--target,macos-arm64,${CMAKE_CURRENT_SOURCE_DIR}/dump/cell.anti"
        "-DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/dump/cell.arm64.alloc"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_dump.cmake")

# Spilling: more values live across a call than callee-saved registers.
add_test(NAME dump_alloc_spill_arm64
    COMMAND "${CMAKE_COMMAND}"
        "-DANTIC=$<TARGET_FILE:antic>"
        "-DCOMMAND=--dump-alloc,--target,macos-arm64,${CMAKE_CURRENT_SOURCE_DIR}/dump/spill12.anti"
        "-DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/dump/spill12.arm64.alloc"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_dump.cmake")
add_test(NAME dump_alloc_spill_x86_64
    COMMAND "${CMAKE_COMMAND}"
        "-DANTIC=$<TARGET_FILE:antic>"
        "-DCOMMAND=--dump-alloc,--target,linux-x86_64,${CMAKE_CURRENT_SOURCE_DIR}/dump/spill7.anti"
        "-DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/dump/spill7.x86_64.alloc"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_dump.cmake")

# The back ends of chapters 14 and 15: addressing modes and calls.
foreach(case "sum,linux-x86_64,sum.x86_64" "args,linux-x86_64,args.linux-x86_64"
        "args,windows-x86_64,args.windows-x86_64" "x86,linux-x86_64,x86.x86_64"
        "sum,macos-arm64,sum.arm64" "args,macos-arm64,args.macos-arm64"
        "args,linux-arm64,args.linux-arm64" "x86,linux-arm64,x86.arm64"
        "arm64,macos-arm64,arm64.macos-arm64" "arm64,linux-arm64,arm64.linux-arm64"
        "floatcalls,linux-x86_64,floatcalls.linux-x86_64"
        "floatcalls,windows-x86_64,floatcalls.windows-x86_64"
        "floatcalls,macos-arm64,floatcalls.macos-arm64"
        "floatcalls,windows-arm64,floatcalls.windows-arm64"
        "floatcalls,linux-arm64,floatcalls.linux-arm64"
        "floats,linux-x86_64,floats.linux-x86_64"
        "floats,macos-arm64,floats.macos-arm64"
        "structs,linux-x86_64,structs.linux-x86_64"
        "structs,windows-x86_64,structs.windows-x86_64"
        "fnptr,linux-x86_64,fnptr.linux-x86_64"
        "fnptr,linux-arm64,fnptr.linux-arm64")
    string(REPLACE "," ";" parts "${case}")
    list(GET parts 0 source)
    list(GET parts 1 target)
    list(GET parts 2 expected)
    add_test(NAME dump_alloc_${expected}
        COMMAND "${CMAKE_COMMAND}"
            "-DANTIC=$<TARGET_FILE:antic>"
            "-DCOMMAND=--dump-alloc,--target,${target},${CMAKE_CURRENT_SOURCE_DIR}/dump/${source}.anti"
            "-DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/dump/${expected}.alloc"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run_dump.cmake")
endforeach()

# The three ARM64 conventions pass these structs alike. The Windows frame
# adds its unwind directives, so it has a file of its own.
foreach(target macos-arm64 linux-arm64 windows-arm64)
    set(expected structs.arm64.alloc)
    if(target STREQUAL "windows-arm64")
        set(expected structs.windows-arm64.alloc)
    endif()
    add_test(NAME dump_alloc_structs.${target}
        COMMAND "${CMAKE_COMMAND}"
            "-DANTIC=$<TARGET_FILE:antic>"
            "-DCOMMAND=--dump-alloc,--target,${target},${CMAKE_CURRENT_SOURCE_DIR}/dump/structs.anti"
            "-DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/dump/${expected}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run_dump.cmake")
endforeach()

# Listings of the chapters. The error listings name the file alone, so
# each runs in the directory of its program.
set(LISTINGS "${CMAKE_CURRENT_BINARY_DIR}/listings")
file(MAKE_DIRECTORY "${LISTINGS}")
foreach(case "layout|-S,--target,linux-x86_64,-o,${LISTINGS}/layout.s,layout.anti"
        "sizeof|-S,-o,${LISTINGS}/sizeof.s,sizeof.anti"
        "modifiers|-S,-o,${LISTINGS}/modifiers.s,modifiers.anti"
        "geo|-S,-o,${LISTINGS}/geo.s,geo.anti"
        "tail|-S,--target,windows-x86_64,-o,${LISTINGS}/tail.s,tail.anti")
    string(REPLACE "|" ";" parts "${case}")
    list(GET parts 0 name)
    list(GET parts 1 arguments)
    add_test(NAME listing_error_${name}
        COMMAND "${CMAKE_COMMAND}"
            "-DANTIC=$<TARGET_FILE:antic>"
            "-DDIRECTORY=${CMAKE_CURRENT_SOURCE_DIR}/errors"
            "-DCOMMAND=${arguments}"
            "-DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/errors/${name}.err"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run_error.cmake")
endforeach()
set(DUMP "${CMAKE_CURRENT_SOURCE_DIR}/dump")
foreach(case "symbolic.types|--dump-types,${DUMP}/symbolic.anti"
        "header/main.ir|--dump-ir,${DUMP}/header/main.anti"
        "size_of/main.ir|--dump-ir,${DUMP}/size_of/main.anti"
        "size_of/main.linux-arm64.sel|--dump-select,--target,linux-arm64,${DUMP}/size_of/main.anti"
        "c_long/main.linux-x86_64.sel|--dump-select,--target,linux-x86_64,${DUMP}/c_long/main.anti"
        "c_long/main.windows-x86_64.sel|--dump-select,--target,windows-x86_64,${DUMP}/c_long/main.anti"
        "probe.ir|--dump-ir,${CMAKE_CURRENT_SOURCE_DIR}/abi/probe.anti"
        "sizes.ir|--dump-ir,${CMAKE_CURRENT_SOURCE_DIR}/programs/sizes.anti"
        "sizes.linux-x86_64.alloc|--dump-alloc,--target,linux-x86_64,${CMAKE_CURRENT_SOURCE_DIR}/programs/sizes.anti"
        "bitfields.ir|--dump-ir,${CMAKE_CURRENT_SOURCE_DIR}/programs/bitfields.anti"
        "bitfields.linux-x86_64.alloc|--dump-alloc,--target,linux-x86_64,${CMAKE_CURRENT_SOURCE_DIR}/programs/bitfields.anti"
        "bitfields.windows-x86_64.alloc|--dump-alloc,--target,windows-x86_64,${CMAKE_CURRENT_SOURCE_DIR}/programs/bitfields.anti"
        "measure.opt|--dump-opt,${DUMP}/measure.anti"
        "measure.linux-arm64.alloc|--dump-alloc,--target,linux-arm64,${DUMP}/measure.anti")
    string(REPLACE "|" ";" parts "${case}")
    list(GET parts 0 expected)
    list(GET parts 1 arguments)
    add_test(NAME listing_${expected}
        COMMAND "${CMAKE_COMMAND}"
            "-DANTIC=$<TARGET_FILE:antic>"
            "-DCOMMAND=${arguments}"
            "-DEXPECTED=${DUMP}/${expected}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run_dump.cmake")
endforeach()
# Assembly files of dev mode and of a shared library for C.
foreach(case "scale.dev.macos-arm64.s|--dev,-S,--target,macos-arm64,-I,${MODULES},MODULE_SCALE"
        "scale.dev.windows-x86_64.s|--dev,-S,--target,windows-x86_64,-I,${MODULES},MODULE_SCALE"
        "main.dev.linux-x86_64.s|--dev,-S,--target,linux-x86_64,-I,${LIBS},${MODULES}/main.anti"
        "doubling.linux-x86_64.s|--lib,shared,-S,--target,linux-x86_64,-I,${CMAKE_CURRENT_SOURCE_DIR}/clib,CLIB_DOUBLING"
        "doubling.macos-arm64.s|--lib,shared,-S,--target,macos-arm64,-I,${CMAKE_CURRENT_SOURCE_DIR}/clib,CLIB_DOUBLING"
        "doubling.windows-arm64.s|--lib,shared,-S,--target,windows-arm64,-I,${CMAKE_CURRENT_SOURCE_DIR}/clib,CLIB_DOUBLING")
    string(REPLACE "|" ";" parts "${case}")
    list(GET parts 0 expected)
    list(GET parts 1 arguments)
    string(REPLACE "MODULE_SCALE" "${MODULES}/com/example/scale.anti"
           arguments "${arguments}")
    string(REPLACE "CLIB_DOUBLING"
           "${CMAKE_CURRENT_SOURCE_DIR}/clib/com/example/doubling.anti"
           arguments "${arguments}")
    add_test(NAME listing_${expected}
        COMMAND "${CMAKE_COMMAND}"
            "-DANTIC=$<TARGET_FILE:antic>"
            "-DCOMMAND=${arguments},-o,${LISTINGS}/${expected}"
            "-DOUTPUT=${LISTINGS}/${expected}"
            "-DEXPECTED=${DUMP}/${expected}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run_dump.cmake")
endforeach()
set_tests_properties(listing_main.dev.linux-x86_64.s PROPERTIES
    FIXTURES_REQUIRED twice_antl)

# Calls through function pointers in the IR of chapter 20.
add_test(NAME dump_opt_fnptr
    COMMAND "${CMAKE_COMMAND}"
        "-DANTIC=$<TARGET_FILE:antic>"
        "-DCOMMAND=--dump-opt,${CMAKE_CURRENT_SOURCE_DIR}/dump/fnptr.anti"
        "-DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/dump/fnptr.opt"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_dump.cmake")

# String literals as globals in the IR of chapter 19.
add_test(NAME dump_opt_strings
    COMMAND "${CMAKE_COMMAND}"
        "-DANTIC=$<TARGET_FILE:antic>"
        "-DCOMMAND=--dump-opt,${CMAKE_CURRENT_SOURCE_DIR}/dump/strings.anti"
        "-DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/dump/strings.opt"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_dump.cmake")

# Aggregates in the IR of chapter 18.
add_test(NAME dump_opt_structs
    COMMAND "${CMAKE_COMMAND}"
        "-DANTIC=$<TARGET_FILE:antic>"
        "-DCOMMAND=--dump-opt,${CMAKE_CURRENT_SOURCE_DIR}/dump/structs.anti"
        "-DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/dump/structs.opt"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_dump.cmake")

# The parameter extensions of chapter 15 in the IR.
add_test(NAME dump_opt_arm64
    COMMAND "${CMAKE_COMMAND}"
        "-DANTIC=$<TARGET_FILE:antic>"
        "-DCOMMAND=--dump-opt,${CMAKE_CURRENT_SOURCE_DIR}/dump/arm64.anti"
        "-DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/dump/arm64.opt"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_dump.cmake")

# The assembly files of chapters 16, 19 and 20 for each object format.
foreach(case "main,macos-arm64" "main,macos-x86_64" "main,linux-x86_64"
        "main,linux-arm64" "main,windows-x86_64" "main,windows-arm64"
        "letters,macos-arm64" "letters,linux-x86_64"
        "strings,linux-x86_64" "strings,macos-arm64" "strings,windows-x86_64"
        "fnptr,macos-arm64")
    string(REPLACE "," ";" parts "${case}")
    list(GET parts 0 source)
    list(GET parts 1 target)
    if(source STREQUAL "letters")
        set(path "${CMAKE_CURRENT_SOURCE_DIR}/programs/${source}.anti")
    else()
        set(path "${CMAKE_CURRENT_SOURCE_DIR}/dump/${source}.anti")
    endif()
    set(output "${CMAKE_CURRENT_BINARY_DIR}/emit/${source}.${target}.s")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/emit")
    add_test(NAME emit_${source}.${target}
        COMMAND "${CMAKE_COMMAND}"
            "-DANTIC=$<TARGET_FILE:antic>"
            "-DCOMMAND=-S,--target,${target},-o,${output},${path}"
            "-DOUTPUT=${output}"
            "-DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/dump/${source}.${target}.s"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run_dump.cmake")
endforeach()

# Every instruction that antic writes assembles with llvm-mc.
foreach(target_triple "linux-x86_64,x86_64-unknown-linux-gnu"
        "windows-x86_64,x86_64-pc-windows-msvc"
        "macos-x86_64,x86_64-apple-macos" "linux-arm64,aarch64-unknown-linux-gnu"
        "macos-arm64,arm64-apple-macos"
        "windows-arm64,aarch64-pc-windows-msvc")
    string(REPLACE "," ";" parts "${target_triple}")
    list(GET parts 0 target)
    list(GET parts 1 triple)
    foreach(source main loop twice cell spill7 sum args x86 arm64 floatcalls
            floats structs strings fnptr)
        add_test(NAME asm_${source}_${target}
            COMMAND "${CMAKE_COMMAND}"
                "-DANTIC=$<TARGET_FILE:antic>"
                "-DLLVM_MC=${ANTIC_LLVM_MC}"
                "-DTARGET=${target}"
                "-DTRIPLE=${triple}"
                "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/dump/${source}.anti"
                "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/asm"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/run_asm.cmake")
    endforeach()
    # Sizes, C types, unions, bitfields, packed and aligned structs and
    # export, from the program and ABI tests.
    foreach(source programs/sizes programs/c_types programs/bitfields
            abi/abi_structs abi/probe)
        get_filename_component(name "${source}" NAME)
        add_test(NAME asm_${name}_${target}
            COMMAND "${CMAKE_COMMAND}"
                "-DANTIC=$<TARGET_FILE:antic>"
                "-DLLVM_MC=${ANTIC_LLVM_MC}"
                "-DTARGET=${target}"
                "-DTRIPLE=${triple}"
                "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/${source}.anti"
                "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/asm"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/run_asm.cmake")
    endforeach()
    # A shared library for C, with the constructor section of the target.
    add_test(NAME asm_clib_${target}
        COMMAND "${CMAKE_COMMAND}"
            "-DANTIC=$<TARGET_FILE:antic>"
            "-DLLVM_MC=${ANTIC_LLVM_MC}"
            "-DTARGET=${target}"
            "-DTRIPLE=${triple}"
            "-DOPTIONS=--lib|shared|-I|${CMAKE_CURRENT_SOURCE_DIR}/clib"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/clib/com/example/geo.anti"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/asm"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run_asm.cmake")
endforeach()

add_test(NAME dump_select_loop_arm64
    COMMAND "${CMAKE_COMMAND}"
        "-DANTIC=$<TARGET_FILE:antic>"
        "-DCOMMAND=--dump-select,--target,linux-arm64,${CMAKE_CURRENT_SOURCE_DIR}/dump/loop.anti"
        "-DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/dump/loop.arm64.sel"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_dump.cmake")
add_test(NAME dump_select_loop_x86_64
    COMMAND "${CMAKE_COMMAND}"
        "-DANTIC=$<TARGET_FILE:antic>"
        "-DCOMMAND=--dump-select,--target,macos-x86_64,${CMAKE_CURRENT_SOURCE_DIR}/dump/loop.anti"
        "-DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/dump/loop.x86_64.sel"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_dump.cmake")

# The optimizer examples of chapter 10. Each NAME.anti in tests/opt has its
# IR before optimization in NAME.ir and after optimization in NAME.opt.
file(GLOB opt_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/opt/*.anti")
foreach(source ${opt_sources})
    get_filename_component(name "${source}" NAME_WE)
    get_filename_component(dir "${source}" DIRECTORY)
    add_test(NAME ir_${name}
        COMMAND "${CMAKE_COMMAND}"
            "-DANTIC=$<TARGET_FILE:antic>"
            "-DOPTION=--dump-ir"
            "-DSOURCE=${source}"
            "-DEXPECTED=${dir}/${name}.ir"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run_dump.cmake")
    add_test(NAME opt_${name}
        COMMAND "${CMAKE_COMMAND}"
            "-DANTIC=$<TARGET_FILE:antic>"
            "-DOPTION=--dump-opt"
            "-DSOURCE=${source}"
            "-DEXPECTED=${dir}/${name}.opt"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run_dump.cmake")
endforeach()

# Semantic analysis reports every error it finds.
add_test(NAME error_types
    COMMAND antic -S -o "${CMAKE_CURRENT_BINARY_DIR}/errors/types.s"
            "${CMAKE_CURRENT_SOURCE_DIR}/errors/types.anti")
set_tests_properties(error_types PROPERTIES
    PASS_REGULAR_EXPRESSION "^[^\n]*types.anti:3:19: error: expected `bool`, found an integer literal\n[^\n]*types.anti:4:9: error: expected `int`, found `bool`\n$")

# The tests of the chapters that the tree holds. A chapter-N tag holds the
# book up to chapter N, and tests/chapters.txt gives every test its
# chapter. A test of a later chapter is disabled, and a test without a
# chapter stops the configuration.
file(GLOB chapter_pages RELATIVE "${PROJECT_SOURCE_DIR}/writing-a-compiler"
    "${PROJECT_SOURCE_DIR}/writing-a-compiler/[0-9][0-9]-*/index.md")
set(book_last 0)
foreach(page IN LISTS chapter_pages)
    string(SUBSTRING "${page}" 0 2 number)
    math(EXPR number "${number}")
    if(number GREATER book_last)
        set(book_last ${number})
    endif()
endforeach()
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/chapters.txt" chapter_rules
    REGEX "^[0-9a-z]+ ")
get_property(all_tests DIRECTORY PROPERTY TESTS)
foreach(test IN LISTS all_tests)
    set(test_chapter "")
    foreach(rule IN LISTS chapter_rules)
        string(FIND "${rule}" " " space)
        string(SUBSTRING "${rule}" 0 ${space} chapter)
        math(EXPR start "${space} + 1")
        string(SUBSTRING "${rule}" ${start} -1 pattern)
        if(test MATCHES "${pattern}")
            if(chapter STREQUAL "excerpt")
                math(EXPR test_chapter "${CMAKE_MATCH_1}")
            else()
                set(test_chapter ${chapter})
            endif()
            break()
        endif()
    endforeach()
    if(test_chapter STREQUAL "")
        message(FATAL_ERROR "the test ${test} has no chapter in tests/chapters.txt")
    endif()
    set_property(TEST "${test}" APPEND PROPERTY LABELS "chapter-${test_chapter}")
    if(test_chapter GREATER book_last)
        set_tests_properties("${test}" PROPERTIES DISABLED TRUE)
    endif()
endforeach()
