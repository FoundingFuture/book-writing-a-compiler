# Build the five LLVM tools of a Linux host, linked statically.
#
#   cmake -DSOURCE=<llvm-project> -DDEST=<dir> [-DCROSS=x86_64] \
#         [-DMACOS=x86_64] -P tools/build-llvm.cmake
#
# DESIGN: the Linux release of LLVM links against the ICU of one Ubuntu
# release, so ld.lld does not start on another distribution. A build of
# our own, without ICU, zstd, libxml2 or libedit, needs nothing but the
# kernel. zlib is the one library it keeps, built from the pinned source
# and linked in, because lld reads the compressed debug sections of the
# musl objects of Alpine. tools/pack-llvm.cmake packs what this writes
# into the archive of the host, and refuses a tool that needs a library.
#
# SOURCE is a checkout of llvm-project at the tag of tools/llvm-version.
# DEST is the build directory, and DEST/bin holds the tools afterwards.
# CROSS names the processor of another Linux host. It needs the cross
# compiler of that processor, its static libc.a and libstdc++.a, and a
# native build in NATIVE, whose llvm-tblgen runs on this machine.
#
# MACOS names the processor of a macOS host and builds there instead.
# LLVM publishes no archive for macOS on x86_64, so the development Mac
# builds it with CMAKE_OSX_ARCHITECTURES and Rosetta runs the result.
# macOS links no static libSystem, so that build is dynamic against the
# libraries of the system, which every Mac holds.
cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED SOURCE OR NOT DEFINED DEST)
    message(FATAL_ERROR "usage: cmake -DSOURCE=<llvm-project> -DDEST=<dir> "
                        "[-DCROSS=x86_64] -P tools/build-llvm.cmake")
endif()

set(TOOLS lld llvm-mc llvm-ar llvm-objdump llvm-readobj)

file(READ "${CMAKE_CURRENT_LIST_DIR}/llvm-version" version)
string(STRIP "${version}" version)

# The checkout decides what the tools report as their version, so it is
# read here rather than trusted.
file(STRINGS "${SOURCE}/cmake/Modules/LLVMVersion.cmake" fields
     REGEX "set\\(LLVM_VERSION_(MAJOR|MINOR|PATCH) [0-9]+\\)")
set(found "")
foreach(field IN LISTS fields)
    string(REGEX REPLACE "^.* ([0-9]+)\\)$" "\\1" number "${field}")
    if(NOT found STREQUAL "")
        string(APPEND found ".")
    endif()
    string(APPEND found "${number}")
endforeach()
if(NOT found STREQUAL version)
    message(FATAL_ERROR "${SOURCE} holds LLVM ${found}, and tools/llvm-version "
                        "asks for ${version}. Check out llvmorg-${version}.")
endif()

# Build libz.a from the pinned source with the compiler in cc, and give
# back the directory of zlib.h and the path of the archive.
function(zlib_library root_out library_out)
    set(pin "${CMAKE_CURRENT_LIST_DIR}/zlib-pin")
    file(STRINGS "${pin}" line REGEX "^ZLIB_VERSION=")
    string(REGEX REPLACE "^ZLIB_VERSION=" "" zlib_version "${line}")
    file(STRINGS "${pin}" line REGEX "^url=")
    string(REGEX REPLACE "^url=" "" url "${line}")
    string(REPLACE "@VERSION@" "${zlib_version}" url "${url}")
    file(STRINGS "${pin}" line REGEX "^digest=")
    string(REGEX REPLACE "^digest=" "" digest "${line}")

    set(work "${DEST}/zlib")
    set(root "${work}/zlib-${zlib_version}")
    set(archive "${work}/zlib-${zlib_version}.tar.gz")
    file(MAKE_DIRECTORY "${work}")
    if(NOT EXISTS "${archive}")
        file(DOWNLOAD "${url}" "${archive}" STATUS status)
        list(GET status 0 code)
        list(GET status 1 text)
        if(NOT code EQUAL 0)
            file(REMOVE "${archive}")
            message(FATAL_ERROR "${url}: ${text}")
        endif()
    endif()
    file(SHA256 "${archive}" actual)
    if(NOT actual STREQUAL digest)
        message(FATAL_ERROR "${archive}: SHA-256 ${actual}, expected ${digest}")
    endif()
    if(NOT EXISTS "${root}/zlib.h")
        file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${work}")
    endif()

    # A static zlib needs no configure step. The source ships zconf.h,
    # and these files are the whole of the library. HAVE_UNISTD_H is what
    # the configure of zlib defines on a POSIX system, and the gz
    # functions need it for lseek.
    set(sources adler32 compress crc32 deflate gzclose gzlib gzread gzwrite
                infback inffast inflate inftrees trees uncompr zutil)
    set(objects "")
    foreach(source IN LISTS sources)
        set(object "${root}/${source}.o")
        execute_process(COMMAND ${cc} -O2 -fPIC -DHAVE_UNISTD_H=1 -c
                                "${root}/${source}.c" -o "${object}"
                        RESULT_VARIABLE compiled)
        if(NOT compiled EQUAL 0)
            message(FATAL_ERROR "the compiler failed on ${source}.c of zlib")
        endif()
        list(APPEND objects "${object}")
    endforeach()
    set(library "${root}/libz.a")
    file(REMOVE "${library}")
    execute_process(COMMAND ${ar} rcs "${library}" ${objects}
                    RESULT_VARIABLE archived)
    if(NOT archived EQUAL 0)
        message(FATAL_ERROR "${ar} failed to write ${library}")
    endif()
    set(${root_out} "${root}" PARENT_SCOPE)
    set(${library_out} "${library}" PARENT_SCOPE)
