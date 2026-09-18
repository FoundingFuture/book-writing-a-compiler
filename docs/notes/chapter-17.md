# Chapter 17 notes

Choices made while writing chapter 17, Floating point. They describe the inside of the
compiler. `docs/decisions.md` holds what a reader of the language or a user of
the tools can observe.

- Chapter 17 float comparisons follow IEEE 754: `fne` holds for NaN, and `feq`, `flt`, `fle`, `fgt` and `fge` fail. Float comparisons compute a `bool` and do not fuse into branches.
- Float registers get the numbers after the integer registers: `xmm0` to `xmm15` are 16 to 31, and ARM64 `v0` to `v31` are 32 to 63, printed `dn` or `sn`. Each virtual register records its class in the machine function, and the allocator chooses from the register list of that class.
- Float scratch registers: `xmm14` and `xmm15` under System V, `xmm4` and `xmm5` on Windows, whose `xmm6` to `xmm15` are callee-saved, and `v16` and `v17` on ARM64. Windows functions save used `xmm6` to `xmm15` in 16 bytes with `movups`. ARM64 functions save `d8` to `d15` in 8 bytes. System V preserves no xmm register, so a float live across a call spills.
- A float constant loads its bits into an integer register and moves them with `movq`, `movd` or `fmov`. antic writes no constant pool, so floats need no data section.
- x86_64: `movsd` and `movss` move floats between registers and memory, and the two-operand SSE instructions compute. Negation is `xorpd` with the sign bit. A comparison is `ucomisd` with `a` and `ae`, with the operands swapped for `<` and `<=`, and `sete` with `setnp` or `setne` with `setp` for equality.
- x86_64 conversions: `cvtsi2sd` and `cvttsd2si` after extending 8-bit and 16-bit integers to 32 bits and a `u32` to 64 bits. A `u64` above 2^63 converts from `(a >> 1) | (a & 1)` doubled, chosen with a mask from the sign bit. A float of 2^63 or more converts to `u64` after subtracting 2^63, chosen with `cmovae`.
- ARM64: `fadd`, `fsub`, `fmul`, `fdiv`, `fneg`, `fcmp` with the conditions `eq`, `ne`, `mi`, `ls`, `gt` and `ge`, `scvtf`, `ucvtf`, `fcvtzs`, `fcvtzu` and `fcvt`.
- Float arguments: System V and AAPCS64 count integer and float registers separately. Windows x64 gives argument i the register at position i of its class and also passes a variadic float in the integer register. A System V variadic call loads the number of float registers into `eax`. Apple passes a named `f32` on the stack in 4 bytes and every variadic float on the stack. Windows ARM64 passes a variadic float in an integer register with `fmov`.
- A zero extension from 32 bits is its own opcode without the move flag: `movl` on x86_64 and `mov` of w registers on ARM64. Register allocation drops a flagged move of a register into itself. The integer back ends of chapters 14 and 15 had that bug for `x as u32 as u64`.
