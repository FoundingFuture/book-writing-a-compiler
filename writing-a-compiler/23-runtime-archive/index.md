---
title: "The runtime archive"
description: "The runtime archive that antic reads: the library of six targets, the sysroots, the licence notice and the directory that holds them."
summary: "The libraries directory and its CMake build for six targets, static linking and the archive layout. The sysroot of each target for lld, and musl beside glibc. `libanti_rt.a` and `anti_rt.lib`, llvm-ar, generated shims, the CA bundle and the `licenses/` directory. The licence obligations of a shipped program and the licence choices for the runtime and the standard library. How the driver picks libraries and system link lines per imported module."
date: 2026-09-13T22:32:00+02:00
lastmod: 2026-09-16T21:05:00+02:00
draft: false
weight: 230
tags: [compilers, programming-languages]
keywords: [runtime archive, static library, llvm-ar, sysroot, musl, licence notice, cross compilation, anti_rt]
---

## Previously

[Chapter 22, Threads]({{% relref "/programming/writing-a-compiler/22-threads" %}}), adds `worker` and `parallel`, the rule that keeps a
worker away
from memory another worker writes, and the pool in `rt/threads.c`. Its programs link
the runtime library of the target they are built for, which is what this chapter
assembles.

## Parts of an install

Chapter 3 installs Anti with one command. That package is this archive with antic beside it, and every part of it earns its place.

| Part | Holds | Why it travels with antic |
|---|---|---|
| `bin/antic` | The compiler | The program the user runs |
| `bin/llvm-mc` | The assembler | antic writes assembly text and stops, so something must encode it |
| `bin/lld` and its three names | The linker | One program links ELF, Mach-O and COFF, under `ld.lld`, `ld64.lld` and `lld-link` |
| `bin/llvm-ar` | The archiver | Static libraries for C, which `antic --lib static` writes |
| `bin/llvm-objdump`, `bin/llvm-readobj` | Readers of object files | The tests of chapters 16 and 21 check what the emitter produced |
| `lib/<target>/` | anti_rt for all six targets | A program of any target links the runtime, whichever host compiled it |
| `std/` | The standard library as `.antl` files | One file serves every target, because the IR holds no sizes |
| `sysroot/linux-x86_64`, `sysroot/linux-arm64` | musl and the compiler-rt builtins | Ours to pass on, so a Linux program links with nothing else installed |
| `licenses/` | One file per component | The obligations that travel with a shipped program |

antic reads that directory without being told. Without `--runtime` it takes the directory above its own executable, so `~/.anti/bin/antic` finds `~/.anti/lib` beside it. It looks for llvm-mc, llvm-ar and the lld programs in `bin/` of that archive before the search path. The tools that compile a program are then the pinned ones rather than whatever the machine carries.

The two sysroots that cannot travel are the subject of the next section.

## Sysroots

The directory `sysroot/<target>/` of the runtime archive holds what lld links a program against on that target. The script `tools/get-sysroot.cmake` installs each one, and the CMake build copies them into the runtime archive with the licence of each component in `licenses/`.

| Target | Contents | Source | Licence |
|---|---|---|---|
| linux-x86_64, linux-arm64 | musl 1.2.6 and `libclang_rt.builtins.a` | Alpine Linux 3.24 packages `musl-dev` and `compiler-rt` | MIT, Apache 2.0 with LLVM Exceptions |
| macos-x86_64, macos-arm64 | `.tbd` stubs of libSystem | Command Line Tools for Xcode | Xcode and Apple SDKs Agreement |
| windows-x86_64, windows-arm64 | Microsoft C runtime 14.44.17.14 and Windows SDK 10.0.26100 | xwin 0.10.0 | Microsoft licence terms |

The file `tools/sysroot-pins` holds the versions and the SHA-256 digests of the downloaded files. For a Windows sysroot xwin writes a tree of about 5600 files, and the pin is the SHA-256 digest of the sorted digests of those files. The script checks it after each download. The build also compiles the runtime library for every other target that has a sysroot, with clang and the headers of that sysroot.

## Mixing musl with glibc

