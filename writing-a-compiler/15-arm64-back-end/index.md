---
title: "The ARM64 back end"
description: "The integer patterns of the antic ARM64 back end: extensions, division, immediates, addressing modes, adrp, calls under AAPCS64 and Apple's variant."
summary: "Instruction patterns, immediates and their encoding limits, condition flags and `adrp` addressing. Calls under AAPCS64 and Apple's variant. The emitted assembly for the same test programs."
date: 2026-09-13T22:32:00+02:00
lastmod: 2026-09-15T03:48:32+02:00
draft: false
weight: 150
tags: [compilers, assembly]
keywords: [ARM64, AArch64, logical immediate, adrp, AAPCS64, Apple ARM64 ABI, sign extension, stack arguments]
---

## Previously

[Chapter 14, The x86_64 back end]({{% relref "/programming/writing-a-compiler/14-x86-64-back-end" %}}), completes the integer patterns for x86_64. It adds division through `rdx:rax`, shifts by `cl`, conversions, memory operands with a scaled index, stack arguments under System V and Windows x64, and the Windows stack probe.

## Scope of the back end

Chapter 12 selected ARM64 instructions for 32-bit and 64-bit arithmetic, comparisons, branches and calls with up to eight register arguments. This chapter completes the integer patterns in `src/arm64.c`, which serve all three ARM64 targets. Division, remainder, shifts, conversions and the operation `addr` gain patterns in the table.

```c
    {IR_SDIV, match_arith, emit_div},
    {IR_UDIV, match_arith, emit_div},
    {IR_SREM, match_arith, emit_div},
    {IR_UREM, match_arith, emit_div},
    {IR_SHL, match_arith, emit_shift},
    {IR_SHR_S, match_arith, emit_shift},
    {IR_SHR_U, match_arith, emit_shift},
    {IR_TRUNC, match_convert, emit_convert},
    {IR_SEXT, match_convert, emit_convert},
    {IR_ZEXT, match_convert, emit_convert},
    {IR_ADDR, NULL, emit_addr},
};
```

The patterns accept every integer width from 8 to 64 bits. Calls pass arguments on the stack, functions read parameters from it, and frames have no size limit. The listings come from `--dump-alloc`, as in chapter 14. Floating-point values and aggregates still stop with the messages of chapter 12.

## Registers and widths

ARM64 names register n `xn` for 64 bits and `wn` for 32 bits. Values of 8, 16 and 32 bits live in the w registers. A write to a w register clears the upper 32 bits: the register accessor of the Arm pseudocode zero-extends every written value to 64 bits[^1]. Microsoft states the same rule for Windows[^2].

The data processing instructions compute 32 or 64 bits, as the bit `sf` of each encoding selects[^1]. An 8-bit addition `add w0, w0, #1` on the value 255 leaves 256 in `w0`, whose low 8 bits hold the result 0. The back end therefore treats the bits above the width of an 8-bit or 16-bit value as unknown, as the x86_64 back end does. An instruction that reads the whole register extends the value first, with `sxtb`, `sxth`, `uxtb` or `uxth`.

```c
/* The instruction that extends an 8-bit or 16-bit value to 32 bits. */
static enum a64_op extension(uint8_t n, bool is_signed)
{
    if (n == 8) {
        return is_signed ? A64_SXTB : A64_UXTB;
    }
    return is_signed ? A64_SXTH : A64_UXTH;
}
```

## Comparisons and condition flags

The instruction `cmp` compares 32-bit or 64-bit registers. For a comparison of 8 or 16 bits, the selector extends the first operand into a new register. A register second operand extends inside `cmp` itself, in the extended-register form `cmp w9, w1, sxth`[^1]. A signed comparison sign-extends, and an unsigned one or an equality zero-extends.

```c
/* cmp compares 32 or 64 bits. An 8-bit or 16-bit first operand extends
   into a new register, and a register second operand extends inside cmp.
   A negative constant compares with cmn, which adds its negation. */
static void compare(struct selector *s, const struct ir_inst *inst)
{
    uint8_t n = bits(inst->a.type);
    bool is_signed = inst->op >= IR_SLT && inst->op <= IR_SGE;
    struct mach_operand a = select_reg(s, &inst->a);
    struct mach_operand ops[3];
    int64_t v;

    if (n < 32) {
        struct mach_operand wide = select_new_vreg(s, 32);
        emit2(s, extension(n, is_signed), wide, a);
        a = wide;
    }
    if (inst->b.kind == IR_INT) {
        v = n < 32 && !is_signed ? (int64_t)inst->b.as.integer
                                 : signed_value(inst->b.as.integer, n);
        ops[0] = a;
        if (fits_imm12(v)) {
            emit_imm12(s, A64_CMP, 1, ops, v);
        } else if (v < 0 && fits_imm12(-v)) {
            emit_imm12(s, A64_CMN, 1, ops, -v);
        } else {
            struct mach_operand b = select_new_vreg(s, a.width);
            load(s, b, (uint64_t)v);
            emit2(s, A64_CMP, a, b);
        }
        return;
    }
    if (n < 32) {
        ops[0] = a;
        ops[1] = select_reg(s, &inst->b);
        ops[2] = mach_imm(extension(n, is_signed));
        select_emit(s, A64_CMP, 3, ops);
        return;
    }
    emit2(s, A64_CMP, a, select_reg(s, &inst->b));
}
```

The instruction `cmp a, b` sets the flags of `AddWithCarry(a, NOT(b), 1)`, and `cmn a, #k` those of `AddWithCarry(a, k, 0)`[^1]. Take b = -k with k from 1 to 4095. The unsigned sums are `UInt(a) + 2^N - UInt(b)` and `UInt(a) + k`, which are equal. The signed sums are `SInt(a) - SInt(b)` and `SInt(a) + k`, also equal. The flags N, Z, C and V come from the result and these two sums[^1], so `cmn a, #5` sets the flags of a comparison with -5. The selector compares with a constant from -4095 to -1 that way, because `cmp` takes no negative immediate.

| Condition | IR comparisons | Flags that hold |
|---|---|---|
| `eq`, `ne` | `eq`, `ne` | Z set, Z clear |
| `lt`, `ge` | `slt`, `sge` | N differs from V, N equals V |
| `gt`, `le` | `sgt`, `sle` | Z clear and N equals V, the opposite |
| `lo`, `hs` | `ult`, `uge` | C clear, C set |
| `hi`, `ls` | `ugt`, `ule` | C set and Z clear, the opposite |

