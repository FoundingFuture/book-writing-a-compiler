---
title: "Floating point"
description: "How antic compiles f32 and f64 values: a second register class, SSE and ARM64 float instructions, conversions, NaN comparisons and float arguments."
summary: "`float` as a second register class. SSE and NEON registers, conversions, comparisons, the ABI rules for passing floats. The changes in selection and allocation."
date: 2026-09-13T22:32:00+02:00
lastmod: 2026-09-15T03:58:33+02:00
draft: false
weight: 170
tags: [compilers, assembly]
keywords: [floating point, SSE, xmm registers, register class, NaN comparison, float to integer conversion, float arguments, callee-saved float registers]
---

## Previously

[Chapter 16, Assembly emission per operating system]({{% relref "/programming/writing-a-compiler/16-assembly-emission" %}}), writes assembly files for ELF, Mach-O and COFF and links executables. It covers directives, symbol names, relocations and COFF specifics. Dev mode and export add global and hidden symbols, and shared libraries add constructor sections. Every linked binary holds the `anti_licenses` notice, and the driver runs llvm-mc and lld for each target.

## A second register class

Chapters 12 to 16 stopped at the types `f32` and `f64` with a message that named this chapter. A float lives in a register of its own class: `xmm0` to `xmm15` on x86_64, and `v0` to `v31` on ARM64, written `dn` for 64 bits and `sn` for 32 bits. The IR of chapter 7 already has the float operations `fadd` to `fdiv`, `fneg`, the comparisons `feq` to `fge` and six conversions. This chapter selects them on both processors.

The machine code numbers the float registers after the integer registers. On x86_64 `xmm0` is 16, and on ARM64 `v0` is 32. Every virtual register records its class when selection creates it.

```c
uint32_t mach_vreg_add(struct mach_function *f, bool fp)
{
    if (f->vreg_count >= f->fp_capacity) {
        size_t capacity = f->fp_capacity == 0 ? 64 : f->fp_capacity * 2;
        bool *classes;
        while (capacity <= f->vreg_count) {
            capacity *= 2;
        }
        classes = realloc(f->fp, capacity * sizeof *classes);
        if (classes == NULL) {
            fputs("antic: out of memory\n", stderr);
            exit(70);
        }
        f->fp = classes;
        f->fp_capacity = capacity;
    }
    f->fp[f->vreg_count] = fp;
    return f->vreg_count++;
}
```

The allocator of chapter 13 takes the registers of an interval from the list of its class. An interval never takes a register of the other class, and a spill victim must share the class of the new interval.

```c
/* The registers that the allocator hands out for the class of iv, in the
   order of preference. */
static const uint8_t *class_registers(const struct alloc *a,
                                      const struct interval *iv, size_t *count)
{
    *count = iv->fp ? a->abi->fp_allocatable_count
                    : a->abi->allocatable_count;
    return iv->fp ? a->abi->fp_allocatable : a->abi->allocatable;
}
```

## ABI tables for floats

Each convention lists argument, result and callee-saved xmm or v registers. The compiler antic adds two scratch registers per class for spilled values.

| Convention | Arguments | Result | Callee-saved | Scratch |
|---|---|---|---|---|
| System V | `xmm0` to `xmm7` | `xmm0` | none | `xmm14`, `xmm15` |
| Windows x64 | `xmm0` to `xmm3` by position | `xmm0` | `xmm6` to `xmm15` | `xmm4`, `xmm5` |
| AAPCS64, Apple, Windows ARM64 | `v0` to `v7` | `v0` | low 64 bits of `v8` to `v15` | `v16`, `v17` |

System V preserves none of `xmm0` to `xmm15`[^1]. Windows x64 treats `xmm0` to `xmm5` as volatile and `xmm6` to `xmm15` as nonvolatile, which "must be saved and restored by a function that uses them"[^2]. AAPCS64 requires a callee to preserve only the bottom 64 bits of `v8` to `v15`[^3]. The Windows scratch registers are therefore `xmm4` and `xmm5`, the volatile registers that pass no argument.