A Linux program that antic links carries musl inside the executable. A static library of Linux, such as a bundled native library, is compiled against the headers of some C library. A library that a distribution builds against glibc with `_FORTIFY_SOURCE` calls checked functions such as `__printf_chk`. The `libc.a` of musl 1.2.6 defines `printf` and no function whose name ends in `_chk`. The link then stops with an undefined symbol. The runtime archive therefore compiles every static library it bundles for Linux against the musl headers of the sysroot. It takes no prebuilt archive from a glibc system.

## Runtime library per target

A program for linux-arm64 links `lib/linux-arm64/libanti_rt.a`, whatever machine
compiled it. The build therefore compiles the runtime seven times: once for the host
through the ordinary CMake target, and once for every target that has a sysroot.

```cmake
    foreach(source init io license start text threads utf)
        set(object "${work}/${source}.o")
        add_custom_command(OUTPUT "${object}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${work}"
            COMMAND "${CMAKE_C_COMPILER}" --target=${triple} -std=c11 -O2
                -Wall -Wextra -Wpedantic -Werror -fvisibility=hidden
                "-ffile-prefix-map=${CMAKE_SOURCE_DIR}=." ${ARGN}
                -c "${PROJECT_SOURCE_DIR}/rt/${source}.c" -o "${object}"
            DEPENDS "${PROJECT_SOURCE_DIR}/rt/${source}.c"
                "${PROJECT_SOURCE_DIR}/rt/rt.h" "${PROJECT_SOURCE_DIR}/rt/std.h"
                "${PROJECT_SOURCE_DIR}/rt/utf.h"
            VERBATIM)
        list(APPEND objects "${object}")
    endforeach()
```

Each compile names the triple of the target and the headers it reads. Linux takes the
headers of musl from the sysroot. The macOS compile takes the SDK of the host through
`-isysroot`, and the Windows one the headers that xwin installed. The flags are those
of the host build, with two of its own. The option `-fvisibility=hidden` keeps the
runtime symbols out of the dynamic table of a shared library. The option
`-fno-sanitize=all` keeps the instrumentation of a sanitizer build of antic out of the
programs a reader compiles.

## One archive with llvm-ar

```cmake
    # The stub stays beside the library, never a member of it.
    add_custom_command(OUTPUT "${library}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${ANTIC_RUNTIME_DIR}/lib/${target}"
        COMMAND "${ANTIC_LLVM_AR}" rcs "${library}" ${objects}
        DEPENDS ${objects}
        VERBATIM)
    add_custom_target(anti_rt_${target} ALL DEPENDS "${library}" "${stub}")
```

`llvm-ar rcs` replaces the members, creates the archive when it is absent and writes
the symbol table[^1]. The archiver is the one of the pinned LLVM release, so the
archive format is the same on every host. The name differs per family: `anti_rt.lib`
for Windows and `libanti_rt.a` elsewhere, which is what each linker looks for.

## The licence notice

The symbol `anti_licenses` carries the notice of a program, between two markers that
antic writes around it. The function that reads it lives in the runtime.

```c
/* The notice that antic links into every executable and shared library. */
extern const char anti_licenses[];

static const char begin[] = "ANTI_LICENSES_BEGIN\n";
static const char end[] = "ANTI_LICENSES_END\n";

struct anti_text anti_rt_license_text(void)
{
    struct anti_text text;
    const char *from = anti_licenses;
    const char *to;

    if (strncmp(from, begin, sizeof begin - 1) == 0) {
        from += sizeof begin - 1;
    }
    to = strstr(from, end);
    text.ptr = (const unsigned char *)from;
    text.len = to != NULL ? (int64_t)(to - from) : (int64_t)strlen(from);
    return text;
}
```

A static library for C is not a program, and it carries no notice of its own to read.
A bundled runtime therefore links a second object instead of this one.

```c
/* The licence text in a static library for C. A bundled runtime carries
   this object instead of the one of rt/license.c, because an archive has
   no anti_licenses notice to read. The notice of such a library comes
   from its package header, which `anti license --from-archive` reads. */
struct anti_text anti_rt_license_text(void)
{
    struct anti_text text;

    text.ptr = NULL;
    text.len = 0;
    return text;
}
```

