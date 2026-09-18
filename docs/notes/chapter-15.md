# Chapter 15 notes

Choices made while writing chapter 15, The ARM64 back end. They describe the inside of the
compiler. `docs/decisions.md` holds what a reader of the language or a user of
the tools can observe.

- Chapter 15 ARM64 patterns cover integers of 8, 16, 32 and 64 bits and pointers. Values of 8, 16 and 32 bits live in w registers, and the bits above their width are unknown, as on x86_64.
- A comparison of 8 or 16 bits extends the first operand with `sxtb`, `sxth`, `uxtb` or `uxth` into a new register and extends a register second operand inside `cmp`, as in `cmp w9, w1, sxth`. `eq` and `ne` extend with zeros. A constant from -4095 to -1, or its shifted form, compares with `cmn`.
- Division uses `sdiv` or `udiv` on 32-bit or 64-bit registers after extending 8-bit and 16-bit operands. A remainder is `msub r, q, b, a`, the value `a - q * b`.
- Shifts use `lsl`, `asr` and `lsr`. A right shift of 8 or 16 bits extends the value first. A constant count of the register width or more goes into a register, where the processor takes it modulo the width.
- `sext` uses `sxtb`, `sxth` or `sxtw`. `zext` uses `uxtb` or `uxth`, and from 32 bits a move of the w register. `trunc` is a move of the w register.
- Immediates: `add`, `sub`, `cmp` and `cmn` take a 12-bit value or a 12-bit value shifted by 12, printed as `#3, lsl #12`. A negative constant turns `add` into `sub` and `sub` into `add`. `and`, `orr` and `eor` take logical immediates. Other constants go into a register with `mov`, `movz` and `movk`.
- A `ptradd` folds into a load or store in one of three forms. The forms are an unsigned 12-bit offset scaled by the size and a signed 9-bit offset. The third is an index shifted by 0 or by the scale of the size. A store of zero reads `wzr` or `xzr`, and a store with an index folds only for zero.
- `addr` is `adrp` and `add` with `:lo12:`. The dumps print this ELF form for every ARM64 target. Chapter 16 writes `@PAGE` and `@PAGEOFF` for Mach-O, which llvm-mc requires there.
- Stack arguments under AAPCS64 and Windows ARM64 take 8-byte slots. Apple gives a named argument its own size at its own alignment and each variadic argument an 8-byte slot, and pads the area to 8 bytes. Stack parameters are read from `16` bytes above `x29` plus the same offsets.
- Apple callers extend register arguments of 8 and 16 bits. IR parameters carry `signext` or `zeroext` for that, set from the Anti type. The library format gains an extension byte after each parameter type.
- Windows ARM64 has its own register table, equal to Apple's, and probes frames of 4096 bytes or more with `__chkstk`, the size divided by 16 in `x15`.
- ARM64 frames have no size limit. A frame size or slot offset that no 12-bit immediate holds goes through `x16` or the destination register. Offsets from `sp` beyond 32760 bytes for spills and saved registers go through a register. The target hook `expand` splits a slot address after allocation.