The flags column follows the function `ConditionHolds` of the Arm pseudocode[^1]. The function `narrow` of the test program `tests/dump/x86.anti` from chapter 14 compares two `i16` values. The test `dump_alloc_x86.arm64` compares its listing on linux-arm64 with `tests/dump/x86.arm64.alloc`.

```text
x86.narrow:
b0:
    sxth w9, w0
    sxth w10, w1
    sdiv w9, w9, w10
    sxth w10, w0
    sxth w11, w1
    sdiv w12, w10, w11
    msub w10, w12, w11, w10
    sxth w11, w0
    cmp w11, w1, sxth
    b.ge b2
b1:
    mul w0, w9, w10
    ret
b2:
    sxth w9, w9
    asr w9, w9, #1
    lsl w10, w10, #3
    eor w0, w9, w10
    ret
```

The comparison `a < b` becomes `sxth w11, w0` and `cmp w11, w1, sxth`. The shift `q >> 1` in block `b2` extends `q` with `sxth` before `asr`, for the same reason.

## Division and remainder

The instructions `sdiv` and `udiv` divide one register by another[^1]. The instruction `sdiv` writes 0 for a division by zero and changes no flags[^1]. The instruction set has no remainder instruction. The instruction `msub d, n, m, a` computes `a - n * m`[^1], so a remainder is `msub r, q, b, a` after the quotient q.

```c
/* sdiv and udiv divide registers of 32 or 64 bits, so 8-bit and 16-bit
   operands extend first. msub computes the remainder a - (a / b) * b. */
static void emit_div(struct selector *s, const struct ir_inst *inst)
{
    bool is_signed = inst->op == IR_SDIV || inst->op == IR_SREM;
    enum a64_op op = is_signed ? A64_SDIV : A64_UDIV;
    struct mach_operand r = select_result(s, inst);
    struct mach_operand a = extended(s, &inst->a, is_signed);
    struct mach_operand b = extended(s, &inst->b, is_signed);
    struct mach_operand ops[4];

    if (inst->op == IR_SDIV || inst->op == IR_UDIV) {
        emit3(s, op, r, a, b);
        return;
    }
    ops[0] = r;
    ops[1] = select_new_vreg(s, r.width);
    ops[2] = b;
    ops[3] = a;
    emit3(s, op, ops[1], a, b);
    select_emit(s, A64_MSUB, 4, ops);
}
```

The helper `extended` returns the register of an operand extended to at least 32 bits. It loads a constant as its extended value, extends a register of 8 or 16 bits with `sxtb`, `sxth`, `uxtb` or `uxth`, and returns a wider register unchanged.

```text
x86.div:
b0:
    sdiv x9, x0, x1
    sdiv x10, x0, x1
    msub x10, x10, x1, x0
    add x0, x9, x10
    ret
```

The IR has `sdiv` and `srem` as two instructions, so the listing divides twice, as on x86_64. The remainder `msub x10, x10, x1, x0` reuses the register of its quotient. In `narrow` above, both operands of each 16-bit division pass through `sxth` first.

## Shifts

The instructions `lsl`, `asr` and `lsr` shift by a register or by a constant. A shift by a register takes the count "modulo the register size in bits"[^1], 32 or 64. Anti leaves a count of the width or more undefined, so a count below the width reaches the instruction unchanged. A constant count of the register width or more has no immediate form, and the selector loads it into a register.

```c
/* lsl, asr and lsr shift by a register, modulo the width of the register,
   or by a constant below that width. A right shift of an 8-bit or 16-bit
   value extends it to 32 bits first. */
static void emit_shift(struct selector *s, const struct ir_inst *inst)
{
    enum a64_op op = inst->op == IR_SHL     ? A64_LSL
                     : inst->op == IR_SHR_S ? A64_ASR
                                            : A64_LSR;
    struct mach_operand r = select_result(s, inst);
    struct mach_operand a = inst->op == IR_SHL
                                ? select_reg(s, &inst->a)
                                : extended(s, &inst->a, op == A64_ASR);

    if (inst->b.kind == IR_INT && inst->b.as.integer < r.width) {
        emit3(s, op, r, a, mach_imm((int64_t)inst->b.as.integer));
    } else {
        emit3(s, op, r, a, select_reg(s, &inst->b));
    }
}
```

A left shift needs no extension, because the low bits of the result depend only on the low bits of the value. A right shift moves the upper bits down, so an 8-bit or 16-bit value extends first. The function `ushift` of the test program shifts `u32` values, which need no extension at 32 bits.

```text
x86.ushift:
b0:
    lsr w9, w0, w1
    lsl w10, w1, #2
    udiv w0, w9, w10
    ret
```

The function `shifts` applies all three shifts to `int` values in x registers.

```text
x86.shifts:
b0:
    lsl x9, x0, x1
    asr x10, x0, x1
    add x9, x9, x10
    lsr x10, x0, x1
    add x0, x9, x10
    ret
```

## Conversions

A `sext` becomes `sxtb`, `sxth` or `sxtw`, which copy a value of 8, 16 or 32 bits with sign extension. A `zext` from 8 or 16 bits becomes `uxtb` or `uxth` on the w register, which the processor zero-extends to 64 bits[^1]. A `zext` from 32 bits and a `trunc` are moves of the w register. The `zext` has the opcode `A64_MOVW`, which carries no move flag, so register allocation never drops it as a move of a register into itself.

```c
/* trunc keeps the low bits with a move of the w register. sext uses sxtb,
   sxth or sxtw. zext uses uxtb, uxth or, from 32 bits, a move of the w
   register, because every write to a w register clears the upper 32
   bits. */
static void emit_convert(struct selector *s, const struct ir_inst *inst)
{
    struct mach_operand r = select_result(s, inst);
    uint8_t from = bits(inst->a.type);
    static const enum a64_op sext[] = {A64_SXTB, A64_SXTH, A64_SXTW};

    if (inst->a.kind == IR_INT) {
        load(s, r, inst->op == IR_SEXT
                       ? (uint64_t)signed_value(inst->a.as.integer, from)
                       : inst->a.as.integer);
    } else if (inst->op == IR_TRUNC) {
        move(s, widened(r, 32), widened(select_reg(s, &inst->a), 32));
    } else if (inst->op == IR_ZEXT && from == 32) {
        emit2(s, A64_MOVW, widened(r, 32),
              widened(select_reg(s, &inst->a), 32));
    } else if (inst->op == IR_SEXT) {
        emit2(s, sext[from == 8 ? 0 : from == 16 ? 1 : 2], r,
              widened(select_reg(s, &inst->a), 32));
    } else {
        emit2(s, from == 8 ? A64_UXTB : A64_UXTH, widened(r, 32),
              widened(select_reg(s, &inst->a), 32));
    }
}
```

