# Writing a compiler

Source for the book "Writing a compiler" and for antic, the compiler it builds.
The book is published as a chapter series on foundingfuture.com. Each chapter
covers one part of compiler construction and includes the C source that
implements it.

antic compiles Anti, a small statically typed language, to GAS-style assembly
text. The six targets are Linux, macOS and Windows on x86_64 and ARM64. The
compiler owns the lexer, parser, semantic analysis, a target-independent IR,
an optimizer and two instruction-selection back ends. It stops at assembly.
llvm-mc assembles the output and the platform linker links it.

## Layout

| Path | Contents |
|---|---|
| `src/` | The compiler, in C11 |
| `rt/` | The runtime library libantirt, starting with `rt/start.c` |
| `std/` | The standard library, in Anti |
| `libs/` | CMake build of the third-party libraries in the runtime archive |
| `tests/` | Unit tests, test programs with expected outputs, and the runner |
| `tools/` | Helper scripts, such as `get-llvm-mc.sh` |
| `writing-a-compiler/` | The book. One Hugo page bundle per chapter |
| `LICENSES/` | Licence texts of bundled third-party components |

## Build

```sh
tools/get-llvm-mc.sh build/toolchain
cmake -S . -B build -DANTIC_LLVM_MC="$PWD/build/toolchain/bin/llvm-mc"
cmake --build build
ctest --test-dir build
```

The build needs CMake 3.20 or newer and a C11 compiler.

## Tools per platform

Compiling an Anti program needs llvm-mc and the platform linker.

macOS: install the Xcode Command Line Tools.

```sh
xcode-select --install
```

Linux, Debian or Ubuntu: install a C compiler and binutils.

```sh
sudo apt install build-essential cmake
```

Windows: install Visual Studio Build Tools with the workload "Desktop
development with C++". Run antic from a Developer Command Prompt so that
`link.exe` and the SDK libraries are on the path.

llvm-mc ships in the runtime archive for every target. The archive is
described in chapter 23 of the book.

## The book

The chapters are Hugo page bundles under `writing-a-compiler/`. The site
mounts that directory as `content/programming/writing-a-compiler/`. Chapter
order comes from the `weight` field in each chapter's front matter.

## Licence

The source code and the book text are released under the MIT licence. See
`LICENSE`. Bundled third-party components keep their own licences, collected
in `LICENSES/`.
