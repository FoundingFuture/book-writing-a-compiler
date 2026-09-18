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

## Install Anti

One command installs the compiler, the tools it drives and the runtime of every target under `~/.anti`. It writes no other directory and asks for no password.

```sh
curl -fsSL https://anti-lang.com/install.sh | sh
```

```powershell
irm https://anti-lang.com/install.ps1 | iex
```

Each question takes a plain return for yes, and a variable answers it for an unattended run.

| Question | Variable |
|---|---|
| Replace an install that is already there | `ANTI_REPLACE` |
| Add `~/.anti/bin` to the profile or to the PATH of the account | `ANTI_PATH` |
| Fetch the Microsoft libraries that a Windows program links against | `ANTI_MICROSOFT` |

The installer takes the package of the processor it runs on. A machine that emulates the
other processor takes that package with `--arm` or `--intel`, and it lands in
`~/.anti-<cpu>` beside the native one. An Apple Silicon Mac runs the x86_64 compiler
through Rosetta, and Windows on ARM runs the x64 one through its own emulation.

```sh
curl -fsSL https://anti-lang.com/install.sh | sh -s -- --intel
```

```powershell
$env:ANTI_ARCH = "x86_64"; irm https://anti-lang.com/install.ps1 | iex
```

A run through `iex` passes no arguments, so `ANTI_ARCH` carries the choice on Windows.

A program then compiles with no options, because antic reads the runtime beside its own executable.

```sh
antic hello.anti -o hello
```

The package holds antic, the five LLVM tools, the runtime library of all six targets, the standard library and the sysroots of Linux. Two sysroots stay out of it. Apple licenses the macOS SDK for its own hardware, and Microsoft licenses the Windows CRT to each user. The installer therefore takes the macOS stubs from the Command Line Tools of the machine. On Windows it lays the sysroot over the Build Tools of Visual Studio when they are installed. The design page for the runtime archive on anti-lang.com describes each part and where it comes from.

`https://anti-lang.com/uninstall.sh` and `uninstall.ps1` remove the directory and the line that the installer wrote, and nothing else.

The rest of this chapter builds antic from source, which is what the following chapters extend.

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

A release archive of LLVM holds every tool of the project and weighs between 0.9 and 1.6 GB. Anti serves an archive of the five tools instead, one per host, at `https://anti-lang.com/downloads/resources/llvm/23.1.1/`. The file `tools/llvm-pin` names the archive of each host with its SHA-256 digest, and `SHA256SUMS` beside the archives holds the same digests.

| Host | Archive | Bytes |
|---|---|---|
| macOS on ARM64 | `anti-llvm-23.1.1-macos-arm64.tar.xz` | 46217684 |
| macOS on x86_64 | `anti-llvm-23.1.1-macos-x86_64.tar.xz` | 27836804 |
| Linux on x86_64 | `anti-llvm-23.1.1-linux-x86_64.tar.xz` | 37037392 |
| Linux on ARM64 | `anti-llvm-23.1.1-linux-arm64.tar.xz` | 33945352 |
| Windows on x86_64 | `anti-llvm-23.1.1-windows-x86_64.tar.xz` | 32731020 |
| Windows on ARM64 | `anti-llvm-23.1.1-windows-arm64.tar.xz` | 28818868 |

The script `tools/pack-llvm.cmake` builds those archives. It downloads each release archive of LLVM and checks it against the digest in `tools/llvm-upstream`. It then takes the five tools with the LLVM licence and writes the archive and its digest. The licence is Apache 2.0 with LLVM Exceptions, which asks for that copy beside the binaries. On Windows the four names of lld are four copies of one binary, so the archive carries one and the install writes the other names.

Inside the archive for macOS on ARM64, llvm-mc is a file of 46805472 bytes, lld 146093968 bytes, llvm-ar 46718832 bytes, llvm-objdump 40147408 bytes and llvm-readobj 8029296 bytes. The names `ld.lld`, `ld64.lld` and `lld-link` are symbolic links to `lld`.

One command installs them. CMake downloads the archive, checks its digest, unpacks the tools and runs the version check, so the same command serves every host in the file. It needs no shell, no `curl` and no `tar`.

```sh
cmake -DDEST=build/toolchain -P tools/get-llvm.cmake
```

The variable `ARCHIVE` names an archive that is already on disk and skips the download. The digest is checked either way. The archive holds every LLVM tool, so the script unpacks the eight names of the five that antic needs and leaves the rest.