```text
x86.bytes:
b0:
    mov w9, #3
    mul w9, w0, w9
    uxtb w9, w9
    add w10, w1, #1
    sxtb x10, w10
    add x0, x9, x10
    ret
```

The product `a * 3` of the function `bytes` stays in `w9`, and `uxtb w9, w9` extends its low byte to 64 bits. The sum `b + 1` of the `i8` value is sign-extended with `sxtb x10, w10`.

## Immediates

Every A64 instruction encoding is 32 bits long[^1], so an immediate has only a few bits. The instructions `add`, `sub`, `cmp` and `cmn` take a 12-bit unsigned value, optionally shifted left by 12 bits[^1]. The function `fits_imm12` tests both forms, and `append_imm12` writes the shifted one as `#3, lsl #12`.

```c
/* add, sub, cmp and cmn take a 12-bit immediate, optionally shifted left
   by 12 bits. */
static bool fits_imm12(int64_t v)
{
    return v >= 0 && (v <= 4095 || ((v & 0xfff) == 0 && v >> 12 <= 4095));
}

/* Append op with the immediate v after its count operands, in the form
   #v >> 12, lsl #12 when v is larger than 4095. */
static void append_imm12(struct mach_block *b, enum a64_op op, size_t count,
                         struct mach_operand *ops, int64_t v)
{
    if (v > 4095) {
        ops[count] = mach_imm(v >> 12);
        ops[count + 1] = mach_imm(12);
        append(b, op, count + 2, ops);
    } else {
        ops[count] = mach_imm(v);
        append(b, op, count + 1, ops);
    }
}
```

A negative constant turns `add` into `sub` and `sub` into `add`. The instructions `and`, `orr` and `eor` take a logical immediate instead. The Arm function `DecodeBitMasks` builds it from an element of 2, 4, 8, 16, 32 or 64 bits. The element holds a run of ones, rotated right, and repeats across the register[^1]. A run of all ones is reserved, so neither 0 nor a value of all ones is a logical immediate[^1].

```c
/* and, orr and eor take a logical immediate: an element of 2, 4, 8, 16,
   32 or 64 bits that repeats across the register. The element is one
   rotated run of ones, so it is neither all zeros nor all ones. */
static bool is_logical_imm(uint64_t v, uint8_t w)
{
    uint64_t mask = w == 64 ? UINT64_MAX : UINT32_MAX;
    unsigned e;

    v &= mask;
    if (v == 0 || v == mask) {
        return false;
    }
    for (e = 2; e <= w; e *= 2) {
        uint64_t emask = e == 64 ? UINT64_MAX : ((uint64_t)1 << e) - 1;
        uint64_t element = v & emask;
        unsigned i;
        bool repeats = true;
        for (i = e; i < w; i += e) {
            repeats = repeats && ((v >> i) & emask) == element;
        }
        if (repeats) {
            uint64_t rotated = ((element << 1) | (element >> (e - 1))) & emask;
            return popcount(element ^ rotated) == 2;
        }
    }
    return false;
}
```

A value that repeats with an element of e bits also repeats with 2e bits, and that larger element holds two runs. The function therefore tests only the smallest element that repeats. A single rotated run has exactly two bit positions where the element differs from its rotation by one bit. The multiplication `mul` takes no immediate, and every other constant goes into a register through `mov`, `movz` and `movk` of chapter 12. The test program `tests/dump/arm64.anti` holds the ARM64 cases of this chapter.

```anti
extern fn widen(a: u8, b: i8, c: u16) -> i32;

fn imms(a: int) -> int
{
    let m = (a & 0xff00) | 0x5555555555555555;
    let k = (m ^ 0x1234) + 0x3000;
    if k < 0x7000 {
        return k - 0x2000;
    }
    return k;
}

fn bump(x: u8) -> u8
{
    return x + 1;
}

fn pass(x: u8, y: i8) -> i32
{
    return widen(bump(x), y, 7);
}

fn last(a: u8, b: i16, c: int, d: int, e: int, f: int, g: int, h: int,
    i: i8, j: i32, k: u8) -> int
{
    return c + j as int + k as int;
}
```

```text
arm64.imms:
b0:
    and x9, x0, #65280
    orr x9, x9, #6148914691236517205
    mov x10, #4660
    eor x9, x9, x10
    add x9, x9, #3, lsl #12
    cmp x9, #7, lsl #12
    b.ge b2
b1:
    sub x0, x9, #2, lsl #12
    ret
b2:
    mov x0, x9
    ret
```

The mask 0xff00 is one run of eight ones in a 64-bit element, and 0x5555555555555555 repeats the 2-bit element `01`. The constant 0x1234 has four runs of ones in its period of 64 bits, so it goes through `x10`. The constants 0x3000, 0x7000 and 0x2000 have zero low 12 bits and take the shifted form. The test `dump_alloc_arm64.macos-arm64` pins the listing.

## Memory operands

A load or store adds one of three offsets to its base register[^1]. The first is an unsigned 12-bit offset, scaled by the size of the value. The second is a signed 9-bit offset, which llvm-mc encodes as `ldur` or `stur`. The third is an index register, shifted left by 0 or by the scale of the size. The function `fold_address` of chapter 14 finds the address, and `fits_address` checks the ARM64 forms.

