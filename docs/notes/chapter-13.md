# Chapter 13 notes

Choices made while writing chapter 13, Register allocation and stack frames. They describe the inside of the
compiler. `docs/decisions.md` holds what a reader of the language or a user of
the tools can observe.

- Chapter 13 register allocation is linear scan after Poletto and Sarkar, with one interval per virtual register. Instruction k reads at position 2k and writes at 2k + 1. Physical registers named by instructions or overwritten by calls form fixed ranges. Moves give hints. The spilled interval is the one that ends last. Candidates are the new interval and the active intervals whose register it can take.
- Allocation order on ARM64: `x9` to `x15`, `x0` to `x8`, `x19` to `x28`. `x16` and `x17` are spill scratch registers, and `x18` is never allocated. On System V x86_64: `rax`, `rcx`, `rdx`, `rsi`, `rdi`, `r8`, `r9`, `rbx`, `r12` to `r15`. On Windows x64 `rsi` and `rdi` move to the callee-saved end. `r10` and `r11` are spill scratch registers, and `rbp` is the frame pointer.
- A function gets a frame when it calls or uses the stack. ARM64 frames start with the frame record `stp x29, x30, [sp, #-16]!`, and x86_64 frames with `push %rbp`. Slots lie at the bottom of the frame above the Windows shadow store. Saved callee-saved registers lie at the top. The size is a multiple of 16 and at most 4095 bytes in this chapter. antic does not use the System V red zone.
- The patterns for `slot`, `load`, `store` and `ptradd` arrive in chapter 13, so that address-taken locals live in memory. `--dump-alloc` prints the machine code after allocation.