```c
static const struct abi windows = {
    .int_args = windows_args,
    .int_arg_count = 4,
    .int_result = RAX,
    .caller_saved = BIT(RAX) | BIT(RCX) | BIT(RDX) | BIT(R8) | BIT(R9) |
                    BIT(R10) | BIT(R11) | (XMM_ALL & ~XMM_WINDOWS_CALLEE_SAVED),
    .callee_saved = BIT(RBX) | BIT(RSI) | BIT(RDI) | BIT(R12) | BIT(R13) |
                    BIT(R14) | BIT(R15) | XMM_WINDOWS_CALLEE_SAVED,
    .allocatable = windows_allocatable,
    .allocatable_count = sizeof windows_allocatable,
    .scratch = {R10, R11},
    .shadow_space = 32,
    .probe_stack = true,
    .fp_args = windows_fp_args,
    .fp_arg_count = 4,
    .fp_result = XMM0,
    .fp_allocatable = windows_fp_allocatable,
    .fp_allocatable_count = sizeof windows_fp_allocatable,
    .fp_scratch = {XMM4, XMM5},
    .fp_save_size = 16,
};
```

A Windows function saves a used `xmm6` to `xmm15` with all 128 bits, because a caller may keep a vector in it. The field `fp_save_size` gives 16 bytes, and frame layout places each saved register at its own offset. An ARM64 function saves `d8` to `d15` in 8 bytes.

```c
/* Windows preserves all 128 bits of xmm6 to xmm15, so a callee saves them
   with movups. */
static void save_register(struct mach_block *b, uint8_t reg, int64_t offset,
                          bool restore)
{
    struct mach_operand ops[2];

    if (reg < XMM0) {
        if (restore) {
            load_spill(b, reg, offset);
        } else {
            store_spill(b, reg, offset);
        }
        return;
    }
    ops[restore ? 1 : 0] = stack(offset);
    ops[restore ? 0 : 1] = mach_preg(reg, 64);
    append(b, X64_MOVUPS, 2, ops);
}
```

## Constants and arithmetic

A float constant has no immediate form on either processor. The compiler antic loads its bits into an integer register and moves them into the float register with `movq` or `movd` on x86_64 and `fmov` on ARM64[^4]. The assembly file needs no constant data.

```c
/* A float constant has no immediate form. Its bits go into an integer
   register and from there with movq or movd into the xmm register. */
static void load_float(struct selector *s, struct mach_operand dst,
                       enum ir_type type, double value)
{
    struct mach_operand bits = select_new_vreg(s, width(type));

    load(s, bits, float_bits(type, value));
    emit2(s, X64_MOVQX, dst, bits);
}
```

On ARM64 `fadd`, `fsub`, `fmul` and `fdiv` take three registers, and `fneg` negates. SSE instructions take two operands, and the first one is read and written, as for integers in chapter 14. The instructions `movsd` and `movss` move floats between registers and to and from memory[^5]. SSE has no negation, and `xorpd` flips the sign bit with a mask that arrives like a constant.

```c
/* SSE has no negation. xorpd flips the sign bit with a mask, which
   arrives through an integer register like a constant. */
static void emit_float_neg(struct selector *s, const struct ir_inst *inst)
{
    struct mach_operand r = select_result(s, inst);
    struct mach_operand bits = select_new_vreg(s, r.width);
    struct mach_operand mask = select_new_fp_vreg(s, r.width);

    load(s, bits, (uint64_t)1 << (r.width - 1));
    emit2(s, X64_MOVQX, mask, bits);
    if (inst->a.kind != IR_TEMP || inst->a.as.temp != inst->result) {
        move_float(s, r, select_reg(s, &inst->a));
    }
    emit2(s, X64_XORP, r, mask);
}
```

The test program `tests/dump/floats.anti` holds the float cases of this chapter.

```anti
fn keep(p: *f32, a: f64, b: f32) -> f64
{
    *p = *p * 0.5 - b;
    if a < 1.5 || a != a {
        return -a;
    }
    return a / (b as f64);
}

fn convert(a: i8, b: u64, c: f64, d: f32) -> u64
{
    let x = a as f32 + d;
    let y = b as f64 * c;
    return (x as f64 + y) as u64 + (c as i32) as u64 + (d as u32) as u64;
}
```

The test `dump_alloc_floats.macos-arm64` pins the listing on macos-arm64.

