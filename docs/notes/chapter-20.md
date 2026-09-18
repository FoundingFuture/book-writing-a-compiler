# Chapter 20 notes

Choices made while writing chapter 20, Function pointers. They describe the inside of the
compiler. `docs/decisions.md` holds what a reader of the language or a user of
the tools can observe.

- Chapter 20 lowers a function used as a value to `addr`. A call of any other expression of function type is `call` with the address in operand `a` and a signature in operand `b`. The signature is a function `fn.N` that the module declares and never defines, named by its index among the module's signatures. Equal parameter and result types share one signature. Library files carry signatures as declared functions, without a format change. The IR prints `call T %t via @module.fn.N(args)`.
- Every type of chapter 2 lowers since chapter 20, so lowering no longer refuses a type with a chapter number. The IR verifier checks the argument count of a call through a pointer against its signature.
- x86_64 calls through a register with `call *%reg`, and ARM64 with `blr`. The target register is an ordinary use of the call, so the allocator keeps it clear of the argument registers.
- The address of a C function comes from its GOT entry on Linux and macOS: `movq sym@GOTPCREL(%rip)` on x86_64, `adrp :got:sym` and `ldr [.., :got_lo12:sym]` on ARM64 ELF, `@GOTPAGE` and `@GOTPAGEOFF` on Mach-O. Windows keeps `lea` and `adrp` with `add`, because the C runtime is linked statically. Apple clang 21 with `-fPIE` emits the same forms for these targets. This settles the former Open item on addresses of C functions. Anti has no extern variables, so C data needs no GOT.