The stub sits beside the library in `lib/<target>/`, never inside it. A member of the
archive would be pulled in whenever the linker needs `anti_rt_license_text`, and a
program would lose its own notice. The build writes it as
`anti_rt_license_stub.o`, or `.obj` for Windows.

## Licences that travel

The directory `licenses/` of the archive holds one file per component. `anti_rt.txt`
is the 0BSD licence of the runtime. The sysroot step adds the licence of musl, of the
compiler-rt builtins and of the LLVM tools. A program that ships carries the
obligations of what it links.

| Component | Licence | Obligation of a shipped binary |
|---|---|---|
| anti_rt and the standard library | 0BSD | None |
| musl | MIT | Reproduce the copyright notice |
| compiler-rt builtins | Apache 2.0 with LLVM exceptions | The exception removes the notice requirement for a binary[^2] |
| LLVM tools | Apache 2.0 with LLVM exceptions | None. The tools build the program and are not part of it |

The runtime and the standard library are 0BSD, which asks for nothing in a binary[^3].
That choice is what lets a program built with Anti carry a notice about musl alone.

## Libraries of later chapters

A second CMake build stands in `libs/`, for the third-party libraries that the archive
will bundle. It builds one target at a time, named as the directory under `lib/`.

```cmake
# Builds the third-party libraries of the runtime archive as static
# libraries for one target triple. Run once per target, from CI or by hand.
# Libraries are added in the chapters that need them: the thread pool
# runtime, raylib, miniaudio, Mbed TLS, PCRE2.

cmake_minimum_required(VERSION 3.20)

project(antic-runtime LANGUAGES C)

set(ANTIC_RUNTIME_TARGET "" CACHE STRING
    "Target directory name, for example macos-arm64 or windows-x86_64")

if(ANTIC_RUNTIME_TARGET STREQUAL "")
    message(FATAL_ERROR "Set ANTIC_RUNTIME_TARGET, for example -DANTIC_RUNTIME_TARGET=macos-arm64")
endif()

set(ANTIC_RUNTIME_OUTPUT
    "${CMAKE_BINARY_DIR}/runtime/lib/${ANTIC_RUNTIME_TARGET}")
file(MAKE_DIRECTORY "${ANTIC_RUNTIME_OUTPUT}")
```

Each library arrives with the chapter that needs it. raylib, miniaudio, Mbed TLS and
PCRE2 arrive with the standard library. That chapter also adds `lib/cacert.pem` for
the networking module. It adds the generated shims that `anti bind` writes for the
`static inline` functions of a C header. The driver then puts the system libraries of
an imported module on the link line. OpenGL, the Cocoa frameworks and `winmm` belong
to the machine and cannot be bundled.

## Tests

The test `runtime_licenses` checks that the archive holds a licence file for every
component the build copied. The tests `cross_link_<target>` link a program for each of
the six targets against the runtime library of that target. They read the format and
the architecture back with llvm-objdump. The test `no_paths` reads the strings of
`libanti_rt.a` and fails on a path of the machine that built it. The build has zero
warnings, and every `ctest` test passes.

## Next

Chapter 24, The build tool, covers what `anti` does for a project. It takes in the
manifest and the lock file, repositories and resolution, and the module cache. It ends
with dev and release builds and the commands that check, format and document a
package.

## References

[^1]: LLVM Project, *llvm-ar* of release 23.1.1, the operation `r` and the modifiers `c` and `s`, https://github.com/llvm/llvm-project/blob/llvmorg-23.1.1/llvm/docs/CommandGuide/llvm-ar.rst

[^2]: LLVM Project, *LLVM Exceptions to the Apache 2.0 License*, file `LICENSE.TXT` of release 23.1.1, https://github.com/llvm/llvm-project/blob/llvmorg-23.1.1/llvm/LICENSE.TXT

[^3]: Open Source Initiative, *Zero-Clause BSD*, https://opensource.org/license/0bsd