```text
floats.keep:
b0:
    fmov d18, d0
    ldr s19, [x0]
    movz w9, #16128, lsl #16
    fmov s20, w9
    fmul s19, s19, s20
    fsub s19, s19, s1
    str s19, [x0]
    movz x9, #16376, lsl #48
    fmov d19, x9
    fcmp d18, d19
    cset w9, mi
    cbz w9, b3
b1:
    fneg d0, d18
    ret
b2:
    fcvt d19, s1
    fdiv d0, d18, d19
    ret
b3:
    fcmp d18, d18
    cset w9, ne
    cbnz w9, b1
    b b2
floats.convert:
b0:
    sxtb w9, w0
    scvtf s18, w9
    fadd s18, s18, s1
    ucvtf d19, x1
    fmul d19, d19, d0
    fcvt d18, s18
    fadd d18, d18, d19
    fcvtzu x9, d18
    fcvtzs w10, d0
    sxtw x10, w10
    add x9, x9, x10
    fcvtzu w10, s1
    mov w10, w10
    add x0, x9, x10
    ret
```

## Comparisons and NaN

A NaN is unordered with every value. C11 states that for "a NaN and a numeric value, or for two NaNs, just the unordered relationship is true"[^6]. The compiler antic follows that rule: `!=` holds for a NaN, and `==`, `<`, `<=`, `>` and `>=` fail.

The instruction `ucomisd` sets ZF, PF and CF to 000 for greater, 001 for less, 100 for equal and 111 for unordered[^7]. The conditions `a` and `ae`, above and above or equal, therefore fail for NaN. A comparison `a < b` swaps the operands and tests `b` above `a`. Equality needs ZF set and PF clear, and inequality accepts ZF clear or PF set.

```c
/* ucomisd sets the flags of an unsigned comparison: CF for below, ZF for
   equal, and ZF, PF and CF together for NaN. a > b and a >= b are the
   conditions a and ae, which NaN fails, and a < b compares b with a.
   Equality needs PF clear as well, and inequality accepts PF set. */
static void emit_float_compare(struct selector *s, const struct ir_inst *inst)
{
    struct mach_operand r = select_result(s, inst);
    bool swap = inst->op == IR_FLT || inst->op == IR_FLE;
    struct mach_operand a = select_reg(s, swap ? &inst->b : &inst->a);
    struct mach_operand b = select_reg(s, swap ? &inst->a : &inst->b);
    struct mach_operand parity;

    emit2(s, X64_UCOMIS, a, b);
    switch (inst->op) {
    case IR_FGT:
    case IR_FLT:
        emit2(s, X64_SET, r, cond(COND_HI));
        break;
    case IR_FGE:
    case IR_FLE:
        emit2(s, X64_SET, r, cond(COND_HS));
        break;
    default:
        parity = select_new_vreg(s, 8);
        emit2(s, X64_SET, r, cond(inst->op == IR_FEQ ? COND_EQ : COND_NE));
        emit2(s, X64_SET, parity, cond(inst->op == IR_FEQ ? COND_NP : COND_P));
        emit2(s, inst->op == IR_FEQ ? X64_AND : X64_OR, r, parity);
        break;
    }
}
```

The Arm function `FPCompare` returns the flags NZCV as 0110 for equal, 1000 for less, 0010 for greater and 0011 for unordered[^4]. The condition `mi`, N set, holds only for less. `ls`, `gt` and `ge` fail for unordered, and `ne` holds, by the function `ConditionHolds`[^4].

```c
/* fcmp sets N for less, Z and C for equal, C for greater and C and V for
   NaN. mi, ls, gt and ge are false for NaN, and ne is true. */
static void emit_float_compare(struct selector *s, const struct ir_inst *inst)
{
    enum mach_cond c = inst->op == IR_FEQ   ? COND_EQ
                       : inst->op == IR_FNE ? COND_NE
                       : inst->op == IR_FLT ? COND_MI
                       : inst->op == IR_FLE ? COND_LS
                       : inst->op == IR_FGT ? COND_GT
                                            : COND_GE;

    emit2(s, A64_FCMP, select_reg(s, &inst->a), select_reg(s, &inst->b));
    emit2(s, A64_CSET, select_result(s, inst), cond(c));
}
```

A float comparison computes a `bool`, and the branch tests it with `cbz`, `cbnz` or `test`. The fusion of a comparison into the branch of chapter 12 covers integer comparisons only. In the listing above, `a < 1.5` becomes `fcmp d18, d19`, `cset w9, mi` and `cbz w9, b3`.

## Conversions

ARM64 converts with one instruction each: `scvtf` and `ucvtf` from a signed or unsigned integer, `fcvtzs` and `fcvtzu` toward zero, and `fcvt` between float widths[^4]. An integer of 8 or 16 bits extends to 32 bits first.

