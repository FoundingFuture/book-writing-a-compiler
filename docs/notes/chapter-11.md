# Chapter 11 notes

Choices made while writing chapter 11, Targets, object formats and ABIs. They describe the inside of the
compiler. `docs/decisions.md` holds what a reader of the language or a user of
the tools can observe.

- Chapter 11 target table in `src/target.c`. Each row holds the operating system, processor, object format, calling convention, llvm-mc triple and file suffixes. Triples: `x86_64-unknown-linux-gnu`, `aarch64-unknown-linux-gnu`, `x86_64-apple-macos`, `arm64-apple-macos`, `x86_64-pc-windows-msvc`, `aarch64-pc-windows-msvc`. Windows uses `.obj` and `.exe`. `--print-targets` prints the matrix, and `mangle` chooses the symbol form by object format.
