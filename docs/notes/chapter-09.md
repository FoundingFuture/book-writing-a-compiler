# Chapter 9 notes

Choices made while writing chapter 9, Modules and library files. They describe the inside of the
compiler. `docs/decisions.md` holds what a reader of the language or a user of
the tools can observe.

- Library format: magic `ANTL`, the version, little-endian integers of fixed width, strings as a u32 length and bytes, floats as IEEE 754 bits. Enum values are stored as bytes, and `_Static_assert` checks guard them. All function signatures precede all bodies. Each instruction is a 49-byte record.
- An IR function of another module is an `extern fn` with a module name, as in `extern fn scale.scale(i64) -> i64`. Loading maps it to the loaded definition.
- The reader checks every count against the bytes left and every index against its table. A damaged file reports `is damaged at byte N`. Other messages: `is not a library file`, `has format version N, and antic reads version M`, `needs module`, `needs struct`.
- Import errors: `` `main` cannot import itself ``, `` `b` depends on `main`, so the import forms a cycle ``, `` `geometry` has no public item `x` ``, `` `geometry` has no public struct `X` ``, `` `geometry` is a module, not a value ``.