```c
/* scvtf and ucvtf convert a w or x register, fcvtzs and fcvtzu truncate
   toward zero, and fcvt changes the float width. An integer of 8 or 16
   bits extends to 32 bits first. */
static void emit_float_convert(struct selector *s, const struct ir_inst *inst)
{
    struct mach_operand r = select_result(s, inst);
    bool is_signed = inst->op == IR_SITOF || inst->op == IR_FTOSI;

    switch (inst->op) {
    case IR_SITOF:
    case IR_UITOF:
        emit2(s, is_signed ? A64_SCVTF : A64_UCVTF, r,
              extended(s, &inst->a, is_signed));
        break;
    case IR_FTOSI:
    case IR_FTOUI:
        emit2(s, is_signed ? A64_FCVTZS : A64_FCVTZU, r,
              select_reg(s, &inst->a));
        break;
    default:
        emit2(s, A64_FCVT, r, select_reg(s, &inst->a));
        break;
    }
}
```

SSE2 converts only signed integers. `cvtsi2sd` takes a signed 32-bit or 64-bit integer[^8], and `cvttsd2si` truncates toward zero to a signed integer[^9]. A `u32` converts as a 64-bit integer after `movl` clears its upper bits. A `u64` of 2^63 or more has no signed form, and antic computes the float without a branch.

```c
/* A u64 of 2^63 or more has no signed conversion. The value (a >> 1) |
   (a & 1) converts and doubles. The sign bit of a, spread into a mask,
   picks between that result and the plain signed conversion. */
static void unsigned_to_float(struct selector *s, struct mach_operand r,
                              struct mach_operand a)
{
    struct mach_operand half = select_new_vreg(s, 64);
    struct mach_operand low = select_new_vreg(s, 32);
    struct mach_operand large = select_new_fp_vreg(s, r.width);
    struct mach_operand small = select_new_fp_vreg(s, r.width);
    struct mach_operand mask = select_new_fp_vreg(s, r.width);
    struct mach_operand sign = select_new_vreg(s, 64);

    move(s, half, a);
    emit2(s, X64_SHR, half, mach_imm(1));
    move(s, low, widened(a, 32));
    emit2(s, X64_AND, low, mach_imm(1));
    emit2(s, X64_OR, half, widened(low, 64));
    emit2(s, X64_CVTSI2S, large, half);
    emit2(s, X64_ADDS, large, large);
    emit2(s, X64_CVTSI2S, small, a);
    move(s, sign, a);
    emit2(s, X64_SAR, sign, mach_imm(63));
    emit2(s, X64_MOVQX, mask, sign);
    emit2(s, X64_ANDP, large, mask);
    emit2(s, X64_ANDNP, mask, small);
    emit2(s, X64_ORP, large, mask);
    move_float(s, r, large);
}
```

The value `(a >> 1) | (a & 1)` is below 2^63, and twice its float is the float of `a`, rounded the same way, because the low bit keeps the rounding direction. The signed conversion of `a` is correct when `a` is below 2^63. `sarq $63` spreads the sign bit of `a` into a mask. `andpd`, `andnpd`[^10] and `orpd` then select one of the two floats.

A float of 2^63 or more has no signed 64-bit result either. `cvttsd2si` returns the indefinite integer `0x8000000000000000` when the result exceeds the signed range[^9]. The compiler antic subtracts 2^63 from the float, converts and flips the sign bit back with `xor`. The instruction `cmovae` picks that value when the float is not below 2^63.

```c
/* A float of 2^63 or more has no signed conversion to u64. It converts
   after subtracting 2^63, the sign bit of the result comes back with xor,
   and cmovae picks that value when the float is not below 2^63. */
static void float_to_unsigned(struct selector *s, struct mach_operand r,
                              struct mach_operand f)
{
    struct mach_operand bits = select_new_vreg(s, f.width);
    struct mach_operand limit = select_new_fp_vreg(s, f.width);
    struct mach_operand reduced = select_new_fp_vreg(s, f.width);
    struct mach_operand high = select_new_vreg(s, 64);
    struct mach_operand sign = select_new_vreg(s, 64);
    struct mach_operand ops[3];

    emit2(s, X64_CVTTS2SI, r, f);
    load(s, bits,
         float_bits(f.width == 64 ? IR_F64 : IR_F32, 9223372036854775808.0));
    emit2(s, X64_MOVQX, limit, bits);
    move_float(s, reduced, f);
    emit2(s, X64_SUBS, reduced, limit);
    emit2(s, X64_CVTTS2SI, high, reduced);
    load(s, sign, (uint64_t)1 << 63);
    emit2(s, X64_XOR, high, sign);
    emit2(s, X64_UCOMIS, f, limit);
    ops[0] = r;
    ops[1] = high;
    ops[2] = cond(COND_HS);
    select_emit(s, X64_CMOV, 3, ops);
}
```

