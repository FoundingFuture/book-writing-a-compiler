---
title: "Setup and the first executable"
description: "The tools per platform, the repository layout and CMake build, and a direct code path in antic that turns a one-statement Anti program into an executable."
summary: "What to install on Linux, macOS and Windows. The repository layout and the CMake build. llvm-mc, lld, llvm-ar and the platform linker. A program that returns an exit code, compiled by a direct code path for the host. The toolchain runs before the pipeline exists."
date: 2026-09-13T22:32:00+02:00
lastmod: 2026-09-15T03:50:34+02:00
draft: false
weight: 30
tags: [compilers, assembly]
keywords: [cmake build, llvm-mc, xcode command line tools, visual studio build tools, movz and movk, posix_spawn, program tests, exit code]
---

## Previously

[Chapter 2, The Anti language]({{% relref "/programming/writing-a-compiler/02-the-anti-language" %}}), specifies the core language. It defines the types, with the C types for bindings, and the literals, expressions, statements and functions of the language. Structs, unions and bitfields follow, together with packed and aligned structs. The chapter then specifies modules with export, doc comments and the memory model. It also lists the reserved and contextual words and the platform facts the language relies on.

## Tools per platform

Seven tools build antic and turn its output into executables and libraries.

| Tool | Purpose | Version |
|---|---|---|
| CMake | Configures and drives the build of antic | 3.20 or newer |
| C11 compiler | Compiles antic and the runtime library | clang, gcc or MSVC |
| Platform linker | Links the first executable of this chapter | `ld` on macOS and Linux, `link.exe` on Windows |
| llvm-mc | Assembles the assembly text that antic writes | LLVM 23.1.1 |
| lld | Links executables and libraries for every target from chapter 16 on | LLVM 23.1.1 |
| llvm-ar | Writes archives of object files, the static libraries for C that antic produces | LLVM 23.1.1 |
| llvm-objdump | Shows the format and architecture of an object file or executable | LLVM 23.1.1 |
| llvm-readobj | Decodes the Windows unwind data of an object file for the tests of chapter 16 | LLVM 23.1.1 |

The development machine of the book is an Apple Silicon Mac with macOS 26.6.2. It runs Apple clang 21.0.0, CMake 4.4.3 and the linker `ld-1267`. Every command and output in this chapter comes from that machine.

### macOS

The Command Line Tools for Xcode contain clang, `ld` and the macOS SDK[^1]. The SDK holds the headers and libraries that a program for macOS builds against. The command below installs them.

```sh
xcode-select --install
```

CMake comes from the CMake download page, where the latest release is 4.4.3[^2].

### Linux

On Debian and Ubuntu one command installs the tools. The package `build-essential` depends on `gcc`, `g++`, `make`, `libc6-dev` and `dpkg-dev`[^3]. The `gcc` package depends on `binutils`, which holds the GNU linker `ld`[^4]. The noble release of Ubuntu ships CMake 3.28.3[^5].

```sh
sudo apt install build-essential cmake curl xz-utils
```

### Windows

The Build Tools for Visual Studio hold the MSVC compiler and `link.exe`. The workload named Desktop development with C++ installs the tools for C++ development[^6]. The same workload installs the C++ CMake tools for Windows[^7]. A Native Tools Command Prompt sets the environment for the command-line build tools[^6].

### LLVM tools

The assembler llvm-mc, the linker lld, the archiver llvm-ar, llvm-objdump and llvm-readobj come from one release of LLVM. The file `tools/llvm-version` holds its version, `23.1.1`, and every script and job of the repository reads the version from there. The linker lld has one program for three object formats. The name `ld.lld` selects ELF, `ld64.lld` selects Mach-O, and `lld-link` selects COFF with the options of `link.exe`. This chapter links its first executable with the platform linker, which every host already has, and chapter 16 moves antic to lld.

The release archives of LLVM 23.1.1 on GitHub hold all five tools. The table lists the archive for each host that LLVM publishes, with the SHA-256 digest from the release page.

