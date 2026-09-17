# The tests of libraries for C. Build the libraries geo and other with
# antic --lib, then compile and run C programs against them. Run with
# cmake -P and these values:
#   ANTIC     the antic executable
#   LLVM_MC   the llvm-mc executable
#   LLVM_AR   the llvm-ar executable
#   RUNTIME   the runtime directory
#   SOURCES   tests/clib
#   DUMP      tests/dump, with the headers that chapter 25 prints
#   WORK      a directory for the output
#   CASE      static, shared, exports, two, loader, header or bundle

set(HOST_SHARED_SUFFIX ".so")
if(APPLE)
    set(HOST_SHARED_SUFFIX ".dylib")
endif()

function(run)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE status
        OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "${ARGN} failed with ${status}\n${out}${err}")
    endif()
    set(run_out "${out}" PARENT_SCOPE)
endfunction()

# Build library name as kind, static or shared, into dir.
function(library name kind dir)
    file(MAKE_DIRECTORY "${dir}")
    if(kind STREQUAL "shared")
        set(file "${dir}/lib${name}${HOST_SHARED_SUFFIX}")
    else()
        set(file "${dir}/lib${name}.a")
    endif()
    run("${ANTIC}" --lib ${kind} ${ARGN} --llvm-mc "${LLVM_MC}"
        --llvm-ar "${LLVM_AR}" --runtime "${RUNTIME}" -I "${SOURCES}"
        -o "${file}" "${SOURCES}/com/example/${name}.anti")
    set(run_out "${run_out}" PARENT_SCOPE)
endfunction()

# Compare the generated header with the one that chapter 25 prints.
function(expect_header header expected)
    file(READ "${header}" got)
    file(READ "${DUMP}/${expected}" wanted)
    if(NOT got STREQUAL wanted)
        message(FATAL_ERROR "${header} differs from ${DUMP}/${expected}\n${got}")
    endif()
endfunction()

function(expect_output program expected)
    run("${program}")
    file(READ "${expected}" wanted)
    if(NOT run_out STREQUAL wanted)
        message(FATAL_ERROR "${program} printed\n${run_out}expected\n${wanted}")
    endif()
endfunction()

set(dir "${WORK}/${CASE}")
file(REMOVE_RECURSE "${dir}")

if(CASE STREQUAL "static")
    library(geo static "${dir}")
    # The printed line links main.c with the archive and the runtime.
    string(STRIP "${run_out}" line)
    if(NOT line MATCHES "^cc main.c ${dir}/libgeo.a ${RUNTIME}/lib/[a-z0-9_-]+/libanti_rt.a")
        message(FATAL_ERROR "unexpected link line: ${line}")
    endif()
    # The archive keeps the copy of the package header in an object.
    run("${LLVM_AR}" t "${dir}/libgeo.a")
    if(NOT run_out MATCHES "geo.package.o")
        message(FATAL_ERROR "the archive holds no package header\n${run_out}")
    endif()
    string(REPLACE "cc main.c " "" inputs "${line}")
    separate_arguments(inputs UNIX_COMMAND "${inputs}")
    run(cc -I "${dir}" "${SOURCES}/roundtrip.c" ${inputs} -o "${dir}/roundtrip")
    expect_output("${dir}/roundtrip" "${SOURCES}/roundtrip.expected")
elseif(CASE STREQUAL "shared")
    library(geo shared "${dir}")
    run(cc -I "${dir}" "${SOURCES}/roundtrip.c"
        "${dir}/libgeo${HOST_SHARED_SUFFIX}" -o "${dir}/roundtrip")
    expect_output("${dir}/roundtrip" "${SOURCES}/roundtrip.expected")
elseif(CASE STREQUAL "exports")
    library(geo shared "${dir}")
    if(APPLE)
        run(nm -gU "${dir}/libgeo.dylib")
    else()
        run(nm -D --defined-only "${dir}/libgeo.so")
    endif()
    string(REGEX MATCHALL "[A-Za-z_][A-Za-z0-9_.]*\n" symbols "${run_out}")
    set(names "")
    foreach(symbol IN LISTS symbols)
        string(STRIP "${symbol}" symbol)
        string(REGEX REPLACE "^_" "" symbol "${symbol}")
        list(APPEND names "${symbol}")
    endforeach()
    list(SORT names)
    set(wanted anti_licenses geo_apply geo_dot geo_half geo_layer geo_ready
        geo_scale)
    if(NOT names STREQUAL wanted)
        message(FATAL_ERROR "exports: ${names}\nexpected: ${wanted}")
    endif()
