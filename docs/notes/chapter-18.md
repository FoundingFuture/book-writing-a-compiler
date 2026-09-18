# Chapter 18 notes

Choices made while writing chapter 18, Structs and arrays. They describe the inside of the
compiler. `docs/decisions.md` holds what a reader of the language or a user of
the tools can observe.

- Chapter 18 lowers a struct or array expression to the address of its value. Every aggregate local gets a slot in the entry block, and an aggregate parameter is the pointer the back end gives it. A literal fills its destination element by element. `[v; n]` computes `v` once and fills the elements in a loop.
- An aggregate assignment or `let` from another aggregate is `memcopy`. A function with an aggregate result returns its address with `ret ptr`, and the back end moves the value to where the convention puts it. `.len` of an array is its constant length, and the base expression is still evaluated.
- The back end classifies an aggregate of up to 32 bytes by the members of its layout. Every convention passes a larger aggregate in memory, and an AAPCS64 float aggregate has at most four members of 8 bytes.
- `memcopy` of up to 64 bytes moves 8, 4, 2 and 1 bytes at a time through a scratch register. A larger copy calls `memcpy`.
- System V: each eightbyte of an aggregate of up to 16 bytes is SSE when all its members are floats and INTEGER otherwise. The aggregate takes registers only when all its eightbytes fit, and goes to the stack whole otherwise. A larger aggregate is MEMORY, passed on the stack. A MEMORY result goes to an address that the caller passes in `rdi` and the callee returns in `rax`.
- A System V stack argument takes whole eightbytes, and one whose type aligns to 16 starts at the next multiple of 16. clang passes the second 16-aligned struct of `abi_wide` at `16(%rsp)`, and antic placed it at `8(%rsp)` until `program_abi_structs_macos-x86_64` compared the two.
- Windows x64: an aggregate of 1, 2, 4 or 8 bytes passes and returns as an integer of that size. Another aggregate passes as a pointer to a copy that the caller makes, aligned to 16 bytes. Its result goes to an address in `rcx`, which moves the other arguments one position, and returns in `rax`.
- AAPCS64 and its Apple and Windows variants: a float aggregate of one to four members of one type takes float registers, and another aggregate of up to 16 bytes takes x registers, all or none. When they do not fit, the float or integer registers count as used. A larger aggregate passes as a pointer to a copy. Results come back in `v0` to `v3`, in `x0` and `x1`, or through an address in `x8`.
- Apple places a float aggregate on the stack at the alignment of its members, and other aggregates in 8-byte slots. A clang-built callee reads it that way on the development Mac. The test `program_abi_structs` checks it with `abi_stack`.
- Register parts of 3, 5, 6 or 7 bytes load and store in pieces joined with shifts, so no copy reads or writes beyond the aggregate.
- System V callers extend register arguments of 8 and 16 bits to 32 bits. Apple clang 21 compiles `int widen(short s, unsigned char u) { return s + u; }` for `x86_64-unknown-linux-gnu` to `leal (%rdi,%rsi), %eax`, which reads 32 bits of each register. For a stack argument and for `x86_64-pc-windows-msvc` the clang callee extends. This replaces the chapter 14 rule that callers do not extend.
- Chapter 18 binds ten raymath functions by hand in `tests/abi/abi_raymath.anti`. `tests/abi/raymath.c` compiles them out of line with `RAYMATH_IMPLEMENTATION`. The generated binding from `raylib_api.json` stays planned for the library chapters.