```c
/* A load or a store adds an unsigned 12-bit offset scaled by the size, a
   signed 9-bit offset or an index to the base. The index shifts by 0 or
   by the scale of the size. A store with an index takes only zero from
   wzr or xzr, so that it reads two registers. */
static bool fits_address(const struct selector *s, const struct address *a,
                         const struct ir_inst *use)
{
    enum ir_type type = use->op == IR_STORE ? use->a.type : use->type;
    uint8_t scale = type == IR_I8    ? 0
                    : type == IR_I16 ? 1
                    : type == IR_I32 ? 2
                                     : 3;
    int64_t size = (int64_t)1 << scale;

    (void)s;
    if (a->index == NULL) {
        return (a->offset >= 0 && a->offset % size == 0 &&
                a->offset / size <= 4095) ||
               (a->offset >= -256 && a->offset <= 255);
    }
    if (a->offset != 0 || (a->shift != 0 && a->shift != scale)) {
        return false;
    }
    return use->op == IR_LOAD ||
           (use->a.kind == IR_INT && use->a.as.integer == 0);
}
```

A store with an index reads the base, the index and the stored register. Chapter 13 has two scratch registers for spilled values, so such a store folds only for the value zero. A store of zero reads `wzr` or `xzr`. Register number 31 in that position reads as zero in the register accessor of the Arm pseudocode[^1].

```c
/* A store of zero reads the zero register, wzr or xzr. */
static void emit_store(struct selector *s, const struct ir_inst *inst)
{
    struct mach_operand value =
        inst->a.kind == IR_INT && inst->a.as.integer == 0
            ? mach_imm(0)
            : select_reg(s, &inst->a);

    emit2(s, A64_STR, value, address_of(s, &inst->b, inst->a.type));
}
```

The sum over an `i32` array of chapter 14 becomes one load with a shifted index. The test `dump_alloc_sum.arm64` pins the listing on macos-arm64.

```text
sum.sum:
b0:
    mov x9, #0
    mov x10, #0
b1:
    cmp x10, x1
    b.ge b3
b2:
    ldr w11, [x0, x10, lsl #2]
    sxtw x11, w11
    add x9, x9, x11
    add x10, x10, #1
    b b1
b3:
    mov x0, x9
    ret
sum.fill:
b0:
    mov x9, #0
b1:
    cmp x9, x1
    b.ge b3
b2:
    lsl x10, x9, #1
    add x10, x0, x10
    mov w11, #7
    strh w11, [x10]
    add x9, x9, #1
    b b1
b3:
    ret
```

The load `ldr w11, [x0, x10, lsl #2]` replaces the shift of `i`, the `ptradd` and the `load`. The store of 7 in `fill` does not fold, because the value needs the register `w11`. It stays `lsl`, `add` and `strh`.

## Page addresses

A 64-bit address has no immediate form. The IR operation `addr` becomes two instructions. The instruction `adrp` adds a signed offset in 4 KB pages to the page of the program counter[^1]. The instruction `add` then adds the low 12 bits of the symbol.

```c
/* The address of a function or a global. adrp writes the address of the
   4 KB page that holds the symbol, relative to the pc, and add adds the
   low 12 bits of the symbol. A C function on Linux and macOS loads its
   address from the GOT entry in that page instead. */
static void emit_addr(struct selector *s, const struct ir_inst *inst)
{
    struct mach_operand r = select_result(s, inst);
    struct mach_operand symbol = mach_imm(inst->a.as.index);

    symbol.kind = inst->a.kind == IR_FUNC ? MACH_FUNC : MACH_GLOBAL;
    symbol.got = select_uses_got(s, &inst->a);
    emit2(s, A64_ADRP, r, symbol);
    emit3(s, symbol.got ? A64_LDRGOT : A64_ADD, r, r, symbol);
}
```

The branch with `got` loads the address of a C function from its entry in the global offset table. Chapter 20 adds it for function pointers to C functions on Linux and macOS.

The unit test `page_address` in `tests/unit/test_arm64.c` checks `adrp x0, main.helper` followed by `add x0, x0, :lo12:main.helper`. That spelling of the low 12 bits is the ELF and COFF form. For the triple `arm64-apple-macos`, llvm-mc 23.1.1 rejects it with `ADR/ADRP relocations must be GOT relative`. It accepts `_main.helper@PAGE` and `_main.helper@PAGEOFF`. Chapter 16 writes that form for Mach-O, and the dumps print the ELF form for all three targets.

## Stack arguments and parameters

AAPCS64 passes the first eight integer arguments in `x0` to `x7`[^3]. Every further argument goes to the stack area at `sp`. Its address is rounded up to a multiple of 8, and an argument of fewer than 8 bytes takes 8 bytes[^3]. Windows ARM64 applies the same rules to integer arguments, including those of variadic functions[^2].

Apple diverges in three rules[^4]. A stack argument takes its own size, such as one byte at `sp` and the next at `sp+1`. Every variadic argument goes to the stack in 8-byte slots. The caller sign- or zero-extends every argument of fewer than 32 bits, which AAPCS64 leaves to the callee.

