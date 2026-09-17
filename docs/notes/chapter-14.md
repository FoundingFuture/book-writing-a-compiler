# Chapter 14 notes

Choices made while writing chapter 14, The x86_64 back end. They describe the inside of the
compiler. `docs/decisions.md` holds what a reader of the language or a user of
the tools can observe.

- Chapter 14 x86_64 patterns cover integers of 8, 16, 32 and 64 bits and pointers. Floating-point operations and aggregates remain for the chapters that lower them.
- An 8-bit or 16-bit multiplication uses the 32-bit registers, because `imul` has no two-operand form for 8 bits. An 8-bit or 16-bit division extends both operands to 32 bits first.
- Division moves the dividend into `rax`. `cqto` or `cltd` sign-extends it into `rdx`, and an unsigned division loads 0 into `rdx`. The quotient comes from `rax`, the remainder from `rdx`. A variable shift count moves into `cl`.
- `zext` from 32 bits is a 32-bit `mov`, whose result the processor zero-extends to 64 bits. `trunc` is a `mov` of the result width.
- A `ptradd` whose only use is the next `load` or `store` folds into its memory operand. A preceding `shl` by 0 to 3 or `mul` by 1, 2, 4 or 8 of the index folds in as the scale. The displacement must fit 32 bits. A `store` with an index folds only when it stores an immediate, so that an instruction reads at most two registers.
- `addr` of a function or global is `lea` relative to `rip`.
- Stack arguments start above the Windows shadow store, 8 bytes each, the first one lowest. The largest outgoing area of a function lies at the bottom of its frame, below the slots. Incoming stack parameters are read from `16(%rbp)` upward, and on Windows x64 the first four parameters count toward that offset.
- A variadic call under System V loads 0 into `eax`, the number of vector registers in use. Since chapter 18, System V callers extend register arguments of 8 and 16 bits to 32 bits. Windows x64 callers do not extend.
- The Windows x64 epilogue frees the frame with `add $size, %rsp`, one of the two forms that Microsoft allows before the pops. The unwind data names no frame register, so the `lea` form from `rbp` does not apply. System V keeps `mov %rbp, %rsp`.
- Selection skips a parameter that the function never reads, so an unused stack parameter costs no load.
- Windows x64 frames of 4096 bytes or more call `__chkstk` with the size in `eax` and then subtract `rax` from `rsp`.