The function `convert` of the test program on linux-x86_64 shows both sequences.

```text
floats.convert:
b0:
    movsbl %dil, %eax
    cvtsi2ssl %eax, %xmm2
    addss %xmm1, %xmm2
    movq %rsi, %rax
    shrq $1, %rax
    movl %esi, %ecx
    andl $1, %ecx
    orq %rcx, %rax
    cvtsi2sdq %rax, %xmm3
    addsd %xmm3, %xmm3
    cvtsi2sdq %rsi, %xmm4
    sarq $63, %rsi
    movq %rsi, %xmm5
    andpd %xmm5, %xmm3
    andnpd %xmm4, %xmm5
    orpd %xmm5, %xmm3
    mulsd %xmm0, %xmm3
    cvtss2sd %xmm2, %xmm2
    addsd %xmm3, %xmm2
    cvttsd2si %xmm2, %rax
    movabsq $4890909195324358656, %rcx
    movq %rcx, %xmm3
    movsd %xmm2, %xmm4
    subsd %xmm3, %xmm4
    cvttsd2si %xmm4, %rcx
    movabsq $-9223372036854775808, %rdx
    xorq %rdx, %rcx
    ucomisd %xmm3, %xmm2
    cmovaeq %rcx, %rax
    cvttsd2si %xmm0, %ecx
    movslq %ecx, %rcx
    addq %rcx, %rax
    cvttss2si %xmm1, %rcx
    movl %ecx, %ecx
    addq %rcx, %rax
    ret
```

## Floats in calls

System V counts integer and float arguments separately and passes floats in `xmm0` to `xmm7`[^1]. A variadic call loads the number of float registers in use into `al`, as chapter 11 describes. Windows x64 gives argument i the register at position i of its class, so a float second argument goes to `xmm1`[^2]. It passes a variadic float in the integer register of its position as well[^2].

The function `locate` in `src/x86_64.c` gives each argument a register or a stack slot. Its branches for scalar arguments follow the branches for aggregates of chapter 18. The index `position` is `i`, plus one on Windows when an aggregate result takes the first integer register.

```c
        } else if (a == &windows && position < a->int_arg_count) {
            out[i].reg = fp ? a->fp_args[position] : a->int_args[position];
            if (fp && i >= callee->param_count) {
                out[i].copy = a->int_args[position];
            }
        } else if (a == &sysv && fp && floats < a->fp_arg_count) {
            out[i].reg = a->fp_args[floats++];
        } else if (a == &sysv && !fp && ints < a->int_arg_count) {
            out[i].reg = a->int_args[ints++];
        } else {
            out[i].stack = true;
            out[i].offset = offset;
            offset += 8;
        }
```

AAPCS64 counts the two classes separately too, with `v0` to `v7` for floats[^3]. Apple passes every variadic argument on the stack in 8 bytes and a named stack argument in its own size, so an `f32` takes 4 bytes[^11]. A Windows ARM64 variadic function uses no float registers[^12]. antic moves the bits of a variadic float into an integer register with `fmov`.

The loop of `locate` in `src/arm64.c` first decides whether an argument is variadic, whether it takes a float register and how many bytes it needs on the stack.

```c
    for (i = 0; i < count; i++) {
        bool variadic = i >= callee->param_count;
        bool fp = select_is_float(types[i]) &&
                  !(s->abi == &windows && variadic);
        int64_t size = s->abi == &apple && !variadic ? bits(types[i]) / 8 : 8;
        memset(&out[i], 0, sizeof out[i]);
        out[i].copy = -1;
```

After the aggregate branches of chapter 18, a scalar argument takes the next register of its class or a stack slot at its alignment.

```c
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
```

The test program `tests/dump/floatcalls.anti` calls the variadic `printf` with a float and two C functions with mixed arguments.

```anti
extern fn printf(format: *byte, ...) -> i32;
extern fn mix(a: int, x: f64, b: int, y: f32) -> f64;
extern fn nine(a: f64, b: f64, c: f64, d: f64, e: f64, f: f64, g: f64, h: f64, i: f32, j: f32) -> f32;

fn calls(p: *byte, v: f64) -> f64
{
    printf(p, v, 3);
    return mix(1, v, 2, 0.5) + nine(v, v, v, v, v, v, v, v, 1.0, 2.0) as f64;
}

fn last(a: f64, b: f64, c: f64, d: f64, e: f64, f: f64, g: f64, h: f64,
    i: f32, j: f32) -> f32
{
    return j;
}
```