```c
/* DESIGN: AAPCS64 counts integer and float arguments separately, and an
   argument after its eight registers goes to the stack in 8 bytes. Apple
   gives a named stack argument its own size at its own alignment and
   passes every variadic argument on the stack in 8 bytes. Windows passes
   a variadic float in an integer register. A homogeneous float aggregate
   takes float registers and any other aggregate of up to 16 bytes integer
   registers, all or none. A larger aggregate is a pointer to a copy. On
   the stack, Apple aligns a float aggregate to its members, and every
   other aggregate takes 8-byte slots. */
static void locate(const struct selector *s, const struct ir_function *callee,
                   const enum ir_type *types, size_t count,
                   struct arg_location *out)
{
    size_t ints = 0;
    size_t floats = 0;
    int64_t next = 0;
    int64_t align;
    size_t i;

    for (i = 0; i < count; i++) {
        bool variadic = i >= callee->param_count;
        bool fp = select_is_float(types[i]) &&
                  !(s->abi == &windows && variadic);
        int64_t size = s->abi == &apple && !variadic ? bits(types[i]) / 8 : 8;
        memset(&out[i], 0, sizeof out[i]);
        out[i].copy = -1;
        if (types[i] == IR_AGG) {
            const struct layout *agg = select_layout(s, callee->params[i].agg);
            size_t hfa = hfa_members(agg);
            size_t parts = (size_t)(agg->size + 7) / 8;
            size = 8;
            if (hfa > 0 && floats + hfa <= s->abi->fp_arg_count) {
                aggregate_parts(agg, s->abi->fp_args[floats], &out[i]);
                floats += hfa;
                continue;
            }
            /* An aggregate aligned to 16 starts at an even register,
               except on Apple. */
            if (hfa == 0 && agg->size <= 16 && agg->align == 16 &&
                s->abi != &apple && ints % 2 == 1 &&
                ints + 1 + parts <= s->abi->int_arg_count) {
                ints++;
            }
            if (hfa == 0 && agg->size <= 16 &&
                ints + parts <= s->abi->int_arg_count) {
                aggregate_parts(agg, s->abi->int_args[ints], &out[i]);
                ints += parts;
                continue;
            }
            if (hfa == 0 && agg->size > 16 && ints < s->abi->int_arg_count) {
                out[i].indirect = true;
                out[i].reg = s->abi->int_args[ints++];
                continue;
            }
            if (hfa > 0) {
                floats = s->abi->fp_arg_count;
            } else if (agg->size <= 16) {
                ints = s->abi->int_arg_count;
            }
            out[i].stack = true;
            out[i].indirect = agg->size > 16 && hfa == 0;
            align = 8;
            if (!out[i].indirect) {
                out[i].size = agg->size;
                size = (int64_t)(agg->size + 7) / 8 * 8;
            }
            if (hfa > 0 && s->abi == &apple) {
                align = (int64_t)agg->align;
                size = (int64_t)agg->size;
            } else if (agg->align == 16 && !out[i].indirect) {
                align = 16;
            }
            out[i].offset = (next + align - 1) / align * align;
            next = out[i].offset + size;
            continue;
        }
        if (s->abi == &apple && variadic) {
            out[i].stack = true;
        } else if (fp && floats < s->abi->fp_arg_count) {
            out[i].reg = s->abi->fp_args[floats++];
        } else if (!fp && ints < s->abi->int_arg_count) {
            out[i].reg = s->abi->int_args[ints++];
        } else {
            out[i].stack = true;
        }
        if (out[i].stack) {
            out[i].offset = (next + size - 1) / size * size;
            next = out[i].offset + size;
        }
    }
}
```

The function `locate` gives each argument a register or an offset in the stack area. Its last branch holds the rules of this chapter for integer arguments: the eight registers, the 8-byte slots, and Apple's own sizes and variadic stack arguments. Chapter 17 adds the float registers, and chapter 18 the branch for aggregates.

The function `emit_call` writes the stack arguments first and loads the register arguments last. The fixed ranges of the argument registers then start just before `bl`.

```c
/* Stack arguments go to the area at sp, then the register arguments into
   their registers. Apple makes the caller extend a register argument of 8
   or 16 bits. bl overwrites every caller-saved register. */
static void emit_call(struct selector *s, const struct ir_inst *inst)
{
    const struct ir_function *callee = select_callee(s, inst);
    bool indirect = inst->b.kind == IR_FUNC;
    struct mach_operand target = indirect ? select_reg(s, &inst->a)
                                          : mach_imm(0);
    enum ir_type *types = calloc(inst->arg_count + 1, sizeof *types);
    struct arg_location *locations =
        calloc(inst->arg_count + 1, sizeof *locations);
    struct mach_operand *copies = calloc(inst->arg_count + 1, sizeof *copies);
    struct arg_location result;
    struct mach_operand result_address;
    struct mach_operand f = mach_imm(inst->a.as.index);
    struct mach_inst *call;
    uint64_t uses = 0;
    int64_t next = 0;
    size_t i;

    memset(&result_address, 0, sizeof result_address);
    if (types == NULL || locations == NULL || copies == NULL) {
        fputs("antic: out of memory\n", stderr);
        exit(70);
    }
    for (i = 0; i < inst->arg_count; i++) {
        types[i] = i < callee->param_count ? callee->params[i].type
                                           : inst->args[i].type;
    }
    locate(s, callee, types, inst->arg_count, locations);
    locate_result(s, callee, &result);
    if (callee->result == IR_AGG && result.indirect) {
        result_address = select_result_slot(s, inst, select_layout(s, callee->result_agg));
    }
    for (i = 0; i < inst->arg_count; i++) {
        const struct ir_operand *arg = &inst->args[i];
        struct mach_operand slot = memory(mach_preg(SP, 64), arg->type);
        if (types[i] == IR_AGG && (locations[i].indirect ||
                                   locations[i].stack)) {
            copies[i] = copy_argument(s, &locations[i],
                                      select_layout(s, callee->params[i].agg),
                                      select_reg(s, arg));
        }
        if (types[i] == IR_AGG && locations[i].stack) {
            int64_t end = locations[i].offset +
                          (locations[i].indirect
                               ? 8
                               : (int64_t)(select_layout(s, callee->params[i].agg)->size + 7) /
                                     8 * 8);
            if (end > next) {
                next = end;
            }
        }
        if (types[i] == IR_AGG || !locations[i].stack) {
            continue;
        }
        slot.value = locations[i].offset;
        if (i >= callee->param_count) {
            slot.width = select_is_float(arg->type) ? width(arg->type) : 64;
        }
        emit2(s, A64_STR,
              arg->kind == IR_INT && arg->as.integer == 0
                  ? mach_imm(0)
                  : select_reg(s, arg),
              slot);
        if (slot.value + slot.width / 8 > next) {
            next = slot.value + slot.width / 8;
        }
    }
    next = (next + 7) / 8 * 8;
    if ((uint64_t)next > s->out->outgoing) {
        s->out->outgoing = (uint64_t)next;
    }
    for (i = 0; i < inst->arg_count; i++) {
        const struct ir_operand *arg = &inst->args[i];
        enum ir_ext ext = IR_EXT_NONE;
        if (locations[i].stack) {
            continue;
        }
        if (types[i] == IR_AGG && locations[i].indirect) {
            move(s, mach_preg(locations[i].reg, 64), copies[i]);
            uses |= (uint64_t)1 << locations[i].reg;
            continue;
        }
        if (types[i] == IR_AGG) {
            uses |= select_load_parts(s, &locations[i], select_reg(s, arg));
            continue;
        }
        if (s->abi == &apple && i < callee->param_count) {
            ext = callee->params[i].ext;
        }
        load_value(s, mach_preg(locations[i].reg, width(arg->type)), arg, ext);
        uses |= (uint64_t)1 << locations[i].reg;
    }
    if (callee->result == IR_AGG && result.indirect) {
        move(s, mach_preg(result.reg, 64), result_address);
        uses |= (uint64_t)1 << result.reg;
    }
    free(types);
    free(locations);
    free(copies);
    f.kind = MACH_FUNC;
    call = select_emit(s, indirect ? A64_BLR : A64_BL, 1,
                       indirect ? &target : &f);
    call->uses = uses;
    call->defs = s->abi->caller_saved;
    if (inst->result != IR_NO_RESULT && callee->result == IR_AGG) {
        if (!result.indirect) {
            select_store_parts(s, &result,
                               select_result_slot(s, inst,
                                                  select_layout(s, callee->result_agg)));
        }
    } else if (inst->result != IR_NO_RESULT && select_is_float(inst->type)) {
        emit2(s, A64_FMOV, select_result(s, inst),
              mach_preg(s->abi->fp_result, width(inst->type)));
    } else if (inst->result != IR_NO_RESULT) {
        move(s, select_result(s, inst),
             mach_preg(s->abi->int_result, width(inst->type)));
    }
}
```