| Host | Archive | SHA-256 |
|---|---|---|
| macOS on ARM64 | `LLVM-23.1.1-macOS-ARM64.tar.xz` | `64220f1c99132ef7e580447781b84f96fbba6862a43a8f6522b423052cd67502` |
| Linux on x86_64 | `LLVM-23.1.1-Linux-X64.tar.xz` | `832aeb58d105de1cabc7b982dd2c65de0610f7377df48ae8fc2dd8e97420a15c` |
| Linux on ARM64 | `LLVM-23.1.1-Linux-ARM64.tar.xz` | `3fbaaa6a1f147557a4095b9911f8dc2d4c745a11863982f76ee20e248c190a80` |
| Windows on x86_64 | `clang+llvm-23.1.1-x86_64-pc-windows-msvc.tar.xz` | `c54ac8146b420fe72e11e6fdd56498d6818011ad23267196b6ab37b5ac9264c3` |
| Windows on ARM64 | `clang+llvm-23.1.1-aarch64-pc-windows-msvc.tar.xz` | `c8cd61f6624accf0d0f9f4519ddcc97745c6a865205bb43f364b6aaf9c50a31e` |

The archive for macOS on ARM64 is 1569887696 bytes. Inside it, llvm-mc is a file of 46805472 bytes, lld 146093968 bytes, llvm-ar 46718832 bytes, llvm-objdump 40147408 bytes and llvm-readobj 8029296 bytes. The names `ld.lld`, `ld64.lld` and `lld-link` are symbolic links to `lld`.

The script `tools/get-llvm-mc.sh` downloads the archive for a macOS or Linux host and checks its SHA-256 digest. The environment variable `LLVM_ARCHIVE` names an archive that is already on disk, and the script then skips the download. Its argument is the directory that receives the tools.

```sh
tools/get-llvm-mc.sh build/toolchain
```

The last lines of the script extract the tools into `bin/` of that directory and check their versions with `tools/check-llvm.cmake`.

```sh
top=${asset%.tar.xz}
tar -xJf "$archive" -C "$dest" --strip-components=1 \
    "$top/bin/llvm-mc" "$top/bin/llvm-ar" "$top/bin/llvm-objdump" \
    "$top/bin/llvm-readobj" \
    "$top/bin/lld" "$top/bin/ld.lld" "$top/bin/ld64.lld" "$top/bin/lld-link"
cmake -DLLVM_BIN="$dest/bin" -P "$(dirname "$0")/check-llvm.cmake"
```

The check runs `--version` of llvm-mc, llvm-ar, llvm-readobj and the three names of lld and compares the version with `tools/llvm-version`. A different version stops the script with an error that names the tool. For the tools in `build/toolchain/bin` on the development Mac it prints these lines.

```text
-- llvm-mc 23.1.1
-- llvm-ar 23.1.1
-- llvm-readobj 23.1.1
-- ld.lld 23.1.1
-- ld64.lld 23.1.1
-- lld-link 23.1.1
```

The release has no archive for macOS on x86_64 and no script download for Windows. On an Intel Mac, Homebrew installs the tools with `brew install llvm@23 lld@23`. The formula names carry the major version, and the plain formula `llvm` follows whatever release Homebrew has at the time. On Windows, `choco install llvm --version 23.1.1` installs them. Both ways end with the same check, which refuses any version other than the pin.

The assembler llvm-mc runs for every program from this chapter on. A static library is an archive of object files that the linker reads. The archiver llvm-ar writes the static libraries for C of `antic --lib static`, which chapter 25, Libraries for C, describes. The CMake build copies the checked tools into `bin/` of the runtime archive, so the archive carries the pinned release.

## Repository layout

| Path | Contents |
|---|---|
| `src/` | The compiler |
| `rt/` | The runtime library anti_rt, starting with `rt/start.c`, under 0BSD |
| `std/` | The standard library, in Anti, under 0BSD |
| `libs/` | The CMake build of the third-party libraries for each target |
| `tests/unit/` | Unit tests of the compiler's C code |
| `tests/programs/` | Anti programs with the expected result of each |
| `tests/errors/` | Anti programs that must fail to compile |
| `tools/` | Helper scripts, such as `get-llvm-mc.sh` |
| `writing-a-compiler/` | This book, one directory per chapter |
| `docs/` | The design decisions and the table of contents |