On linux-x86_64 the parameter `v` lives across the calls, and no xmm register survives a call, so it spills to `16(%rsp)`. The call of `printf` loads 1 into `eax`, and the last two arguments of `nine` go to the stack.

```text
floatcalls.calls:
b0:
    pushq %rbp
    movq %rsp, %rbp
    subq $32, %rsp
    movsd %xmm0, %xmm14
    movsd %xmm14, 16(%rsp)
    movsd 16(%rsp), %xmm14
    movsd %xmm14, %xmm0
    movq $3, %rsi
    movl $1, %eax
    call printf
    movq $1, %rdi
    movsd 16(%rsp), %xmm14
    movsd %xmm14, %xmm0
    movq $2, %rsi
    movl $1056964608, %eax
    movd %eax, %xmm1
    call mix
    movsd %xmm0, %xmm14
    movsd %xmm14, 24(%rsp)
    movl $1065353216, %eax
    movd %eax, %xmm0
    movss %xmm0, (%rsp)
    movl $1073741824, %eax
    movd %eax, %xmm0
    movss %xmm0, 8(%rsp)
    movsd 16(%rsp), %xmm14
    movsd %xmm14, %xmm0
    movsd 16(%rsp), %xmm14
    movsd %xmm14, %xmm1
    movsd 16(%rsp), %xmm14
    movsd %xmm14, %xmm2
    movsd 16(%rsp), %xmm14
    movsd %xmm14, %xmm3
    movsd 16(%rsp), %xmm14
    movsd %xmm14, %xmm4
    movsd 16(%rsp), %xmm14
    movsd %xmm14, %xmm5
    movsd 16(%rsp), %xmm14
    movsd %xmm14, %xmm6
    movsd 16(%rsp), %xmm14
    movsd %xmm14, %xmm7
    call nine
    cvtss2sd %xmm0, %xmm0
    movsd 24(%rsp), %xmm14
    movsd %xmm14, %xmm1
    addsd %xmm0, %xmm1
    movsd %xmm1, %xmm0
    movq %rbp, %rsp
    popq %rbp
    ret
floatcalls.last:
b0:
    pushq %rbp
    movq %rsp, %rbp
    movss 24(%rbp), %xmm0
    popq %rbp
    ret
```

On windows-x86_64 `v` arrives in `xmm1` and lives in `xmm6`, which the prologue saves with `movups`. The lines `.seh_savexmm` record those saves in the unwind data of chapter 16. The call of `printf` copies `xmm1` into `rdx`.

```text
floatcalls.calls:
b0:
    pushq %rbp
    .seh_pushreg %rbp
    movq %rsp, %rbp
    subq $112, %rsp
    .seh_stackalloc 112
    movups %xmm6, 96(%rsp)
    .seh_savexmm %xmm6, 96
    movups %xmm7, 80(%rsp)
    .seh_savexmm %xmm7, 80
    .seh_endprologue
    movsd %xmm1, %xmm6
    movsd %xmm6, %xmm1
    movq %xmm1, %rdx
    movq $3, %r8
    call printf
    movq $1, %rcx
    movsd %xmm6, %xmm1
    movq $2, %r8
    movl $1056964608, %eax
    movd %eax, %xmm3
    call mix
    movsd %xmm0, %xmm7
    movsd %xmm6, 32(%rsp)
    movsd %xmm6, 40(%rsp)
    movsd %xmm6, 48(%rsp)
    movsd %xmm6, 56(%rsp)
    movl $1065353216, %eax
    movd %eax, %xmm0
    movss %xmm0, 64(%rsp)
    movl $1073741824, %eax
    movd %eax, %xmm0
    movss %xmm0, 72(%rsp)
    movsd %xmm6, %xmm0
    movsd %xmm6, %xmm1
    movsd %xmm6, %xmm2
    movsd %xmm6, %xmm3
    call nine
    cvtss2sd %xmm0, %xmm0
    movsd %xmm7, %xmm1
    addsd %xmm0, %xmm1
    movsd %xmm1, %xmm0
    movups 96(%rsp), %xmm6
    movups 80(%rsp), %xmm7
    addq $112, %rsp
    popq %rbp
    ret
floatcalls.last:
b0:
    pushq %rbp
    .seh_pushreg %rbp
    movq %rsp, %rbp
    .seh_endprologue
    movss 88(%rbp), %xmm0
    popq %rbp
    ret
```