endfunction()

# The compilers of the host, or of the other processor with CROSS.
set(cc "cc")
set(cxx "c++")
set(ar "ar")
if(DEFINED CROSS)
    set(cc "${CROSS}-linux-gnu-gcc")
    set(cxx "${CROSS}-linux-gnu-g++")
    set(ar "${CROSS}-linux-gnu-ar")
endif()
if(DEFINED MACOS)
    set(cc "clang -arch ${MACOS}")
    separate_arguments(cc UNIX_COMMAND "${cc}")
endif()

# DESIGN: lld reads the debug sections of the musl objects that the Linux
# sysroot holds, and the x86_64 objects of Alpine carry SHF_COMPRESSED.
# Without zlib it stops with an error on every one of them, so zlib is
# built from the pinned source and linked in. The source of our own also
# keeps the build off whatever zlib the machine happens to hold.
zlib_library(zlib_root zlib_archive)

# DESIGN: every other option that decides what the binary needs at
# runtime stands here. An option that is on pulls in a shared library of
# the machine that builds, which is the failure this build exists to
# avoid.
set(options
    -DCMAKE_BUILD_TYPE=Release
    -DLLVM_ENABLE_PROJECTS=lld
    "-DLLVM_TARGETS_TO_BUILD=X86\;AArch64"
    -DLLVM_ENABLE_ZLIB=FORCE_ON
    "-DZLIB_INCLUDE_DIR=${zlib_root}"
    "-DZLIB_LIBRARY=${zlib_archive}"
    -DLLVM_ENABLE_ZSTD=OFF
    -DLLVM_ENABLE_LIBXML2=OFF
    -DLLVM_ENABLE_LIBEDIT=OFF
    -DLLVM_ENABLE_ICU=OFF
    -DLLVM_ENABLE_ASSERTIONS=OFF
    -DLLVM_INCLUDE_TESTS=OFF
    -DLLVM_INCLUDE_BENCHMARKS=OFF)
# -static links the C and C++ libraries into the tool, and -s leaves the
# symbol table out. The five tools weigh 34 MB packed instead of 37 MB.
if(DEFINED MACOS)
    # macOS has no static libSystem, so the tools link against the
    # libraries of the system. -Wl,-S leaves the debug symbols out, as
    # -s does on the other hosts.
    list(APPEND options "-DCMAKE_OSX_ARCHITECTURES=${MACOS}"
                        "-DCMAKE_EXE_LINKER_FLAGS=-Wl,-S")
else()
    list(APPEND options "-DCMAKE_EXE_LINKER_FLAGS=-static -s")
endif()

if(DEFINED CROSS)
    if(NOT DEFINED NATIVE)
        message(FATAL_ERROR "a cross build needs -DNATIVE=<native build>, "
                            "whose llvm-tblgen runs on this machine")
    endif()
    if(NOT EXISTS "${NATIVE}/bin/llvm-tblgen")
        message(FATAL_ERROR "${NATIVE}/bin/llvm-tblgen does not exist")
    endif()
    list(APPEND options
         -DCMAKE_SYSTEM_NAME=Linux
         "-DCMAKE_SYSTEM_PROCESSOR=${CROSS}"
         "-DCMAKE_C_COMPILER=${CROSS}-linux-gnu-gcc"
         "-DCMAKE_CXX_COMPILER=${CROSS}-linux-gnu-g++"
         "-DLLVM_HOST_TRIPLE=${CROSS}-unknown-linux-gnu"
         "-DLLVM_DEFAULT_TARGET_TRIPLE=${CROSS}-unknown-linux-gnu"
         "-DLLVM_NATIVE_TOOL_DIR=${NATIVE}/bin"
         "-DLLVM_TABLEGEN=${NATIVE}/bin/llvm-tblgen")
endif()

find_program(NINJA ninja)
if(NINJA STREQUAL "NINJA-NOTFOUND")
    message(FATAL_ERROR "ninja is not on the PATH, and the build needs it")
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" -G Ninja -S "${SOURCE}/llvm"
                        -B "${DEST}" ${options}
                RESULT_VARIABLE configured)
if(NOT configured EQUAL 0)
    message(FATAL_ERROR "cmake failed to configure ${DEST}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${DEST}" --target ${TOOLS}
                RESULT_VARIABLE built)
if(NOT built EQUAL 0)
    message(FATAL_ERROR "the build of ${DEST} failed")
endif()

foreach(tool IN LISTS TOOLS)
    if(NOT EXISTS "${DEST}/bin/${tool}")
        message(FATAL_ERROR "${DEST}/bin/${tool} was not built")
    endif()
endforeach()
message(STATUS "${DEST}/bin holds the tools of LLVM ${version}")