The repository is under the MIT licence. The directories `rt/` and `std/` each hold a `LICENSE` file with the BSD Zero Clause License, 0BSD, as [rt/LICENSE in the book repository](https://github.com/FoundingFuture/book-writing-a-compiler/blob/main/rt/LICENSE) shows. Its one clause grants permission to use, copy, modify and distribute the software for any purpose. The clause sets no condition, so a program that the linker combines with runtime code owes no attribution for that code.

## CMake build

Three commands configure, build and test antic. `ANTIC_LLVM_MC` tells the program tests where llvm-mc is.

```sh
cmake -S . -B build -DANTIC_LLVM_MC="$PWD/build/toolchain/bin/llvm-mc"
cmake --build build
ctest --test-dir build
```

The build compiles every C file with `-Wall -Wextra -Wpedantic -Werror`, and with `/W4 /WX` under MSVC. A warning therefore stops the build. The build has four targets.

- `antic_core`, a static library with the compiler's code
- `antic`, the compiler executable, which is `src/main.c` linked with `antic_core`
- `antic_unit_tests`, the unit tests linked with the same library
- `anti_rt`, the runtime library for the host, `libanti_rt.a`, or `anti_rt.lib` under MSVC

The runtime library lands in the layout of the runtime archive, `runtime/lib/<target>/` inside the build directory. The build computes the target name from the host system and processor.

```cmake
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
```

```cmake
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
```

The runtime library has six source files. This chapter writes `rt/start.c`. Chapter 19 adds `rt/utf.c`, the text conversions for the arguments and the environment, and chapter 25, Libraries for C, adds `rt/init.c`. Chapter 26, The standard library, adds `rt/io.c`, `rt/text.c` and `rt/license.c`, the C functions behind the modules `anti.io`, `anti.text` and `anti.license`. The property `C_VISIBILITY_PRESET hidden` gives every runtime symbol hidden visibility. A shared library is a library that a process loads at run time, and the process sees the symbols that the library exports. A hidden symbol links like any other symbol but stays out of those exports. Two shared libraries built from Anti code therefore load into one process without a conflict. A sanitizer configuration checks antic and not the runtime, because an Anti program links the runtime without a sanitizer runtime. The runtime therefore compiles with `-fno-sanitize=all`. The command `configure_file` copies `rt/LICENSE` to `anti_rt.txt` in `runtime/licenses/`, the directory that holds the licence of each component of the runtime archive.

## llvm-mc and the linker by hand

Before antic runs the tools, the steps can be run by hand. The assembly text below is the output of antic for `tests/programs/return42.anti`, whose only statement is `return 42;`.

```asm
    .build_version macos, 11, 0
    .text
    .globl  _anti.rt.main
    .set    _anti.rt.main, _return42.main
    .p2align 2
_return42.main:
    movz    x0, #42
    ret
```

llvm-mc turns the text into an object file. `-triple` names the target, and `-filetype=obj` asks for an object file.

```sh
llvm-mc -triple=arm64-apple-macos -filetype=obj -o return42.o return42.s
```

The linker combines the object file with `libanti_rt.a` and the C library into an executable. The tool `xcrun` reports the path and the version of the macOS SDK, which on the development Mac is 26.5. The option `-platform_version` gives the minimum macOS version, 11.0, and the SDK version. The option `-lSystem` links the C library.

```sh
ld -arch arm64 -platform_version macos 11.0 "$(xcrun --sdk macosx --show-sdk-version)" \
   -syslibroot "$(xcrun --sdk macosx --show-sdk-path)" \
   -o return42 return42.o build/runtime/lib/macos-arm64/libanti_rt.a -lSystem
./return42; echo $?
```

The last command prints `42`, the exit code of the program.

## Direct code path

The pipeline of chapter 1 does not exist yet. The compiler reaches an executable anyway, through a short route from the source text to assembly text that skips every stage. This route is the direct code path. It accepts exactly one program shape and emits assembly for macOS on ARM64. Chapter 4 starts to replace it with the lexer, and chapter 16 adds the five other targets. The finished compiler no longer holds the direct path, so its files live in `direct-path/` of this chapter's folder and build as the program `direct_path_03`.

```anti
fn main() -> int
{
    return 42;
}
```

### Recognising the program

The function `direct_parse` scans the source characters and checks them against the program shape. It skips spaces, tabs and line breaks between the parts, and it reads an optional `-` and decimal digits as the returned value. A value outside the range of `int` is a compile error, as chapter 2 specifies for literals. The file `src/direct.c` holds the helpers `expect` and `integer`.

```c
bool direct_parse(const char *source, size_t length, int64_t *value,
                  struct diagnostic *diag)
{
    struct scan s = {source, length, 0, 1, 1};

    if (!expect(&s, "fn", true, diag) || !expect(&s, "main", true, diag) ||
        !expect(&s, "(", false, diag) || !expect(&s, ")", false, diag) ||
        !expect(&s, "->", false, diag) || !expect(&s, "int", true, diag) ||
        !expect(&s, "{", false, diag) || !expect(&s, "return", true, diag) ||
        !integer(&s, value, diag) || !expect(&s, ";", false, diag) ||
        !expect(&s, "}", false, diag)) {
        return false;
    }
    skip_space(&s);
    if (s.pos != s.length) {
        return fail(&s, diag, "expected the end of the file");
    }
    return true;
}
```

A mismatch stops the scan with a diagnostic, the position and a message. The driver prints it in the form `file:line:column: error: message`. The file `tests/errors/missing_semicolon.anti` ends its `return` line without `;`.

```text
missing_semicolon.anti:4:1: error: expected `;`
```

### Loading the value

An immediate operand is a constant written inside an instruction. An ARM64 instruction holds an immediate operand of at most 16 bits. For a file `t.s` with the line `movz x0, #65536`, llvm-mc 23.1.1 reports an error.

```text
t.s:2:14: error: immediate must be an integer in range [0, 65535].
```

A 64-bit value therefore enters `x0` in up to four pieces of 16 bits. The instruction `movz` stores one piece, shifted left by 0, 16, 32 or 48 bits, and sets every other bit to zero. Each following `movk` stores another piece and keeps the bits already in the register.

```c
/* Load a 64-bit value into x0. An ARM64 instruction holds at most 16 bits
   of an immediate, so the value goes in as up to four 16-bit pieces. The
   first movz sets one piece and clears the rest. Each movk sets one more. */
static void load_x0(struct text *out, int64_t value)
{
    uint64_t bits = (uint64_t)value;
    bool first = true;
    int shift;

    if (bits == 0) {
        text_append(out, "    movz    x0, #0\n");
        return;
    }
    for (shift = 0; shift < 64; shift += 16) {
        uint64_t piece = (bits >> shift) & 0xffff;
        if (piece == 0) {
            continue;
        }
        text_appendf(out, "    %s    x0, #%u", first ? "movz" : "movk",
                     (unsigned)piece);
        if (shift != 0) {
            text_appendf(out, ", lsl #%d", shift);
        }
        text_append(out, "\n");
        first = false;
    }
}
```

The unit tests fix the output for several values. The value -1 has all 64 bits set and needs all four pieces.

```asm
    movz    x0, #65535
    movk    x0, #65535, lsl #16
    movk    x0, #65535, lsl #32
    movk    x0, #65535, lsl #48
```

### Symbols and the entry

The symbol of `main` follows the mangling of chapter 2, `module.name`, with the leading `_` of Mach-O. The file `return42.anti` is the module `return42`, so its `main` is `_return42.main`. Windows uses a different form, which chapter 9 specifies.

```c
/* DESIGN: module.name on ELF and Mach-O, as docs/decisions.md settles.
   Mach-O prefixes every C-level symbol with '_'. The COFF form for
   Windows is specified in chapter 9. */
bool mangle(struct text *out, enum target t, const char *module,
            const char *name)
{
    switch (t) {
    case TARGET_MACOS_X86_64:
    case TARGET_MACOS_ARM64:
        text_appendf(out, "_%s.%s", module, name);
        return true;
    case TARGET_LINUX_X86_64:
    case TARGET_LINUX_ARM64:
        text_appendf(out, "%s.%s", module, name);
        return true;
    case TARGET_WINDOWS_X86_64:
    case TARGET_WINDOWS_ARM64:
    case TARGET_COUNT:
        break;
    }
    return false;
}
```

The runtime library cannot know the module name of a program. antic therefore defines a second name for the program's `main`, `anti.rt.main`, with the directive `.set`, and `.globl` exports it. The name has the form of a function `main` in a module `anti.rt`. That module belongs to the runtime, and antic refuses to compile a program module of that name. The C file `rt/start.c` defines the C function `main` and calls `anti.rt.main`. A symbol with a dot is not a C identifier, so the declaration names the symbol with an assembler label.

```c
/* The C entry point of every Anti program. It calls the program's main
   through anti.rt.main, the runtime entry symbol that antic defines, and
   returns the result as the process exit code. */
#include <stdint.h>

/* A symbol with a dot is not a C identifier, so the declaration names it
   with an assembler label. Mach-O adds '_' to C symbols, ELF does not. */
#if defined(__APPLE__)
#define ANTI_ENTRY_SYMBOL "_anti.rt.main"
#else
#define ANTI_ENTRY_SYMBOL "anti.rt.main"
#endif

extern int64_t anti_main(void) __asm__(ANTI_ENTRY_SYMBOL);

int main(void)
{
    return (int)anti_main();
}
```

This version of `rt/start.c` calls a `main` without parameters and initialises nothing before the call. The conversion of the arguments and the environment arrives with strings and slices in chapter 19. The initialisation of the runtime arrives with libraries for C in chapter 25. The file converts the result to `int`, and macOS keeps its low 8 bits as the exit code. The program test `return_pieces.anti` returns 305419896, which is `0x12345678` and exits with 120, the value `0x78`.

## Driver

The driver is the part of antic that reads the command line and runs each step. For a source file `return42.anti` it runs five steps.

1. Read the source file and derive the module name `return42` from the file name.
2. Recognise the program with `direct_parse`.
3. Write the assembly text to `return42.s`.
4. Run llvm-mc to write `return42.o`.
5. Run `xcrun` twice for the SDK values, then `ld` to write the executable `return42`.

The assembly and object files stay beside the executable, where a reader can open them. The option `-S` stops after the assembly text, and `-o` names the output. The option `--runtime` names the directory that holds `lib/<target>/libanti_rt.a`. The option `--llvm-mc` names llvm-mc when it is not on the path.

```sh
build/antic --runtime build/runtime tests/programs/return42.anti
```

### Running the tools

antic starts llvm-mc, `xcrun` and `ld` with `posix_spawnp`. When the file name contains no slash, `posix_spawnp` searches the directories in the environment variable `PATH`[^8]. The function waits for the tool and returns its exit status. A tool that fails stops antic. Windows starts processes with `CreateProcess`, which chapter 16 adds.

```c
/* DESIGN: Windows starts processes with CreateProcess, which chapter 16
   adds together with the Windows targets. */
int process_run(const char *const argv[])
{
    fprintf(stderr, "antic: cannot run %s: not implemented on Windows yet\n",
            argv[0]);
    return -1;
}
```

The link step builds the same command line as the manual run above.

```c
static bool link_macos_arm64(const char *object, const char *executable,
                             const char *runtime)
{
    struct text sdk_path = {0};
    struct text sdk_version = {0};
    struct text min_version = {0};
    struct text library = {0};
    bool ok = false;

    if (xcrun("--show-sdk-path", &sdk_path) &&
        xcrun("--show-sdk-version", &sdk_version)) {
        const char *argv[] = {
            "ld", "-arch", "arm64", "-platform_version", "macos",
            NULL, NULL, "-syslibroot", NULL, "-o", executable, object,
            NULL, "-lSystem", NULL};

        text_appendf(&min_version, "%d.%d", MACOS_MIN_MAJOR, MACOS_MIN_MINOR);
        text_appendf(&library, "%s/%s/%s/%s", runtime, RUNTIME_LIB_DIR,
                     target_name(TARGET_MACOS_ARM64), RUNTIME_LIBRARY);
        argv[5] = text_cstr(&min_version);
        argv[6] = text_cstr(&sdk_version);
        argv[8] = text_cstr(&sdk_path);
        argv[12] = text_cstr(&library);
        ok = process_run(argv) == 0;
        if (!ok) {
            fprintf(stderr, "antic: the linker failed\n");
        }
    }
    text_free(&sdk_path);
    text_free(&sdk_version);
    text_free(&min_version);
    text_free(&library);
    return ok;
}
```

## Tests

`ctest` runs three kinds of test.

- The unit test executable checks `direct_parse`, the target names, the mangling, the assembly of the direct path and the text buffer. A failed check prints its file and line.
- A program test compiles a file from `tests/programs` with antic, runs the executable and compares the result with a file beside the source.
- A diagnostic test compiles a file from `tests/errors` and passes when antic prints the expected message.

The expected file of a program test starts with the line `exit N`. The bytes after that line are the expected standard output. `return42.expected` holds one line, `exit 42`. The runner `tests/run_program.cmake` also fails a test when antic prints anything, so a warning from llvm-mc or the linker fails it.

The target name has two definitions, one in `CMakeLists.txt` and one in `src/target.c`. The test `antic_host_target` compares the output of `antic --print-host-target` with the name from CMake. The build of this chapter defines eight tests.

| Test | Checks |
|---|---|
| `unit` | The C code of antic, through the unit test executable |
| `runtime_licenses` | That `runtime/licenses/anti_rt.txt` starts with `BSD Zero Clause License` |
| `antic_version` | The version line of `antic --version` |
| `antic_host_target` | The host target name of CMake against the name of `src/target.c` |
| `program_return42` | The exit code 42 of `return42.anti` |
| `program_return_pieces` | The exit code 120 of `return_pieces.anti` |
| `error_missing_semicolon` | The message for `missing_semicolon.anti` |
| `error_missing_runtime` | The message for a link without `--runtime` |

The direct path has four tests of its own in the finished repository. The test `direct_path_03_assembly` compares the assembly of `return42.anti` with `direct-path/return42.s`. The tests `direct_path_03_return42` and `direct_path_03_return_pieces` link and run both programs with the `start.c` of the folder. The test `direct_path_03_missing_semicolon` checks the message.

On the development Mac all eight tests pass.

## Next

[Chapter 4, The lexer]({{% relref "/programming/writing-a-compiler/04-lexer" %}}), reads UTF-8 source into tokens by hand. It covers keywords, identifiers, numbers with separators and character literals. Comments do not nest, and the four doc comment markers each have a line form and a block form. The chapter also records the positions for error messages. It reads ordinary, raw and byte string literals with hash delimiters.

## References

[^1]: Apple, *Installing the command-line tools*, https://developer.apple.com/documentation/xcode/installing-the-command-line-tools

[^2]: Kitware, *Download CMake*, https://cmake.org/download/

[^3]: Ubuntu, *Package build-essential in noble*, https://packages.ubuntu.com/noble/build-essential

[^4]: Ubuntu, *Package binutils in noble*, "GNU assembler, linker and binary utilities", https://packages.ubuntu.com/noble/binutils

[^5]: Ubuntu, *Package cmake in noble*, version 3.28.3, https://packages.ubuntu.com/noble/cmake

[^6]: Microsoft, *Use the Microsoft C++ toolset from the command line*, https://learn.microsoft.com/en-us/cpp/build/building-on-the-command-line?view=msvc-170

[^7]: Microsoft, *CMake projects in Visual Studio*, section "Installation", https://learn.microsoft.com/en-us/cpp/build/cmake-projects-in-visual-studio?view=msvc-170

[^8]: The Open Group, *The Open Group Base Specifications Issue 8*, `posix_spawnp`, https://pubs.opengroup.org/onlinepubs/9799919799/functions/posix_spawnp.html