On macos-arm64 `v` lives in `d8`. The variadic `printf` receives `v` at `[sp]` and 3 at `[sp, #8]`, and `nine` receives its two `f32` arguments at `[sp]` and `[sp, #4]`.

```text
floatcalls.calls:
b0:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    sub sp, sp, #32
    str d8, [sp, #24]
    str d9, [sp, #16]
    fmov d8, d0
    str d8, [sp]
    mov x9, #3
    str x9, [sp, #8]
    bl printf
    mov x0, #1
    fmov d0, d8
    mov x1, #2
    movz w9, #16128, lsl #16
    fmov s1, w9
    bl mix
    fmov d9, d0
    movz w9, #16256, lsl #16
    fmov s18, w9
    str s18, [sp]
    movz w9, #16384, lsl #16
    fmov s18, w9
    str s18, [sp, #4]
    fmov d0, d8
    fmov d1, d8
    fmov d2, d8
    fmov d3, d8
    fmov d4, d8
    fmov d5, d8
    fmov d6, d8
    fmov d7, d8
    bl nine
    fcvt d18, s0
    fadd d0, d9, d18
    ldr d8, [sp, #24]
    ldr d9, [sp, #16]
    mov sp, x29
    ldp x29, x30, [sp], #16
    ret
floatcalls.last:
b0:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    ldr s0, [x29, #20]
    ldp x29, x30, [sp], #16
    ret
```

The listing of windows-arm64 moves `v` into `x1` with `fmov x1, d8` before `printf`, and the listing of linux-arm64 passes it in `d0`. The tests `dump_alloc_floatcalls.windows-arm64` and `dump_alloc_floatcalls.linux-arm64` pin both.

## Self-moves after allocation

Register allocation drops a move of a register into itself. A zero extension from 32 bits is such a move when both operands get the same register, and it still changes the value. On x86_64 `movl %eax, %eax` clears the upper 32 bits of `rax`[^13], and on ARM64 a write to `w0` clears the upper bits of `x0`[^4]. The zero extension therefore has an opcode of its own without the move flag, `A64_MOVW` on ARM64 and `X64_MOVL` on x86_64.

```c
    /* A move of a w register clears the upper 32 bits, even into itself,
       so it is no move that register allocation may drop. */
    [A64_MOVW] = {"mov", {DEF, USE}, 0},
```

```c
    /* A 32-bit move that clears the upper 32 bits, even of its own
       register, so it is no move that register allocation may drop. */
    [X64_MOVL] = {"mov", {DEF, USE}, 0},
```

The unit tests `test_arm64` and `test_x86_64` check `x as u32 as u64` and `g() as u32 as u64`, whose listings keep `mov w0, w0` and `movl %eax, %eax`.

## A float program

The program test `float_math` runs on the development Mac. It computes with `f32` and `f64`, converts `u64` values above 2^63 in both directions, compares NaN, and prints seven values with `printf` and the format `%.17g`.

```anti
extern fn printf(format: *byte, ...) -> i32;

fn show(x: f64)
{
    let format = alloc(byte, 7);
    format[0] = 37;
    format[1] = 46;
    format[2] = 49;
    format[3] = 55;
    format[4] = 103;
    format[5] = 10;
    format[6] = 0;
    printf(format, x);
    free(format);
}

fn divide(a: f64, b: f64) -> f64
{
    return a / b;
}

fn hypot2(a: f64, b: f32) -> f64
{
    return a * a + (b * b) as f64;
}

fn main() -> int
{
    let n = 0;
    let x: f32 = 1.1;
    let y = x as f64 * 3.0;
    show(y);
    show(hypot2(3.0, 4.0));
    show(-y / 7.0);
    let big: u64 = 18446744073709551615;
    show(big as f64);
    let huge = divide(9300000000000000000.0, 1.0);
    show((huge as u64) as f64);
    show((divide(-7.5, 1.0) as i32) as f64);
    show((x - 0.25) as f64);
    let nan = divide(0.0, 0.0);
    if nan != nan {
        n += 1;
    }
    if !(nan < 1.0) {
        n += 2;
    }
    if !(nan >= 1.0) {
        n += 4;
    }
    if !(nan == nan) {
        n += 8;
    }
    if y > 3.0 {
        n += 16;
    }
    if -y <= -3.0 {
        n += 32;
    }
    return n;
}
```