The function serves every argument of the book. Chapter 17 adds float arguments, chapter 18 aggregates in registers, in memory and through an address, and chapter 20 the call through a register. For the integer arguments of this chapter the first loop writes the stack arguments. The second loop loads the register arguments, with the extension that Apple requires.

### Parameter extensions in the IR

The IR keeps signedness in its operations, and a call has no operation that says whether an `i8` argument is signed. Lowering records it in the signature. The function `param_ext` gives a parameter of 8 or 16 bits `signext` or `zeroext` from its Anti type. The function `add_param` stores that extension with the parameter. The aggregate type that `add_param` also records is the topic of chapter 18.

```c
static enum ir_ext param_ext(const struct type *t)
{
    enum ir_type type = ir_type_of(t);

    if (type != IR_I8 && type != IR_I16) {
        return IR_EXT_NONE;
    }
    return type_is_signed(t) ? IR_EXT_SIGN : IR_EXT_ZERO;
}

/* A parameter of 8 or 16 bits records whether it is signed, and an
   aggregate its layout. */
static void add_param(struct lowerer *l, struct ir_function *f,
                      const struct type *t)
{
    enum ir_type type = ir_type_of(t);

    ir_param_add(f, type, result_agg(l, t));
    f->params[f->param_count - 1].ext = param_ext(t);
}
```

The test `dump_opt_arm64` prints the IR of `tests/dump/arm64.anti`, with the extensions of `widen`, `bump`, `pass` and `last`.

```text
extern fn widen(i8 zeroext, i8 signext, i16 zeroext) -> i32
fn arm64.imms(%0: i64) -> i64 {
b0:
    %1 = and i64 %0, 65280
    %2 = or i64 %1, 6148914691236517205
    %3 = xor i64 %2, 4660
    %4 = add i64 %3, 12288
    %5 = slt i8 %4, 28672
    branch %5, b1, b2
b1:
    %6 = sub i64 %4, 8192
    ret i64 %6
b2:
    ret i64 %4
}
fn arm64.bump(%0: i8 zeroext) -> i8 {
b0:
    %1 = add i8 %0, 1
    ret i8 %1
}
fn arm64.pass(%0: i8 zeroext, %1: i8 signext) -> i32 {
b0:
    %2 = call i8 @arm64.bump(%0)
    %3 = call i32 @widen(%2, %1, 7)
    ret i32 %3
}
fn arm64.last(%0: i8 zeroext, %1: i16 signext, %2: i64, %3: i64, %4: i64, %5: i64, %6: i64, %7: i64, %8: i8 signext, %9: i32, %10: i8 zeroext) -> i64 {
b0:
    %11 = sext i64 %9
    %12 = add i64 %2, %11
    %13 = zext i64 %10
    %14 = add i64 %12, %13
    ret i64 %14
}
```

A library file of chapter 9 stores the extension of every parameter in the byte after its type. The reader refuses a file in which a parameter of 8 or 16 bits has no extension, or a wider parameter has one.

### Calls on macOS and Linux

The function `pass` calls `bump` and `widen`. On macos-arm64 the caller extends each narrow argument.

```text
arm64.pass:
b0:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    sub sp, sp, #16
    str x19, [sp, #8]
    mov w19, w1
    uxtb w0, w0
    bl arm64.bump
    uxtb w0, w0
    sxtb w1, w19
    mov w2, #7
    bl widen
    ldr x19, [sp, #8]
    mov sp, x29
    ldp x29, x30, [sp], #16
    ret
```

Before `bl arm64.bump`, `uxtb w0, w0` extends `x`. The result of `bump`, whose upper bits are unknown, passes through `uxtb w0, w0` again before `widen`, and `y` through `sxtb w1, w19`. On linux-arm64 the same function has no extensions.

```text
arm64.pass:
b0:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    sub sp, sp, #16
    str x19, [sp, #8]
    mov w19, w1
    bl arm64.bump
    mov w1, w19
    mov w2, #7
    bl widen
    ldr x19, [sp, #8]
    mov sp, x29
    ldp x29, x30, [sp], #16
    ret
```

A parameter beyond `x7` lies in the argument area of the caller. The prologue stores the frame record at the new `sp` and points `x29` at it, so the caller's `sp` is `x29 + 16`.

```c
/* A parameter beyond x7 lies in the argument area of the caller, which
   starts 16 bytes above the frame record that x29 points to. */
static void stack_param(struct selector *s, int64_t offset,
                        struct mach_operand dst)
{
    const struct ir_function *f = s->f;
    struct mach_operand m = mach_preg(X29, 64);
    size_t i;

    m.kind = MACH_MEM;
    m.value = 16 + offset;
    m.width = dst.width;
    for (i = 0; i < f->param_count; i++) {
        if (f->params[i].temp == dst.reg) {
            m = memory(mach_preg(X29, 64), f->params[i].type);
            m.value = 16 + offset;
        }
    }
    s->out->stack_params = true;
    emit2(s, A64_LDR, dst, m);
}
```

The offset of each stack parameter comes from `locate`, the function that also places the arguments of a call. The loop over the parameters finds the IR type of the parameter that `dst` receives, so the load reads its own width.

