# Chapter 19 notes

Choices made while writing chapter 19, Strings, slices and bytes. They describe the inside of the
compiler. `docs/decisions.md` holds what a reader of the language or a user of
the tools can observe.

- Chapter 19 lowers `str` and `[]T` as aggregates of 16 bytes, `ptr` at offset 0 and `len` at offset 8. `.ptr` and `.len` load a field. `s[i]` loads the pointer and indexes from it. `a[lo..hi]` stores the address of element `lo` and `hi - lo`, after evaluating the base, `lo` and `hi` in that order. A slice literal stores its fields in source order.
- A string literal and a byte string literal become a global of the module that holds the bytes and a NUL after them. The global is named by its decimal index among the module's globals, as `main.0` or `_A4main_0` on COFF. No identifier starts with a digit, so no function gets that name. Literals and `str` constants with equal bytes share one global.
- The emitter writes global data after the functions: `.section .rodata` on ELF, `.section __TEXT,__const` on Mach-O and `.section .rdata,"dr"` on COFF. Each global is a local label, `.p2align` for an alignment above 1 and `.byte` lines of up to 16 hexadecimal values. A global with a relocation stops emission with `global data ... holds an address, which antic does not emit`, because no Anti expression produces one.
- The former Open item on System V array variables does not apply. antic's globals hold literal bytes under local symbols, and no C file can declare them.
- A program test reads the lines of an optional `NAME.args` beside its source as command-line arguments. The test `program_args` sets `ANTI_TEST_GREETING`.