A C program with the same operations, compiled with Apple clang 21.0.0, prints the same seven lines and exits with 63. The file `float_math.expected` holds that output.

```text
exit 63
3.3000000715255737
25
-0.47142858164651052
1.8446744073709552e+19
9.3e+18
-7
0.85000002384185791
```

The x86_64 float code assembles for all three x86_64 triples in the asm tests. The test `program_float_math_macos-x86_64` runs it on the development Mac through Rosetta, and the two other x86_64 targets need their own machine.

## Tests

Float arithmetic, constants, memory, calls, comparisons, conversions and a far spill are pinned, and the float listings assemble for all six triples. The build has zero warnings, and every `ctest` test passes.

## Next

[Chapter 18, Structs and arrays]({{% relref "/programming/writing-a-compiler/18-structs-and-arrays" %}}), lays out structs, unions, bitfields and packed and aligned structs in the back end for each target. Bitfields follow the System V or the MSVC rule. It adds field access, value semantics, copying, fixed-size arrays and indexing. It covers struct and union passing by value under all three calling conventions. A small raylib binding and the ABI probe test the rules.

## References

[^1]: H.J. Lu and others, *System V Application Binary Interface, AMD64 Architecture Processor Supplement*, version 1.0, March 12, 2025, sections 3.2.1 and 3.2.3, figure 3.4, https://gitlab.com/x86-psABIs/x86-64-ABI

[^2]: Microsoft, *x64 calling convention*, sections "Parameter passing", "Varargs" and "Caller/callee saved registers", https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention?view=msvc-170

[^3]: Arm Limited, *Procedure Call Standard for the Arm 64-bit Architecture*, release 2025Q4, sections 6.1.2 and 6.8.2, https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst

[^4]: Arm Limited, *Arm A-profile A64 Instruction Set Architecture*, release 2025-12, XML files `fcmp_float.xml`, `fmov_float_gen.xml`, `scvtf_float_int.xml`, `fcvtzu_float_int.xml`, `fcvt_float.xml` and `shared_pseudocode.xml`, https://developer.arm.com/-/cdn-downloads/permalink/Exploration-Tools-A64-ISA/ISA_A64/ISA_A64_xml_A_profile-2025-12.tar.gz

[^5]: Intel, *Intel 64 and IA-32 Architectures Software Developer's Manual*, Volume 2, MOVSD, December 2023 edition as indexed by F. Cloutier, https://www.felixcloutier.com/x86/movsd

[^6]: ISO/IEC JTC1/SC22/WG14, *N1570, Committee Draft of ISO/IEC 9899:201x*, 2011, section 7.12.14, https://www.open-std.org/jtc1/sc22/wg14/www/docs/n1570.pdf

[^7]: Intel, *Intel 64 and IA-32 Architectures Software Developer's Manual*, Volume 2, UCOMISD, December 2023 edition as indexed by F. Cloutier, https://www.felixcloutier.com/x86/ucomisd

[^8]: Intel, *Intel 64 and IA-32 Architectures Software Developer's Manual*, Volume 2, CVTSI2SD, December 2023 edition as indexed by F. Cloutier, https://www.felixcloutier.com/x86/cvtsi2sd

[^9]: Intel, *Intel 64 and IA-32 Architectures Software Developer's Manual*, Volume 2, CVTTSD2SI, December 2023 edition as indexed by F. Cloutier, https://www.felixcloutier.com/x86/cvttsd2si

[^10]: Intel, *Intel 64 and IA-32 Architectures Software Developer's Manual*, Volume 2, ANDNPD, December 2023 edition as indexed by F. Cloutier, https://www.felixcloutier.com/x86/andnpd

[^11]: Apple, *Writing ARM64 code for Apple platforms*, sections on arguments and variadic functions, https://developer.apple.com/documentation/xcode/writing-arm64-code-for-apple-platforms

[^12]: Microsoft, *Overview of ARM64 ABI conventions*, section "Addendum: Variadic functions", https://learn.microsoft.com/en-us/cpp/build/arm64-windows-abi-conventions?view=msvc-170

[^13]: Intel, *Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 1: Basic Architecture*, order number 253665-023US, May 2007, section 3.4.1.1, https://pdos.csail.mit.edu/6.828/2018/readings/ia32/IA32-1.pdf
