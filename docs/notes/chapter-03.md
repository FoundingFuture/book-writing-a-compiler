# Chapter 3 notes

Choices made while writing chapter 3, Setup and the first executable. They describe the inside of the
compiler. `docs/decisions.md` holds what a reader of the language or a user of
the tools can observe.

- Chapter 3 direct path: antic recognises one program shape, `fn main() -> int { return N; }`, and emits assembly for macos-arm64 only. Chapter 16 adds the five other targets.
- The sources of `anti_rt` live in `rt/` at the repository root. The main CMake build compiles `anti_rt` for the host into `build/runtime/lib/<target>/libanti_rt.a`, the layout of the runtime archive, with hidden visibility, and copies `rt/LICENSE` to `build/runtime/licenses/anti_rt.txt`.
- `rt/start.c` called a `main` without parameters until chapter 19 added strings and slices.
- Program tests: `tests/programs/NAME.anti` with `NAME.expected`. The expected file starts with the line `exit N`, and the bytes after it are the expected standard output. A test fails when antic prints anything.
- [provisional] The direct path of chapter 3 lives in `direct-path/` of its folder with its driver, emitter, target table, `start.c` and the expected `return42.s`. Chapters 4 and 5 keep only their `direct.c` and `direct.h` there. The programs `direct_path_04` and `direct_path_05` take the chapter 3 driver and the lexer and parser of `src/`. Reason: the chapter 3 driver calls `direct_parse`, which the later versions define with the same signature.
- The chapter 3 files use the module `anti.rt` and the library `libanti_rt.a`, the names that the text of chapter 3 shows, where the commit of chapter 3 had `anti` and `libantirt.a`. Reason: the listing of `return42.s` names `anti.rt.main`, and its link command names `libanti_rt.a`.
- [provisional] The tests `direct_path_<chapter>_assembly` and `direct_path_<chapter>_missing_semicolon` run on every host, and `direct_path_<chapter>_return42` and `_return_pieces` link with `ld` on macos-arm64 only. `start_01_return42` links the `start.c` of chapter 1 with the output of antic on Linux and macOS. Reason: the direct path emits macos-arm64 assembly and links with the Command Line Tools.
