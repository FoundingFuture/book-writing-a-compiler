# Anti libraries for C

Design of `anti build --lib`: static and shared libraries built from Anti code and used
from C, C++ or any language with C bindings. Settled on 2026-09-15 and aligned with
`docs/decisions.md` the same day. `docs/decisions.md` is the authority, and the details
below agree with it and with `docs/tooling.md`, `docs/tooling-addendum.md` and
`docs/distribution.md`.

Contents:

- [Scope](#scope)
- [Export marker](#export-marker)
- [Signature rule](#signature-rule)
- [Header generation](#header-generation)
- [Outputs per target](#outputs-per-target)
- [Runtime](#runtime)
- [Symbol visibility](#symbol-visibility)
- [Memory and threads](#memory-and-threads)
- [Errors](#errors)
- [Versioning](#versioning)
- [Tests](#tests)
- [Chapter changes](#chapter-changes)

## Scope

An `.antl` file is how Anti code is reused from Anti. A static or shared library is how
Anti code is reused from C. The two are built from the same source and the same
`anti.toml`. `anti build --lib static` and `anti build --lib shared` add the second
form. Nothing about the `.antl` changes.

A library project has no `main`. `anti build --lib` refuses a project that defines one.
The C side owns the process.

Two uses drive the design. CLAP and VST3 plugins are shared libraries with an exported
C entry point. Libraries written in Anti are called from C, C++, Python, Rust or Go
through their C bindings.

## Export marker

Mangled names such as `geometry.dot` are not C identifiers. The `export` keyword gives
a function its plain C symbol:

```anti
export fn dot(a: Vec2, b: Vec2) -> c_int
{
    return a.x * b.x + a.y * b.y;
}

export struct Vec2
{
    x: c_int,
    y: c_int,
}
```

Rules:

- `export` implies `pub`.
- An `export fn` has the unmangled symbol `dot`, the same rule as `extern fn`. Two
  modules may not export the same name. `anti check` reports it.
- An `export struct` or `export union` appears in the generated header. Its layout is
  already C layout.
- `export const` appears in the header as a `#define` for integer and float values and
  as a `static const` for strings.
- `export` on a project built as an executable is allowed and has no effect beyond the
  symbol name.

## Signature rule

An `export fn` may use only types with a C representation:

- Sized integers, `int`, `uint`, `float`, `bool`, `f32`, `f64`, and the `c_` types.
- Pointers `*T` where `T` follows this rule, plus `*byte` for `void*`.
- `export struct` and `export union` by value or by pointer.
- Function pointers whose signature follows this rule.

Not allowed in an export signature: `char`, `str`, slices, fixed-size arrays by value,
and structs that are not exported. `anti check` reports a violation with the parameter
name and the reason.

`str` and `[]T` cross the boundary as two arguments, pointer and length. Every `str`
and slice has the fields `.ptr` and `.len` for that. A C caller passing a string uses a
NUL-terminated `*byte`, and the callee converts with `text.from_c(p)` from `anti.text`.

## Header generation

`anti bind --header <name>.antl` writes `<name>.h` from the public interface in the
library file. Because it reads the `.antl`, the header can be generated for a library
whose source the C side never had.

The header contains, in this order:

1. An include guard and `#include <stdint.h>`, `<stdbool.h>`, `<stddef.h>`.
2. `#ifdef __cplusplus extern "C" {` and the matching close.
3. Every `export struct` and `export union` as a C definition. `c_` types map back to
   C names. Sized types map to `<stdint.h>` names, and `int`, `uint` and `float` to
   `int64_t`, `uint64_t` and `double`. `packed` becomes `#pragma pack`.
   `align(N)` becomes `_Alignas(N)`.
4. Every `export const`.
5. A prototype per `export fn`. `*byte` becomes `uint8_t*`. Anti has no const
   pointers, so the header has no `const`.
6. The `///` comment of every exported item as a `/** */` comment above it. An IDE
   then shows the same text a C author would have written.

The header is regenerated on every `anti build --lib`. Do not edit it.

## Outputs per target

`anti build --lib static` writes:

| Target | File |
|---|---|
| `linux-*` | `dist/<os>-<cpu>/<mode>/lib<name>.a` |
| `macos-*` | `dist/<os>-<cpu>/<mode>/lib<name>.a` |
| `windows-*` | `dist/<os>-<cpu>/<mode>/<name>.lib` |

`anti build --lib shared` writes:

| Target | Files |
|---|---|
| `linux-*` | `lib<name>.so` |
| `macos-*` | `lib<name>.dylib` |
| `windows-*` | `<name>.dll` and the import library `<name>.lib` |

Both forms write `<name>.h` next to the library.

The static archive is created with `llvm-ar`, taken from the same pinned LLVM release
as `llvm-mc` and added to the runtime archive. `llvm-ar` writes ELF, Mach-O and COFF
archives. The shared library and the Windows import library are created by the platform
linker with `-shared`, `-dynamiclib` or `/DLL`.

## Runtime

Anti code needs the runtime when it uses `parallel`, the standard library, or the
embedded licence text. How the runtime reaches the C program differs per form.

Shared library: the runtime is linked into the `.so`, `.dylib` or `.dll` and
initialised by a constructor that runs on load. The emitter writes the constructor
reference into `.init_array` on ELF, `__DATA,__mod_init_func` on Mach-O and `.CRT$XCU`
on COFF. A C program calls exported functions without an initialisation call. Every
runtime symbol is hidden, so two Anti shared libraries in one process do not conflict.

Static library: the runtime is not in the archive. Two Anti static libraries in one C
program would otherwise define every runtime symbol twice. The runtime archive ships
`libanti_rt.a` and `anti_rt.lib` per target, and `anti build --lib static` prints the
link line:

```text
cc main.c libgeometry.a /path/to/lib/linux-x86_64/libanti_rt.a -lpthread -lm
```

The runtime initialises itself on first use in the static case. The thread pool is
created by the first `parallel`. Nothing runs before the C program calls an exported
function.

`--bundle-runtime` puts the runtime objects into the static archive for the single-file
case. The printed link line then omits `libanti_rt.a`. The header carries a comment
that only one bundled Anti archive may be linked into a program.

The bundled native libraries behind `anti.raylib`, `anti.miniaudio`, `anti.net` and
`anti.regex` are never in the archive. The printed link line names them, as the driver
does for executables.

## Symbol visibility

In a shared library only `export` symbols are visible. The emitter marks every other
symbol `.hidden` on ELF, `.private_extern` on Mach-O, and writes a `.def` file listing
the exports for the Windows linker. `nm -D` or `dumpbin /exports` on the result shows
the exported functions and nothing else.

In a static archive every symbol is present, since the archive is object files. The
mangled names of non-exported functions cannot collide with C names. They contain a dot
on ELF and Mach-O and the COFF-safe variant on Windows.

`anti_licenses` is present and visible in the shared form, so `anti license --from`
works on a shared library too. A static archive carries a copy of the package header of
its `.antl` instead, which `anti license --from-archive` reads.

## Memory and threads

Memory crosses the boundary as it does between two C libraries. Anti allocates with the
same `malloc`, so a C caller may `free` what an exported function returned. The `///`
comment of each exported function states who frees what, and the header carries it.

A C thread calling an exported function is the dispatching thread for any `parallel`
inside it. The pool is shared by every exported function in the library. A C program
that calls exported functions from two threads at once gets two `parallel` blocks
sharing one pool. Saturation runs jobs inline, as decided for the language.

Anti code holds no global mutable state, so an exported function is reentrant unless its
`///` comment says otherwise.

## Errors

Anti has no exceptions and the boundary adds none. An exported function reports failure
through its return value or an out parameter, as a C function does. A runtime failure
that Anti cannot report, such as an assertion in the runtime itself, calls `abort()`.
The header states this in its top comment.

## Versioning

`anti build --lib shared --soname` writes the version from `anti.toml` into the file:
`libgeometry.so.1` with a `libgeometry.so` symlink on Linux, `-install_name` and
`-compatibility_version` on macOS, no equivalent on Windows. Without the flag the
library carries no version. The major version is the compatibility version. A change to
any exported signature or struct requires a major bump, and `anti publish` compares the
exported interface of the previous version in the index to enforce it.

## Tests

| Test | Checks |
|---|---|
| Header compiles | `<name>.h` compiles with `cc -std=c11 -Wall -Werror` and `c++ -std=c++17` on every target |
| Round trip | A C program calls every exported function of a fixture library with struct-by-value arguments and compares results with the Anti test suite |
| Exports only | The shared library's export table equals the set of `export` names plus `anti_licenses` |
| Two libraries | Two Anti shared libraries load into one C process. Two static archives with `libanti_rt.a` link into one program |
| Constructor | An exported function using `parallel` works as the first call after `dlopen` |
| Bundle conflict | Two `--bundle-runtime` archives in one program fail to link with a duplicate symbol error naming the runtime |

## Chapter changes

- Chapter 2: the `export` keyword and the signature rule.
- Chapter 6: the export checks in semantic analysis.
- Chapter 9: `export` items in the public interface of the `.antl`.
- Chapter 16: hidden visibility, `.def` files, constructor sections per object format.
- Chapter 23: `llvm-ar`, `libanti_rt.a` and `anti_rt.lib` in the runtime archive.
- The build tool chapter: `--lib`, the printed link line, `--bundle-runtime`,
  `--soname`, `--from-archive`, described without the code of `anti`.
- New extension chapter after the build tool chapter: libraries for C. Header
  generation, the runtime in both forms, memory and threads across the boundary, the
  tests above.