The function `last` reads `j`, a 32-bit parameter, and `k`, an 8-bit one. On macos-arm64 `i` takes one byte at offset 0, `j` four bytes at offset 4 and `k` one byte at offset 8, so `k` is `[x29, #24]`.

```text
arm64.last:
b0:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    ldr w9, [x29, #20]
    ldrb w10, [x29, #24]
    sxtw x9, w9
    add x9, x2, x9
    uxtb w10, w10
    add x0, x9, x10
    ldp x29, x30, [sp], #16
    ret
```

On linux-arm64 every stack parameter takes 8 bytes, and the same loads read `[x29, #24]` and `[x29, #32]`. The program `args.anti` of chapter 14 calls the variadic `printf`. Its listing on macos-arm64 stores the variadic argument with `str x0, [sp]`, and the listing on linux-arm64 moves it into `x1`. The tests `dump_alloc_args.macos-arm64` and `dump_alloc_args.linux-arm64` pin both.

## Frame sizes and far offsets

The largest immediate of `sub sp, sp, #size` is 4095, and chapter 13 stopped there. The prologue of this chapter subtracts a size that fits `fits_imm12`, shifted or not, and loads any other size into `x16` first. On windows-arm64 a frame of 4096 bytes or more calls `__chkstk` before it moves `sp`. The helper takes "the total stack allocation divided by 16 in x15"[^2].

```c
/* Move sp down by size bytes of the frame, through x16 when no 12-bit
   immediate holds it. Windows probes a frame of 4096 bytes or more with
   __chkstk first, which takes the size divided by 16 in x15. */
static void allocate_frame(struct mach_block *b, const struct frame *frame,
                           uint64_t bytes)
{
    struct mach_operand ops[4];
    int64_t size = (int64_t)bytes;

    if (frame->probe) {
        load_into(b, mach_preg(X15, 64), bytes / 16);
        memset(ops, 0, sizeof ops);
        ops[0].kind = MACH_NAME;
        ops[0].name = "__chkstk";
        append(b, A64_BL, 1, ops);
    }
    ops[0] = mach_preg(SP, 64);
    ops[1] = mach_preg(SP, 64);
    if (fits_imm12(size)) {
        append_imm12(b, A64_SUB, 2, ops, size);
        return;
    }
    load_into(b, mach_preg(X16, 64), bytes);
    ops[2] = mach_preg(X16, 64);
    append(b, A64_SUB, 3, ops);
}
```

The parameter `bytes` is the whole frame, or on Windows the part below the save area, which chapter 16 describes. The register `x16` is a scratch register of chapter 13, which holds a value only within one instruction, so the prologue may use it. A load or store of 64 bits takes an offset from `sp` that is a multiple of 8 up to 32760, the scaled 12-bit form. A spill slot or a saved register beyond that offset goes through a register.

```c
/* ldr and str of 64 bits take an offset from sp that is a multiple of 8
   up to 32760. A larger offset goes into a register first: ldr uses the
   register it loads, and str the scratch register that it does not
   store. */
static bool fits_scaled(int64_t offset)
{
    return offset >= 0 && offset % 8 == 0 && offset / 8 <= 4095;
}

/* A float register cannot hold the offset, so its load goes through x16.
   A store names its value before its address, so the allocator loads a
   spilled float value before a spilled integer address into x16. */
static void load_spill(struct mach_block *b, uint8_t reg, int64_t offset)
{
    uint8_t index = reg >= V0 ? X16 : reg;
    struct mach_operand ops[2];

    ops[0] = mach_preg(reg, 64);
    ops[1] = stack(offset, INDEX_NONE);
    if (!fits_scaled(offset)) {
        load_into(b, mach_preg(index, 64), (uint64_t)offset);
        ops[1] = stack_indexed(index);
    }
    append(b, A64_LDR, 2, ops);
}

static void store_spill(struct mach_block *b, uint8_t reg, int64_t offset)
{
    uint8_t other = reg == X16 ? X17 : X16;
    struct mach_operand ops[2];

    ops[0] = mach_preg(reg, 64);
    ops[1] = stack(offset, INDEX_NONE);
    if (!fits_scaled(offset)) {
        load_into(b, mach_preg(other, 64), (uint64_t)offset);
        ops[1] = stack_indexed(other);
    }
    append(b, A64_STR, 2, ops);
}
```

A float register of chapter 17 cannot hold an offset, so `load_spill` puts the offset of a far float slot into `x16`.

A load uses the register it loads for the offset. A store uses the scratch register it does not store. That register held at most an operand of the instruction before the store, which has finished.

The address of a slot is `add t, sp, #offset`, and frame layout knows the offset only after register allocation. The target hook `expand` receives each instruction after allocation and splits a slot address whose offset exceeds both immediate forms. The x86_64 back end sets the hook to `NULL`, because its displacements hold 32 bits.

```c
/* The address of a slot is add dst, sp, #offset. An offset that no 12-bit
   immediate holds, shifted or not, goes into dst first. */
static void expand(struct mach_block *b, const struct mach_inst *inst)
{
    struct mach_operand ops[4];
    int64_t offset;

    if (inst->op != A64_ADD || inst->count != 3 ||
        inst->operands[1].kind != MACH_PREG || inst->operands[1].reg != SP ||
        inst->operands[2].kind != MACH_IMM || inst->operands[2].value <= 4095) {
        *mach_append(b) = *inst;
        return;
    }
    offset = inst->operands[2].value;
    ops[0] = inst->operands[0];
    ops[1] = inst->operands[1];
    if (fits_imm12(offset)) {
        append_imm12(b, A64_ADD, 2, ops, offset);
        return;
    }
    load_into(b, ops[0], (uint64_t)offset);
    ops[2] = ops[0];
    append(b, A64_ADD, 3, ops);
}
```

The unit test `large_frames` in `tests/unit/test_arm64.c` checks the prologue of frames of 8192, 70000 and 65536 bytes and the spill slots beyond 32760 bytes. The unit test `far_slots` compiles a function with 520 address-taken locals on linux-arm64. Its frame of 8320 bytes starts with `mov x16, #8320` and `sub sp, sp, x16`. The slot at offset 4096 becomes `add x16, sp, #1, lsl #12`, and the slot at 4104 becomes `mov x16, #4104` and `add x16, sp, x16`. The unit test `probe` checks the call of `__chkstk` on windows-arm64 with `mov x15, #512` for 8192 bytes.