elseif(CASE STREQUAL "two")
    library(geo shared "${dir}/shared")
    library(other shared "${dir}/shared")
    run(cc -I "${dir}/shared" "${SOURCES}/twolibs.c"
        "${dir}/shared/libgeo${HOST_SHARED_SUFFIX}"
        "${dir}/shared/libother${HOST_SHARED_SUFFIX}" -o "${dir}/two_shared")
    run("${dir}/two_shared")
    if(NOT run_out STREQUAL "10\n")
        message(FATAL_ERROR "two shared libraries printed ${run_out}")
    endif()
    library(geo static "${dir}/static")
    library(other static "${dir}/static")
    # The printed link line names the runtime library of the host.
    string(REGEX MATCH "[^ ]*libanti_rt.a" runtime_library "${run_out}")
    run(cc -I "${dir}/static" "${SOURCES}/twolibs.c" "${dir}/static/libgeo.a"
        "${dir}/static/libother.a" ${runtime_library} -o "${dir}/two_static")
    run("${dir}/two_static")
    if(NOT run_out STREQUAL "10\n")
        message(FATAL_ERROR "two static libraries printed ${run_out}")
    endif()
elseif(CASE STREQUAL "loader")
    library(geo shared "${dir}")
    run(cc "${SOURCES}/loader.c" -o "${dir}/loader")
    run("${dir}/loader" "${dir}/libgeo${HOST_SHARED_SUFFIX}")
    if(NOT run_out STREQUAL "1\n")
        message(FATAL_ERROR "the constructor did not run: ${run_out}")
    endif()
elseif(CASE STREQUAL "header")
    library(geo static "${dir}")
    expect_header("${dir}/geo.h" geo.h)
    library(shapes static "${dir}")
    expect_header("${dir}/shapes.h" shapes.h)
    run(cc -std=c11 -Wall -Werror -I "${dir}" -fsyntax-only
        "${SOURCES}/header.c")
    run(c++ -std=c++17 -Wall -Werror -I "${dir}" -fsyntax-only
        "${SOURCES}/header.cpp")
    run(cc -std=c11 -Wall -Werror -I "${dir}" -fsyntax-only
        "${SOURCES}/shapes.c")
    run(c++ -std=c++17 -Wall -Werror -I "${dir}" -fsyntax-only
        "${SOURCES}/shapes.cpp")
elseif(CASE STREQUAL "bundle")
    library(geo static "${dir}" --bundle-runtime)
    expect_header("${dir}/geo.h" geo.bundle.h)
    string(STRIP "${run_out}" line)
    # The driver puts the system libraries of the target on the line, and
    # a Linux program links pthread and the maths library.
    set(system "")
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
        set(system " -lpthread -lm")
    endif()
    if(NOT line STREQUAL "cc main.c ${dir}/libgeo.a${system}")
        message(FATAL_ERROR "unexpected link line: ${line}")
    endif()
    library(other static "${dir}" --bundle-runtime)
    execute_process(
        COMMAND cc -I "${dir}" "${SOURCES}/twolibs.c" "${dir}/libgeo.a"
                "${dir}/libother.a" -o "${dir}/two_bundled"
        RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(status EQUAL 0 OR NOT "${out}${err}" MATCHES "duplicate symbol|multiple definition")
        message(FATAL_ERROR "two bundled runtimes linked: ${status}\n${out}${err}")
    endif()
    if(NOT "${out}${err}" MATCHES "anti_rt")
        message(FATAL_ERROR "the duplicate does not name the runtime\n${out}${err}")
    endif()
    # A library that reads the notice links against the stub of the bundle
    # and reports an empty notice.
    library(notice static "${dir}/notice" --bundle-runtime)
    run(cc -I "${dir}/notice" "${SOURCES}/notice.c" "${dir}/notice/libnotice.a"
        -o "${dir}/notice/notice")
    run("${dir}/notice/notice")
    if(NOT run_out STREQUAL "0\n")
        message(FATAL_ERROR "the notice length is ${run_out}")
    endif()
else()
    message(FATAL_ERROR "unknown CASE ${CASE}")
endif()