```cmake
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

The release of LLVM carries no archive for macOS on x86_64. That host is built from the pinned source, as the two Linux hosts are. The script `tools/build-llvm.cmake` compiles it on an Apple Silicon Mac with `CMAKE_OSX_ARCHITECTURES=x86_64`. Rosetta runs the result for the check. All six hosts therefore install their tools with the one script and the one pin.

The assembler llvm-mc runs for every program from this chapter on. A static library is an archive of object files that the linker reads. The archiver llvm-ar writes the static libraries for C of `antic --lib static`, which chapter 23, Libraries for C, describes. The CMake build copies the checked tools into `bin/` of the runtime archive, so the archive carries the pinned release.

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
| `tools/` | Helper scripts, such as `get-llvm.cmake` |
| `writing-a-compiler/` | This book, one directory per chapter |
| `docs/` | The design decisions and the table of contents |

The repository is under the MIT licence. The directories `rt/` and `std/` each hold a `LICENSE` file with the BSD Zero Clause License, 0BSD, as [rt/LICENSE in the book repository](https://github.com/FoundingFuture/book-writing-a-compiler/blob/main/rt/LICENSE) shows. Its one clause grants permission to use, copy, modify and distribute the software for any purpose. The clause sets no condition, so a program that the linker combines with runtime code owes no attribution for that code.

## CMake build

Two commands configure and build antic. `ANTIC_LLVM_MC` tells the build where llvm-mc is.

```sh
cmake -S . -B build -DANTIC_LLVM_MC="$PWD/build/toolchain/bin/llvm-mc"
cmake --build build
```

The build compiles every C file with `-Wall -Wextra -Wpedantic -Werror`, and with `/W4 /WX` under MSVC. A warning therefore stops the build.

```text
$ cmake -S . -B build -DANTIC_LLVM_MC="$PWD/build/toolchain/bin/llvm-mc"
-- The C compiler identification is AppleClang 17.0.0.17000013
-- Configuring done (0.4s)
-- Generating done (0.1s)
-- Build files have been written to: /Users/you/anti/build

$ cmake --build build
[ 12%] Building C object CMakeFiles/antic_core.dir/src/lexer.c.o
[ 25%] Building C object CMakeFiles/antic_core.dir/src/parser.c.o
...
[100%] Built target antic
```

The build has four targets.

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
```

The runtime library has six source files. This chapter writes `rt/start.c`. Chapter 19 adds `rt/utf.c`, the text conversions for the arguments and the environment. Chapter 23, Libraries for C, adds `rt/init.c`. The standard library adds `rt/io.c`, `rt/text.c` and `rt/license.c`, the C functions behind the modules `anti.io`, `anti.text` and `anti.license`. The property `C_VISIBILITY_PRESET hidden` gives every runtime symbol hidden visibility. A shared library is a library that a process loads at run time, and the process sees the symbols that the library exports. A hidden symbol links like any other symbol but stays out of those exports. Two shared libraries built from Anti code therefore load into one process without a conflict. A sanitizer configuration checks antic and not the runtime, because an Anti program links the runtime without a sanitizer runtime. The runtime therefore compiles with `-fno-sanitize=all`. The command `configure_file` copies `rt/LICENSE` to `anti_rt.txt` in `runtime/licenses/`, the directory that holds the licence of each component of the runtime archive.

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

This version of `rt/start.c` calls a `main` without parameters and initialises nothing before the call. The conversion of the arguments and the environment arrives with strings and slices in chapter 19. The initialisation of the runtime arrives with libraries for C in chapter 23. The file converts the result to `int`, and macOS keeps its low 8 bits as the exit code. The program test `return_pieces.anti` returns 305419896, which is `0x12345678` and exits with 120, the value `0x78`.

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

antic starts llvm-mc, `xcrun` and `ld` with `posix_spawnp`. When the file name contains no slash, `posix_spawnp` searches the directories in the environment variable `PATH`[^8]. The function waits for the tool and returns its exit status. A tool that fails stops antic. Windows has no `posix_spawnp`, so the same two functions call `CreateProcessW` there.

Windows takes one command line where POSIX takes a list of arguments, and the C runtime of the program parses it back. Each argument is therefore quoted by those rules before the parts are joined.

```c
/* DESIGN: Windows takes one command line where POSIX takes a list, and
   the C runtime of the program parses it back. An argument is quoted by
   those rules: a run of backslashes doubles before a quote, and a quote
   of the argument itself is escaped. */
static void quote(const char *arg, struct text *out)
{
    size_t i;

    if (arg[0] != '\0' && strpbrk(arg, " \t\n\v\"") == NULL) {
        text_append(out, arg);
        return;
    }
    text_append(out, "\"");
    for (i = 0; arg[i] != '\0';) {
        size_t slashes = 0;
        while (arg[i] == '\\') {
            slashes++;
            i++;
        }
        if (arg[i] == '\0' || arg[i] == '"') {
            slashes *= 2;
        }
        while (slashes-- > 0) {
            text_append(out, "\\");
        }
        if (arg[i] == '\0') {
            break;
        }
        if (arg[i] == '"') {
            text_append(out, "\\");
        }
        text_append_bytes(out, arg + i, 1);
        i++;
    }
    text_append(out, "\"");
}
```

The command line is converted to UTF-16, because antic holds its strings as UTF-8 and the wide functions of Windows take UTF-16. Without an application name `CreateProcessW` searches `PATH` and appends `.exe`, as a shell does.

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

The build writes a test runner as well, and `ctest` runs it from the build
directory.

```text
$ ctest --test-dir build
Test project /Users/you/anti/build
        Start   1: unit
  1/798 Test   #1: unit ...................................   Passed   0.86 sec
        Start   2: runtime_licenses
  2/798 Test   #2: runtime_licenses .......................   Passed   0.01 sec
...
100% tests passed out of 798
```

The count grows with the book. A test of this chapter compiles a file from
`tests/programs` with antic, runs the executable and compares the result with a file
beside the source. The expected file starts with the line `exit N`, and the bytes
after it are the expected standard output. The runner also fails a test when antic
prints anything, so a warning from llvm-mc or the linker fails it.

The target name has two definitions, one in `CMakeLists.txt` and one in
`src/target.c`. A test compares the output of `antic --print-host-target` with the
name from CMake, so the two cannot drift apart.

The build has zero warnings, and every `ctest` test passes.

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