## Assembling the output

The tests `asm_<program>_<target>` extend those of chapter 14 to all six targets. They assemble nine programs with llvm-mc 23.1.1 for the triples `x86_64-unknown-linux-gnu`, `x86_64-apple-macos`, `x86_64-pc-windows-msvc`, `aarch64-unknown-linux-gnu`, `arm64-apple-macos` and `aarch64-pc-windows-msvc`. The programs are `main`, `loop`, `twice`, `cell`, `spill7`, `sum`, `args`, `x86` and `arm64`, and all of them assemble. Chapter 16 adds the directives, the symbol names of each object format and the command lines of llvm-mc and the linker.

### Assertions

An assertion reaches the back end as an ordinary branch, so no pattern of this chapter is about it. The comparison, the conditional jump and the call are the ones already selected for `if` and for a call of a C function.

What the back end does decide is whether the branch is there at all. A release build has already cut it, and this chapter sees a function without it.

### Atomic operations

An atomic field reaches the back end as a call of the runtime, so no pattern of this chapter is about it either. The reason to leave them there is this target. The baseline of ARMv8.0 has no atomic read-modify-write instruction, so each operation is a loop of `ldaxr` and `stlxr` that repeats until the store succeeds. The selector emits into the blocks that lowering built and creates none of its own, so it cannot write that loop.

A release build of `rt/atomic.c` holds `ldaddal`, `casal`, `swpal`, `ldclral` and `ldsetal` when the compiler targets an ARM64 with the large system extensions. On a baseline ARM64 it holds the load-store-exclusive loop instead. A `load` is `ldar` and a `store` is `stlr` in both.

Inlining them needs a selector that can add blocks, which the closing guide lists among the optimisations that follow the book.

### Thunks

The table of an interface holds thunks, which chapter 8 builds. A thunk is an ordinary IR function. It subtracts a symbolic offset from its first parameter and calls another function, so no pattern of this chapter is about it either. The subtraction is the `sub` that any pointer arithmetic selects, and the call is an ordinary `bl`.

## Performance

This back end applies five rules that the instruction set makes free. Arm's optimization guides hold many more, and a compiler that competes on speed applies most of them.

Values of 8, 16 and 32 bits live in the `w` registers. A zero extension from 32 bits to 64 bits is therefore a move of the `w` register. Every write to a `w` register clears the upper 32 bits, so nothing else is needed. The instructions `uxtb` and `uxth` stay for the narrower widths, where the upper bits are unknown.

A remainder is `sdiv` or `udiv` followed by `msub`. The multiply and the subtract that reconstruct the remainder are one instruction, and the quotient stays in the register the division wrote.

The address of a symbol is `adrp` and the `add` that follows it, emitted as a pair. The relocation of the page and the relocation of the offset name the same symbol, and chapter 16 gives their syntax per format.

The prologue writes the frame record with one `stp`, and the epilogue reads it back with one `ldp`. Two 64-bit accesses become one.

A logical operation whose immediate fits the bitmask encoding keeps the immediate. The function `is_logical_imm` decides it, and a value it rejects is loaded into a register first.

Four rules are not here. The first is `madd`, the fused multiply-add. Its counterpart `msub` is above, because a remainder produces the pattern directly. A multiply-add needs the selector of chapter 12 to match a multiply that feeds an add, which it does not do. The second is `csel`, which replaces a short forward branch with a conditional select and leaves the pipeline alone. The third is pairing adjacent spill slots into `ldp` and `stp`, which the prologue already does for the frame record. The fourth is a division by a constant as a multiply by a reciprocal, which holds on both architectures.

Two sources cover the rest. Arm publishes a software optimization guide per core, with the latency and the throughput of every instruction on that core[^5]. Apple publishes the *Apple Silicon CPU Optimization Guide*. It is named here and not quoted, because its licence decides what may be reproduced[^6].

## Tests

Comparisons of narrow values, division, shifts, immediates, addressing modes, `adrp`, the calls of three conventions, large frames and the Windows probe are pinned. The build has zero warnings, and every `ctest` test passes.

## Next

[Chapter 16, Assembly emission per operating system]({{% relref "/programming/writing-a-compiler/16-assembly-emission" %}}), gives the directives and sections, the leading `_` on Mach-O symbols, and `rip`-relative and `:lo12:` versus `@PAGEOFF` relocations. It adds the COFF specifics, global and hidden symbols for dev mode and export, constructor sections and the `anti_licenses` notice. It closes with the llvm-mc and lld command lines of the driver for each target, with the platform linker as a fallback.

## References

[^1]: Arm Limited, *Arm A-profile A64 Instruction Set Architecture*, release 2025-12, XML files `sdiv.xml`, `msub.xml`, `lslv.xml`, `add_addsub_imm.xml`, `add_addsub_ext.xml`, `cmp_subs_addsub_imm.xml`, `cmn_adds_addsub_imm.xml`, `and_log_imm.xml`, `ldr_imm_gen.xml`, `ldr_reg_gen.xml`, `ldur_gen.xml`, `adrp.xml` and `shared_pseudocode.xml`, https://developer.arm.com/-/cdn-downloads/permalink/Exploration-Tools-A64-ISA/ISA_A64/ISA_A64_xml_A_profile-2025-12.tar.gz

[^2]: Microsoft, *Overview of ARM64 ABI conventions*, sections "Integer registers", "Parameter passing", "Addendum: Variadic functions" and "Stack", https://learn.microsoft.com/en-us/cpp/build/arm64-windows-abi-conventions?view=msvc-170

[^3]: Arm Limited, *Procedure Call Standard for the Arm 64-bit Architecture*, release 2025Q4, section 6.8.2, https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst

[^4]: Apple, *Writing ARM64 code for Apple platforms*, sections on arguments and variadic functions, https://developer.apple.com/documentation/xcode/writing-arm64-code-for-apple-platforms

[^5]: Arm Limited, *Arm Neoverse V2 Software Optimization Guide*, document 109898, https://developer.arm.com/documentation/109898/latest/

[^6]: Apple, *Apple Silicon CPU Optimization Guide*, https://developer.apple.com/documentation/apple-silicon/cpu-optimization-guide
