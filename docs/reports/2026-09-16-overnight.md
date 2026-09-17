# Overnight report, 2026-09-16

The compiler work of the four designs is in `src/`, `rt/` and `tests/`. On the development Mac, macOS arm64, the build has zero warnings and all 272 ctest tests pass. `docs/decisions.md` holds each decision below in full.

## Provisional decisions

### Lexer, parser and types

- `////`, `/**/` and `/***` are ordinary comments. Rule lines and banners stay comments.
- Doc block text sits between the delimiter lines, and text on a delimiter line is an error. The block form then gives the text of the line form.
- Two doc comments of one marker before one item join with a blank line. No text is lost.
- Module docs are the `//!` and `//#!` comments before the first import or item, and a stray doc comment is dropped. Doc warnings belong to `anti check`.
- `uint` and the `c_` type names are keywords, like every type name.
- `c_wchar` is unsigned on every target, since its values are code units.
- A `c_long`, `c_ulong` or `c_wchar` constant fits the narrower width. It then has one value on every target.
- A conversion to or from those types truncates when the target type is never wider and extends otherwise. One IR instruction serves every target.
- A union literal is not a constant, because a constant holds every field.
- A union may hold bitfields, each at bit 0, as in C.
- A bitfield is 1 bit up to the bits of its type wide, with no zero width. A zero width names no field.
- A bitfield needing more than 8 bytes is a back-end error. The lowering reads one integer of at most 8 bytes.
- `align(N)` takes a constant `int` power of two, as `_Alignas` does.
- `align(N)` below the natural alignment is a back-end error. Only the back end knows that alignment.
- A `size_of` value converts only to an integer in a constant. The back end folds integers.

### Export and header

- Export aggregate fields follow the signature rule or are fixed arrays of such types. The header spells every field in C.
- An aligned export aggregate has no bitfield as its first field. The header puts `_Alignas` there, and C refuses it on a bitfield.
- An `export const` is numeric, `bool` or `str`, and `main` cannot be exported. The header has a form for each, and the runtime owns `main`.
- Whole-program optimisation keeps every `export fn`, since C may call it.
- The header maps `c_long`, `c_ulong` and `c_wchar` to `long`, `unsigned long` and `wchar_t`, and names parameters `p0`, `p1`. A library file keeps no parameter names.

### IR and back end

- A stride is `size_of` of the element, and a first field has offset 0 without a symbol. C fixes both.
- Folding wraps at the type width, and division by zero names the target. It matches run-time arithmetic.
- The optimizer runs again after folding. Every machine code dump stayed the same.

### Driver, library files and libraries for C

- Search roots are `-I` options, and the module path comes from the source path under the first root. The path mirrors the tree.
- `--anti-internal` allows `anti.` paths, a bare `anti` is reserved and `anti.rt` is refused. `anti.rt.main` is the entry symbol.
- Package header options replace the manifest, with the module path, `0.0.0` and empty licence fields as defaults. antic works without `anti.toml`.
- The interface keeps the module's `//!` text, and `--strip-docs` removes it. User docs build from a `.antl` alone.
- A `//#` note on a `pub` item without `///` warns in every compilation. Semantic analysis sees the comments.
- `--dev` compiles one module into one object, with its functions global and hidden. Release mode is `antic -c` per module and one final call.
- `--dev` also compiles a library file into its object, which never links. Dev mode compiles every `.antl` of the graph, and a main module is source.
- `antic --lib static|shared` writes the library and `<name>.h`, which is compiler work.
- A static library keeps its package header in `<name>.package.o`. Apple's linker refuses other members.
- `--bundle-runtime` joins objects with `ld -r`, or adds members on Windows. Two bundled runtimes then clash at link time.
- `anti_licenses` is plain text lines between markers, each package name once, as `anti license --from` prints.
- `--soname` needs `--package-version` and follows the soname and install-name conventions.
- A shared library's constructor and an executable's `main` both call `anti_rt_init`. A DLL's `.def` file lists the export functions and `anti_licenses DATA`.

### Book and CI

- `tools/scripts/format_anti.py` applies the formatter style, with four spaces per tab in listings, which `anti html` renders.
- Chapters 24 and 25 stay drafts until chapters 21 to 23 exist.
- The macOS x86_64 job takes LLVM from Homebrew and the Windows jobs from Chocolatey, since LLVM 23.1.1 publishes no archive for them.

## Failed steps

- **Sanitizer program tests:** under ASan and UBSan, 240 of 272 tests pass. The other 32 link Anti programs against a runtime built with sanitizer flags, and antic calls `ld` without the sanitizer runtime (`___asan_init` undefined).
- **CI matrix:** `.github/workflows/test.yml` runs on `workflow_dispatch` only and has not run, by the CI rule.
- **Tracked assembly files:** `*.s` in `.gitignore` had kept the expected files of the emit tests out of git, so a fresh checkout failed them. Commit 6af6669 adds them.
- **Review findings:** the chapter review found eight defects, each fixed test first. Four were in the front end: two messages for one mistake, `export extern fn`, `.len` of a local array as a constant and an aligned bitfield in a header. The others were private field docs in library files, a repeated notice line and `--dev` refusing library files, besides listings without a test.

## Chapters rewritten

Chapters 1 to 20 are rewritten, with every listing in formatter style and pinned by a test. Chapters 24, The build tool, and 25, Libraries for C, are new drafts. `docs/table-of-contents.md` lists 27 chapters, with the standard library and the guide moved to 26 and 27. All chapters pass `check-web-content`, and the site build passes with and without drafts.

## Not done

- **The `anti` tool:** the scope leaves its code out. The tests that need it wait: resolver, offline build, format stability, highlighter coverage, header consistency with `anti.toml`, doc equivalence through `anti doc`, and `anti license --from-archive`. `antl_docs_equivalent` compares the library files of both comment forms.
- **Standard library modules:** `anti.io`, `anti.net`, `anti.regex`, `anti.license`, `anti.text`, `anti.raylib` and `anti.miniaudio` do not exist in `std/`. The renames live in the designs, decisions and chapters.
- **Constructor test with `parallel`:** threads are chapter 22. `clib_loader` checks `anti_rt_init` as the first call after `dlopen`.
- **Native libraries in the printed link line:** no bundled native library exists yet, so `link_line` names none.
- **Cross-target ABI probe:** the probe runs on the host only.

## Host-only tests

For the other five targets ctest assembles the new features with llvm-mc 23.1.1 and compares `.antl` bytes. These are unit tests or assembly only:

- the MSVC bitfield rule, and `c_long` and `c_wchar` at 32 and 16 bits, checked against clang's sizes
- shared libraries, `.def` files and `.CRT$XCU` constructors on Windows
- `.init_array` and `--soname` on Linux
- the AAPCS64 even-register rule for 16-aligned aggregates outside Apple
- the `clib_*` tests, which ctest skips on Windows, and the header compile test on other targets
