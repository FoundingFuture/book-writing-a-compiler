---
title: "The x86_64 back end"
description: "The integer patterns of the antic x86_64 back end: division, shifts, conversions, memory operands, stack arguments and Windows frames."
summary: "Instruction patterns, addressing modes, condition codes and calls under System V and Windows x64. The emitted assembly for the test programs."
date: 2026-09-13T22:32:00+02:00
lastmod: 2026-09-15T03:48:32+02:00
draft: false
weight: 140
tags: [compilers, assembly]
keywords: [x86_64, AT&T syntax, integer division, memory operand, scaled index, stack arguments, shadow store, __chkstk]
---

## Previously

[Chapter 13, Register allocation and stack frames]({{% relref "/programming/writing-a-compiler/13-register-allocation" %}}), gives every virtual register a physical register with linear scan. It spills values to the stack, saves callee-saved registers and lays out frames. Address-taken locals live in stack slots.

## Scope of the back end

Chapter 12 selected x86_64 instructions for 32-bit and 64-bit arithmetic, comparisons, branches and calls with register arguments. This chapter completes the integer patterns in `src/x86_64.c`. The table gains division and remainder, shifts, the conversions `trunc`, `sext` and `zext`, and the operation `addr`. Every integer width from 8 to 64 bits is allowed, and so is `bool`. Calls pass arguments on the stack, and functions read parameters from it.

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

The listings of this chapter come from `--dump-alloc`, the machine code after register allocation. Chapter 16 adds the directives that turn the listing into an assembly file. Floating-point values and aggregates still stop with the message of chapter 12, because chapters 17 and 18 lower them.

## Registers and widths

An x86_64 register has a name for each width: `rax` for 64 bits, `eax` for 32, `ax` for 16 and `al` for 8. The printer takes the name from one table per width.

```c
static const char *const names64[] = {
    "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
};
static const char *const names32[] = {
    "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi",
    "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d",
};
static const char *const names16[] = {
    "ax", "cx", "dx", "bx", "sp", "bp", "si", "di",
    "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w",
};
static const char *const names8[] = {
    "al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil",
    "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b",
};
```

The width of an operation decides what happens to the upper bits of the destination. The Intel manual states that "32-bit operands generate a 32-bit result, zero-extended to a 64-bit result"[^1]. An 8-bit or 16-bit operation leaves the upper 56 or 48 bits unchanged[^1]. The back end of antic therefore treats the bits above the width of a value as unknown. An instruction that reads more bits than the value holds extends it first.

The IR types `clong` and `cwchar` never reach the patterns of either back end. Before selection, the function `layout_resolve` in `src/layout.c` gives both their width on the target. The type `clong` becomes `i32` on Windows and `i64` on Linux and macOS. The type `cwchar` becomes `i16` on Windows and `i32` on Linux and macOS.

## Multiplication

The instruction `imul` multiplies signed values, and its low half is the same for unsigned values. It has a two-operand form for 16, 32 and 64 bits and a three-operand form with an immediate[^2]. Neither form exists for 8 bits[^2]. The function `emit_mul` multiplies 8-bit and 16-bit values in the 32-bit registers, whose low 8 or 16 bits hold the same product.

```c
/* imul has a three-operand form with an immediate. It has no two-operand
   form for 8 bits, so 8-bit and 16-bit products use the 32-bit
   registers, whose low bits hold the same product. */
static void emit_mul(struct selector *s, const struct ir_inst *inst)
{
    struct mach_operand r = select_result(s, inst);
    struct mach_operand ops[3];
    uint8_t w = r.width < 32 ? 32 : r.width;

    if (!fits_imm(&inst->b, r.width)) {
        struct mach_operand b;
        if (inst->b.kind == IR_TEMP && inst->b.as.temp == inst->result) {
            b = widened(select_reg(s, &inst->a), w);
        } else {
            b = widened(select_reg(s, &inst->b), w);
            if (inst->a.kind == IR_INT) {
                load(s, widened(r, w), inst->a.as.integer);
            } else if (inst->a.as.temp != inst->result) {
                move(s, widened(r, w), widened(select_reg(s, &inst->a), w));
            }
        }
        emit2(s, X64_IMUL, widened(r, w), b);
        return;
    }
    ops[0] = widened(r, w);
    ops[1] = widened(select_reg(s, &inst->a), w);
    ops[2] = mach_imm(signed_value(inst->b.as.integer, r.width));
    select_emit(s, X64_IMUL3, 3, ops);
}
```

The helper `widened` changes the width of a register operand and leaves other operands unchanged. The test program `tests/dump/x86.anti` exercises the patterns of this chapter. Its function `bytes` multiplies a `u8` by 3 and adds 1 to an `i8`.

```anti
fn bytes(a: u8, b: i8) -> int
{
    return (a * 3) as int + (b + 1) as int;
}
```

```text
x86.bytes:
b0:
    imull $3, %edi, %eax
    movzbq %al, %rax
    addb $1, %sil
    movsbq %sil, %rcx
    addq %rcx, %rax
    ret
```

The product goes to `eax` as a 32-bit multiplication, and `movzbq` extends its low byte to 64 bits. The addition on `i8` stays an 8-bit `addb` on `sil`, and `movsbq` sign-extends the result. The test `dump_alloc_x86.x86_64` compares the listing of the whole program with `tests/dump/x86.x86_64.alloc`.

## Division and remainder

The instruction `idiv` divides the signed value in `rdx:rax` by its operand. It stores the quotient in `rax` and the remainder in `rdx`[^3]. The instruction `div` does the same for unsigned values[^3]. The dividend is twice as wide as the operand, so the selector must fill `rdx` first. The instruction `cqto`, called CQO in Intel syntax, sign-extends `rax` into `rdx:rax`[^4]. The 32-bit form `cltd`, Intel's CDQ, does the same for `eax`[^4]. An unsigned division loads 0 into `rdx`.

```c
/* Division takes the dividend in rdx:rax and leaves the quotient in rax
   and the remainder in rdx. cqto and cltd sign-extend rax into rdx, and an
   unsigned division clears rdx. 8-bit and 16-bit operands divide as 32-bit
   values after an extension. */
static void emit_div(struct selector *s, const struct ir_inst *inst)
{
    bool is_signed = inst->op == IR_SDIV || inst->op == IR_SREM;
    bool remainder = inst->op == IR_SREM || inst->op == IR_UREM;
    struct mach_operand r = select_result(s, inst);
    uint8_t w = r.width < 32 ? 32 : r.width;
    struct mach_operand divisor;
    struct mach_inst *div;

    if (inst->b.kind == IR_TEMP && r.width >= 32) {
        divisor = select_reg(s, &inst->b);
    } else {
        divisor = select_new_vreg(s, w);
        extend_into(s, divisor, &inst->b, is_signed);
    }
    extend_into(s, mach_preg(RAX, w), &inst->a, is_signed);
    if (is_signed) {
        select_emit(s, w == 64 ? X64_CQTO : X64_CLTD, 0, NULL)->uses =
            BIT(RAX);
        s->b->insts[s->b->count - 1].defs = BIT(RDX);
    } else {
        load(s, mach_preg(RDX, w), 0);
    }
    div = emit1(s, is_signed ? X64_IDIV : X64_DIV, divisor);
    div->uses = BIT(RAX) | BIT(RDX);
    div->defs = BIT(RAX) | BIT(RDX);
    move(s, r, mach_preg(remainder ? RDX : RAX, r.width));
}
```

The helper `extend_into` loads a constant, extends an 8-bit or 16-bit register with `movsx` or `movzx`, or moves a wider one. The fields `uses` and `defs` of `cqto` and `idiv` name `rax` and `rdx`. The register allocator of chapter 13 turns them into fixed ranges, so no value that is live across the division gets either register. An 8-bit or 16-bit division extends both operands to 32 bits and divides with `idivl` or `divl`. The function `div` of the test program computes a quotient and a remainder of two `int` values.

```anti
fn div(a: int, b: int) -> int
{
    return a / b + a % b;
}
```

```text
x86.div:
b0:
    movq %rdi, %rax
    cqto
    idivq %rsi
    movq %rax, %rcx
    movq %rdi, %rax
    cqto
    idivq %rsi
    movq %rcx, %rax
    addq %rdx, %rax
    ret
```

The IR has `sdiv` and `srem` as two instructions, so the listing divides twice. The first quotient moves to `rcx`, and the second division leaves the remainder in `rdx`. Division by zero raises the divide error `#DE`, and so does a quotient too large for the register, as for the minimum value divided by -1[^3]. Anti defines neither case, as chapter 2 states.

The function `narrow` of the test program divides 16-bit values.

```anti
fn narrow(a: i16, b: i16) -> i16
{
    let q = a / b;
    let r = a % b;
    if a < b {
        return q * r;
    }
    return (q >> 1) ^ (r << 3);
}
```

```text
x86.narrow:
b0:
    movswl %si, %ecx
    movswl %di, %eax
    cltd
    idivl %ecx
    movw %ax, %cx
    movswl %si, %r8d
    movswl %di, %eax
    cltd
    idivl %r8d
    cmpw %si, %di
    jge b2
b1:
    movl %ecx, %eax
    imull %edx, %eax
    ret
b2:
    sarw $1, %cx
    shlw $3, %dx
    movw %cx, %ax
    xorw %dx, %ax
    ret
```

Both operands pass through `movswl` into 32-bit registers before `cltd` and `idivl`. The quotient `q` is kept in `cx` and the remainder `r` in `dx`. The product `q * r` uses `imull` on the 32-bit registers, and the return value is the low 16 bits of `eax`.

## Shifts

A shift by a constant takes an immediate. A shift by a variable count takes the count in `cl`[^5], so the selector moves the low byte of the count into `cl` first. The processor masks the count to 5 bits, or to 6 bits for a 64-bit operand[^5]. A count below the width passes the mask unchanged. [Chapter 2]({{% relref "/programming/writing-a-compiler/02-the-anti-language" %}}) makes a larger count undefined behaviour.

```c
/* A variable shift count must be in cl. */
static void emit_shift(struct selector *s, const struct ir_inst *inst)
{
    enum x64_op op = inst->op == IR_SHL     ? X64_SHL
                     : inst->op == IR_SHR_S ? X64_SAR
                                            : X64_SHR;
    struct mach_operand r = select_result(s, inst);

    if (inst->b.kind == IR_INT) {
        load_into(s, r, &inst->a);
        emit2(s, op, r, mach_imm((int64_t)(inst->b.as.integer & 0xff)));
        return;
    }
    move(s, mach_preg(RCX, 8), widened(select_reg(s, &inst->b), 8));
    load_into(s, r, &inst->a);
    emit2(s, op, r, mach_preg(RCX, 8));
}
```

The IR operation `shl` becomes `shl`, `shr_s` becomes `sar` and `shr_u` becomes `shr`. The function `shifts` of the test program applies all three to an `int`.

```anti
fn shifts(a: int, n: int) -> int
{
    return (a << n) + (a >> n) + ((a as u64 >> n as u64) as int);
}
```

```text
x86.shifts:
b0:
    movb %sil, %cl
    movq %rdi, %rax
    shlq %cl, %rax
    movb %sil, %cl
    movq %rdi, %rdx
    sarq %cl, %rdx
    addq %rdx, %rax
    movb %sil, %cl
    shrq %cl, %rdi
    addq %rdi, %rax
    ret
```

The selector emits the move into `cl` for each shift, and no pass removes the repeated moves. Each move starts a fixed range of `rcx`. The value `a` stays in `rdi`, and the partial results take `rax` and `rdx`.

## Conversions

The conversions between integer types of chapter 7 each become one instruction. The instruction `movsx` copies a smaller value into a larger register with sign extension, and `movzx` with zero extension[^6]. AT&T syntax spells them `movs` and `movz` with two size suffixes, source first, as `movswl` for 16 to 32 bits and `movzbq` for 8 to 64 bits.

```c
/* trunc keeps the low bits with a move of the smaller width. sext uses
   movsx. zext uses movzx, and from 32 bits a 32-bit move, whose result the
   processor zero-extends to 64 bits. */
static void emit_convert(struct selector *s, const struct ir_inst *inst)
{
    struct mach_operand r = select_result(s, inst);
    uint8_t from = width(inst->a.type);

    if (inst->a.kind == IR_INT) {
        load(s, r, inst->op == IR_SEXT
                       ? (uint64_t)signed_value(inst->a.as.integer, from)
                       : inst->a.as.integer);
    } else if (inst->op == IR_TRUNC) {
        move(s, r, widened(select_reg(s, &inst->a), r.width));
    } else if (inst->op == IR_ZEXT && from == 32) {
        emit2(s, X64_MOVL, widened(r, 32), select_reg(s, &inst->a));
    } else {
        emit2(s, inst->op == IR_SEXT ? X64_MOVSX : X64_MOVZX, r,
              select_reg(s, &inst->a));
    }
}
```

A `zext` from 32 bits has no `movzx` form[^6]. A 32-bit `mov` takes its place, because the processor zero-extends every 32-bit result[^1]. The opcode `X64_MOVL` prints that `movl`. It carries no move flag, so register allocation never drops it as a move of a register into itself, which would leave the upper 32 bits unchanged. A `trunc` is a `mov` of the smaller width, which reads the low bits of the source register.

## Condition codes

Chapter 12 turns a comparison into `cmp` and a conditional jump or a `set` instruction. The condition is a suffix of the mnemonic. Intel associates the terms "above" and "below" with unsigned values and "greater" and "less" with signed values[^7]. The printer maps the conditions of the machine code to those suffixes.

```c
static const char *const cond_names[] = {
    [COND_EQ] = "e", [COND_NE] = "ne", [COND_LT] = "l", [COND_LE] = "le",
    [COND_GT] = "g", [COND_GE] = "ge", [COND_LO] = "b", [COND_LS] = "be",
    [COND_HI] = "a", [COND_HS] = "ae", [COND_P] = "p", [COND_NP] = "np",
};
```

The conditions `p` and `np` test the parity flag, which chapter 17 uses for the comparisons of floats with NaN.

| IR comparison | Condition | Jump | Flags tested |
|---|---|---|---|
| `eq`, `ne` | `e`, `ne` | `je`, `jne` | ZF |
| `slt`, `sge` | `l`, `ge` | `jl`, `jge` | SF xor OF |
| `sle`, `sgt` | `le`, `g` | `jle`, `jg` | (SF xor OF) or ZF |
| `ult`, `uge` | `b`, `ae` | `jb`, `jae` | CF |
| `ule`, `ugt` | `be`, `a` | `jbe`, `ja` | CF or ZF |

The flags in the last column come from table B-1 of the Intel manual[^7]. The comparison `a < b` of `narrow` compares 16-bit values with `cmpw` and jumps with `jge`, the negated condition, to the block that does not follow.

## Memory operands

An x86_64 memory operand adds up to four parts: a displacement of up to 32 bits, a base register, an index register and a scale of 2, 4 or 8[^1]. AT&T syntax writes it as `displacement(base,index,scale)`. Lowering computes the address of `p[i]` with `ptradd` and a `mul` of the index by the symbolic element size, as in `mul i64 %3, size_of i32`. The back end folds `size_of i32` to 4 before selection and runs the optimizer again on the function. The optimizer turns the multiplication by 4 into a shift left by 2. The function `fold_address` in `src/select.c` merges those instructions into one memory operand.

```c
/* A ptradd whose only use is the load or store right after it becomes
   part of that instruction's address. A shift or multiplication of the
   index right before it joins the address as the scale. Returns the
   number of instructions that the address replaces, or 0. */
static size_t fold_address(struct selector *s, const struct ir_block *b,
                           size_t i)
{
    const struct ir_inst *inst = &b->insts[i];
    const struct ir_inst *add = inst;
    const struct ir_inst *scaled = NULL;
    struct address a;
    size_t n = 1;

    memset(&a, 0, sizeof a);
    if ((inst->op == IR_SHL || inst->op == IR_MUL) && i + 2 < b->count &&
        inst->b.kind == IR_INT && s->uses[inst->result] == 1 &&
        b->insts[i + 1].op == IR_PTRADD &&
        b->insts[i + 1].b.kind == IR_TEMP &&
        b->insts[i + 1].b.as.temp == inst->result) {
        uint64_t k = inst->b.as.integer;
        if (inst->op == IR_MUL) {
            k = k == 1 ? 0 : k == 2 ? 1 : k == 4 ? 2 : k == 8 ? 3 : 4;
        }
        if (k > 3 || inst->a.kind != IR_TEMP) {
            return 0;
        }
        scaled = inst;
        add = &b->insts[i + 1];
        a.shift = (uint8_t)k;
        n = 2;
    }
    if (add->op != IR_PTRADD || add->a.kind != IR_TEMP ||
        i + n >= b->count || s->uses[add->result] != 1 ||
        !uses_as_address(&b->insts[i + n], add->result)) {
        return 0;
    }
    a.base = &add->a;
    if (scaled != NULL) {
        a.index = &scaled->a;
    } else if (add->b.kind == IR_TEMP) {
        a.index = &add->b;
    } else {
        a.offset = (int64_t)add->b.as.integer;
    }
    if (!s->target->fits_address(s, &a, &b->insts[i + n])) {
        return 0;
    }
    s->address = a;
    s->has_address = true;
    return n;
}
```

A `ptradd` folds only when its result has exactly one use, the `load` or `store` right after it. A preceding shift folds as the scale when its count is 0 to 3. A factor of 1, 2, 4 or 8 lets a multiplication fold the same way. The target decides the rest in its function `fits_address`.

```c
/* A displacement holds 32 bits and a scale is 1, 2, 4 or 8. A store with
   an index takes only an immediate value, so that no instruction reads
   more than two registers. */
static bool fits_address(const struct selector *s, const struct address *a,
                         const struct ir_inst *use)
{
    (void)s;
    if (a->offset < INT32_MIN || a->offset > INT32_MAX) {
        return false;
    }
    if (a->index != NULL && use->op == IR_STORE) {
        return fits_imm(&use->a, width(use->a.type));
    }
    return true;
}
```

A store with an index reads the base and the index register. A stored register would make three, and chapter 13 has two scratch registers for spilled values. The store therefore folds an index only with an immediate value. The ARM64 function `fits_address` returns `false`, and chapter 15 adds the ARM64 addressing modes. The test `dump_alloc_sum.x86_64` compiles a sum over an `i32` array and a loop that fills an `i16` array.

```anti
fn sum(p: *i32, n: int) -> int
{
    let s = 0;
    let i = 0;
    while i < n do {
        s += p[i] as int;
        i += 1;
    }
    return s;
}

fn fill(p: *i16, n: int)
{
    let i = 0;
    while i < n do {
        p[i] = 7;
        i += 1;
    }
}
```

```text
sum.sum:
b0:
    movq $0, %rax
    movq $0, %rcx
b1:
    cmpq %rsi, %rcx
    jge b3
b2:
    movl (%rdi,%rcx,4), %edx
    movslq %edx, %rdx
    addq %rdx, %rax
    addq $1, %rcx
    jmp b1
b3:
    ret
sum.fill:
b0:
    movq $0, %rax
b1:
    cmpq %rsi, %rax
    jge b3
b2:
    movw $7, (%rdi,%rax,2)
    addq $1, %rax
    jmp b1
b3:
    ret
```

The load `movl (%rdi,%rcx,4), %edx` replaces three IR instructions: the shift of `i` by 2, the `ptradd` and the `load`. The store `movw $7, (%rdi,%rax,2)` writes the immediate 7 into 16 bits. The IR operation `addr` takes the address of a function or a global. On x86_64 it becomes `lea` with a displacement relative to `rip`[^1], so the code stays position-independent, as chapter 11 requires. Lowering emits `addr` for the string literals of chapter 19 and the functions of chapter 20. The unit test `rip_relative` builds its IR directly.

## Stack arguments and parameters

System V passes the first six integer values in registers. The others go to memory, the first of them at the lowest address[^8]. Windows x64 passes four in registers[^9]. The caller always reserves 32 bytes of shadow store for them, and the fifth argument lies above it[^9]. The function `emit_call` writes each stack argument with `mov` into the area at the bottom of the frame.

```c
/* Stack arguments go to the area at the bottom of the frame, then the
   register arguments into their registers. A variadic call under System V
   sets al to the number of float registers in use. Windows passes a
   variadic float in the integer register of its position as well. */
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
    uint64_t outgoing = s->abi->shadow_space;
    size_t floats = 0;
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
        uint8_t w = width(arg->type);
        struct mach_operand slot = stack(locations[i].offset);
        if (types[i] == IR_AGG && (locations[i].indirect ||
                                   locations[i].stack)) {
            copies[i] = copy_argument(s, &locations[i],
                                      select_layout(s, callee->params[i].agg),
                                      select_reg(s, arg));
        }
        if (types[i] == IR_AGG && locations[i].stack) {
            uint64_t end = (uint64_t)locations[i].offset +
                           (locations[i].indirect
                                ? 8
                                : (select_layout(s, callee->params[i].agg)->size + 7) / 8 * 8);
            if (end > outgoing) {
                outgoing = end;
            }
        }
        if (types[i] == IR_AGG || !locations[i].stack) {
            continue;
        }
        slot.width = w;
        if (select_is_float(arg->type)) {
            move_float(s, slot, select_reg(s, arg));
        } else {
            emit2(s, X64_MOV, slot,
                  fits_imm(arg, w) ? mach_imm(signed_value(arg->as.integer, w))
                                   : select_reg(s, arg));
        }
        if ((uint64_t)locations[i].offset + 8 > outgoing) {
            outgoing = (uint64_t)locations[i].offset + 8;
        }
    }
    if (outgoing > s->out->outgoing) {
        s->out->outgoing = outgoing;
    }
    for (i = 0; i < inst->arg_count; i++) {
        const struct ir_operand *arg = &inst->args[i];
        struct mach_operand reg = mach_preg(locations[i].reg, width(arg->type));
        if (locations[i].stack) {
            continue;
        }
        if (types[i] == IR_AGG && locations[i].indirect) {
            move(s, mach_preg(locations[i].reg, 64), copies[i]);
            uses |= BIT(locations[i].reg);
            continue;
        }
        if (types[i] == IR_AGG) {
            uses |= select_load_parts(s, &locations[i], select_reg(s, arg));
            continue;
        }
        /* DESIGN: clang callees on System V read an 8-bit or 16-bit
           register argument as 32 bits, so the caller extends it. */
        if (s->abi == &sysv && i < callee->param_count &&
            callee->params[i].ext != IR_EXT_NONE) {
            extend_into(s, mach_preg(locations[i].reg, 32), arg,
                        callee->params[i].ext == IR_EXT_SIGN);
        } else {
            load_value(s, reg, arg);
        }
        uses |= BIT(locations[i].reg);
        if (locations[i].reg >= XMM0) {
            floats++;
        }
        if (locations[i].copy >= 0) {
            emit2(s, X64_MOVQX, mach_preg((uint32_t)locations[i].copy, 64),
                  mach_preg(locations[i].reg, 64));
            uses |= BIT(locations[i].copy);
        }
    }
    if (callee->result == IR_AGG && result.indirect) {
        move(s, mach_preg(result.reg, 64), result_address);
        uses |= BIT(result.reg);
    }
    if (callee->variadic && s->abi == &sysv) {
        load(s, mach_preg(RAX, 32), floats);
        uses |= BIT(RAX);
    }
    free(types);
    free(locations);
    free(copies);
    f.kind = MACH_FUNC;
    call = emit1(s, indirect ? X64_CALLR : X64_CALL, indirect ? target : f);
    call->uses = uses;
    call->defs = s->abi->caller_saved;
    if (inst->result != IR_NO_RESULT && callee->result == IR_AGG) {
        if (!result.indirect) {
            select_store_parts(s, &result,
                               select_result_slot(s, inst,
                                                  select_layout(s, callee->result_agg)));
        }
    } else if (inst->result != IR_NO_RESULT && select_is_float(inst->type)) {
        move_float(s, select_result(s, inst),
                   mach_preg(s->abi->fp_result, width(inst->type)));
    } else if (inst->result != IR_NO_RESULT) {
        move(s, select_result(s, inst),
             mach_preg(s->abi->int_result, width(inst->type)));
    }
}
```

The function serves every argument of the book. The function `locate` gives each argument a register or an offset in the stack area, and chapter 17 describes it with the float arguments and the count in `al`. Chapter 18 adds aggregates in registers, in memory and through an address, and chapter 20 adds the call through a register. For the integer arguments of this chapter the first loop writes the stack arguments with `mov`, and the second loop loads the register arguments. A register argument of 8 or 16 bits under System V is extended to 32 bits first, which chapter 18 derives from the code of clang.

The field `outgoing` of the function records the largest argument area among its calls. Frame layout of chapter 13 places the slots above that area. Microsoft requires the parameter area "always at the bottom of the stack", adjacent to the return address of every call[^10]. A call to a variadic function under System V sets `al` to the number of vector registers in use, an upper bound in the range 0 to 8[^8]. Integer arguments use none, so antic loads 0 into `eax`.

A parameter beyond the register parameters is a memory argument of the caller. After `push %rbp` and `mov %rsp, %rbp`, System V places memory argument 0 at `16(%rbp)` and memory argument n at `8n+16(%rbp)`[^8]. On Windows x64 the shadow store counts as the first four slots. The function `locate` computes the offset of each stack parameter in the argument area, the shadow store included. The function `stack_param` adds the 16 bytes of the saved `rbp` and the return address. A float parameter of chapter 17 loads with `movs`.

```c
/* A parameter on the stack sits above the saved rbp and the return
   address, at the offset that locate gives it in the argument area. */
static void stack_param(struct selector *s, int64_t offset,
                        struct mach_operand dst)
{
    struct mach_operand m = mach_preg(RBP, 64);

    m.kind = MACH_MEM;
    m.width = dst.width;
    m.value = 16 + offset;
    s->out->stack_params = true;
    emit2(s, s->out->fp[dst.reg] ? X64_MOVS : X64_MOV, dst, m);
}
```

The test program `tests/dump/args.anti` calls a function with eight parameters and the variadic C function `printf`.

```anti
extern fn printf(format: *byte, ...) -> i32;

fn eight(a: int, b: int, c: int, d: int, e: int, f: int, g: int, h: int) -> int
{
    return a + h;
}

fn main() -> int
{
    let format = alloc(byte, 4);
    format[0] = 37;
    format[1] = 100;
    format[2] = 10;
    format[3] = 0;
    printf(format, eight(1, 2, 3, 4, 5, 6, 7, 8));
    return 0;
}
```

```text
args.eight:
b0:
    pushq %rbp
    movq %rsp, %rbp
    movq 24(%rbp), %rax
    movq %rdi, %rcx
    addq %rax, %rcx
    movq %rcx, %rax
    popq %rbp
    ret
args.main:
b0:
    pushq %rbp
    movq %rsp, %rbp
    subq $32, %rsp
    movq %rbx, 24(%rsp)
    movq $4, %rdi
    call malloc
    movq %rax, %rbx
    movb $37, (%rbx)
    movb $100, 1(%rbx)
    movb $10, 2(%rbx)
    movb $0, 3(%rbx)
    movq $7, (%rsp)
    movq $8, 8(%rsp)
    movq $1, %rdi
    movq $2, %rsi
    movq $3, %rdx
    movq $4, %rcx
    movq $5, %r8
    movq $6, %r9
    call args.eight
    movq %rbx, %rdi
    movq %rax, %rsi
    movl $0, %eax
    call printf
    movq $0, %rax
    movq 24(%rsp), %rbx
    movq %rbp, %rsp
    popq %rbp
    ret
```

On linux-x86_64 the arguments 7 and 8 go to `(%rsp)` and `8(%rsp)`, and the function `eight` reads `h` from `24(%rbp)`. Selection skips parameters that the function never reads, so the parameters `b` to `g` cost no instruction. The frame of `main` holds 16 bytes of arguments and the saved `rbx`, rounded up to 32 bytes.

```text
args.eight:
b0:
    pushq %rbp
    .seh_pushreg %rbp
    movq %rsp, %rbp
    .seh_endprologue
    movq 72(%rbp), %rax
    addq %rax, %rcx
    movq %rcx, %rax
    popq %rbp
    ret
args.main:
b0:
    pushq %rbp
    .seh_pushreg %rbp
    movq %rsp, %rbp
    subq $80, %rsp
    .seh_stackalloc 80
    movq %rbx, 72(%rsp)
    .seh_savereg %rbx, 72
    .seh_endprologue
    movq $4, %rcx
    call malloc
    movq %rax, %rbx
    movb $37, (%rbx)
    movb $100, 1(%rbx)
    movb $10, 2(%rbx)
    movb $0, 3(%rbx)
    movq $5, 32(%rsp)
    movq $6, 40(%rsp)
    movq $7, 48(%rsp)
    movq $8, 56(%rsp)
    movq $1, %rcx
    movq $2, %rdx
    movq $3, %r8
    movq $4, %r9
    call args.eight
    movq %rbx, %rcx
    movq %rax, %rdx
    call printf
    movq $0, %rax
    movq 72(%rsp), %rbx
    addq $80, %rsp
    popq %rbp
    ret
```

On windows-x86_64 the arguments 5 to 8 go to `32(%rsp)` to `56(%rsp)`, above the shadow store. The lines that start with `.seh_` are the unwind directives of chapter 16. The function `eight` reads `h` from `72(%rbp)`, that is 16 + 8 × 7. The call of `printf` sets no `al`, because the Windows convention passes variadic arguments like other arguments[^9].

## Windows frames

A Windows function whose fixed allocation is a page or more must call the helper `__chkstk` before it changes `rsp`. The helper "probes the to-be-allocated stack range"[^11]. The helper receives the size in `rax` and changes no register other than `r10`, `r11` and the flags[^11]. The prologue then subtracts `rax` from `rsp`.

```c
/* Windows requires a frame of a page or more to probe its pages first.
   __chkstk takes the size in rax and leaves every register but r10 and
   r11 unchanged. */
static void allocate_frame(struct mach_block *b, const struct frame *frame)
{
    struct mach_operand ops[2];
    struct mach_inst *call;

    if (!frame->probe) {
        ops[0] = mach_preg(RSP, 64);
        ops[1] = mach_imm((int64_t)frame->size);
        append(b, X64_SUB, 2, ops);
        return;
    }
    ops[0] = mach_preg(RAX, 32);
    ops[1] = mach_imm((int64_t)frame->size);
    append(b, X64_MOV, 2, ops);
    memset(ops, 0, sizeof ops);
    ops[0].kind = MACH_NAME;
    ops[0].name = "__chkstk";
    append(b, X64_CALL, 1, ops);
    call = &b->insts[b->count - 1];
    call->uses = BIT(RAX);
    ops[0] = mach_preg(RSP, 64);
    ops[1] = mach_preg(RAX, 64);
    append(b, X64_SUB, 2, ops);
}
```

The frame layout of `src/regalloc.c` sets `probe` for a Windows frame of 4096 bytes or more. The unit test `probe` in `tests/unit/test_x86_64.c` checks the prologue of an 8192-byte frame. Its instructions are `pushq %rbp`, `movq %rsp, %rbp`, `movl $8192, %eax`, `call __chkstk` and `subq %rax, %rsp`, followed by two saves. The unwind directives of chapter 16 stand between them.

Microsoft also restricts the epilogue, so that the unwinder can recognise it. It "must consist of either an add RSP,constant or lea RSP,constant[FPReg]", followed by pops of 8-byte registers and a return[^11]. The epilogue of chapter 13 restores `rsp` with `mov %rbp, %rsp`. The Windows epilogue frees the frame with `add`, as in `addq $80, %rsp` in the listing above. The unwind data of chapter 16 names no frame register, so the `lea` form from `rbp` does not apply.

```c
/* DESIGN: Windows x64 unwinding recognises an epilogue by its code. It
   is add rsp, or lea rsp from the frame register of the unwind data, then
   pops and ret. The Windows unwind data names no frame register, so the
   Windows epilogue frees the frame with add. */
static void epilogue(struct mach_block *b, const struct frame *frame)
{
    struct mach_operand ops[2];
    size_t i;

    if (!frame->needed) {
        return;
    }
    for (i = 0; i < frame->saved_count; i++) {
        save_register(b, frame->saved[i], frame->saved_offset[i], true);
    }
    if (frame->size > 0) {
        ops[0] = mach_preg(RSP, 64);
        ops[1] = mach_preg(RBP, 64);
        if (frame->unwind) {
            ops[1] = mach_imm((int64_t)frame->size);
            append(b, X64_ADD, 2, ops);
        } else {
            append(b, X64_MOV, 2, ops);
        }
    }
    ops[0] = mach_preg(RBP, 64);
    append(b, X64_POP, 1, ops);
}
```

The function `save_register` with its last argument `true` restores a saved register from its offset in the frame. Chapter 17 gives it the float registers, which Windows saves in 16 bytes.

The x86_64 back end sets no frame limit. The instruction `sub` and the displacements of memory operands hold 32 bits, so a frame of 2 GiB or more is out of their reach. The back end does not check that bound in this chapter.

## Assembling the output

The assembler llvm-mc accepts the code of this chapter. The tests `asm_<program>_linux-x86_64` and `asm_<program>_windows-x86_64` write the assembly file of each program with `antic -S`, the assembly emitter of chapter 16. The script `tests/run_asm.cmake` then assembles the file with llvm-mc 23.1.1 and the triples `x86_64-unknown-linux-gnu` and `x86_64-pc-windows-msvc`.

```cmake
execute_process(
    COMMAND "${LLVM_MC}" "-triple=${TRIPLE}" -filetype=obj
            -o "${WORK}/${name}.${TARGET}.o" "${assembly}"
    RESULT_VARIABLE status
    ERROR_VARIABLE err)
if(NOT status EQUAL 0 OR NOT err STREQUAL "")
    message(FATAL_ERROR "llvm-mc failed for ${assembly}\n${err}")
endif()
```

A test fails when llvm-mc exits with an error or prints a message. The programs `main`, `loop`, `twice`, `cell`, `spill7`, `sum`, `args` and `x86` of this chapter assemble for both triples. The development Mac runs a macos-x86_64 program through Rosetta, so the tests `program_<name>_macos-x86_64` of chapter 16 run this code as well. The two other x86_64 targets need their own machine, which chapter 21 describes.

### Assertions

An assertion reaches the back end as an ordinary branch, so no pattern of this chapter is about it. The comparison, the conditional jump and the call are the ones already selected for `if` and for a call of a C function.

What the back end does decide is whether the branch is there at all. A release build has already cut it, and this chapter sees a function without it.

### Atomic operations

An atomic field reaches the back end as a call of the runtime, so no pattern of this chapter is about it. The call passes the address, the width of the value and the operands, and the runtime holds one body per operation.

The instructions the operations need are therefore in the runtime and not in the output of antic. A release build of `rt/atomic.c` for this target holds `lock xadd` for `add` and `sub`, and `lock cmpxchg` for `compare_swap`. It holds `xchg` for `store` and `swap`, and a `lock cmpxchg` loop for `and` and `or`. A `load` is a plain `mov`, which on x86_64 is already ordered against the stores this model allows.

Inlining them is the next step, and the closing guide lists it with the other optimisations that follow the book.

### Thunks

The table of an interface holds thunks, which chapter 8 builds. A thunk is an ordinary IR function. It subtracts a symbolic offset from its first parameter and calls another function, so no pattern of this chapter is about it either. The subtraction is the `lea` or the `sub` that any pointer arithmetic selects, and the call is an ordinary call.

## Performance

This back end applies four rules that cost nothing to apply. The processor manuals hold many more, and a compiler that competes on speed applies most of them.

Narrow values compute in 32-bit registers. The instruction `imul` has no two-operand form for 8 bits, so 8-bit and 16-bit products use the 32-bit registers, whose low bits hold the same product. A zero extension from 32 bits to 64 bits is a 32-bit move, because a write to a 32-bit register clears the upper half of the 64-bit register. Both are in the chapter above, and both save an instruction.

A comparison that feeds a branch becomes `cmp` and a conditional jump. Without the fusion of chapter 12 the comparison would write a byte with `set`, and the branch would read that byte back with `test`.

A branch on a value that is not a comparison uses `test c, c` rather than a comparison against zero. Both set the same flags, and `test` carries no immediate, so its encoding is shorter.

The instruction `lea` computes an address without writing the flags. The back end uses it for the address of a stack slot, for an aggregate in the argument area of the caller, and for a symbol.

Three rules of the manuals are not here. The first is `lea` for arithmetic. The form `lea (%rax,%rbx,4), %rcx` is a shift and an add into a third register, in one instruction that leaves the flags alone. The second is alignment of a loop head to sixteen bytes with `.p2align 4`, which keeps the head off the end of a fetch block. The third is a division by a constant as a multiply by a reciprocal. Agner Fog's instruction tables give a 64-bit `div` a latency between twenty and ninety cycles. The microarchitecture and the operands decide where in that range it lands. An `imul` costs three to five[^12].

Four sources cover the rest. Intel's optimization reference manual holds the rules for Intel processors[^13]. AMD's software optimization guide holds them for Zen[^14]. Agner Fog's manuals cover both vendors, with the instruction tables per microarchitecture and a manual on optimizing assembly[^12][^15]. Chapter 15 names the sources for ARM64.

## Tests

Division, unsigned shifts, 8-bit arithmetic, the probe of a Windows frame and `lea` relative to `rip` are pinned, and so are the listings of this chapter. The build has zero warnings, and every `ctest` test passes.

## Next

[Chapter 15, The ARM64 back end]({{% relref "/programming/writing-a-compiler/15-arm64-back-end" %}}), gives the instruction patterns, immediates and their encoding limits, condition flags and `adrp` addressing. It covers calls under AAPCS64 and Apple's variant and shows the emitted assembly for the same test programs.

## References

[^1]: Intel, *Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 1: Basic Architecture*, order number 253665-023US, May 2007, sections 3.4.1.1 and 3.7.5.1, https://pdos.csail.mit.edu/6.828/2018/readings/ia32/IA32-1.pdf

[^2]: Intel, *Intel 64 and IA-32 Architectures Software Developer's Manual*, Volume 2, IMUL, December 2023 edition as indexed by F. Cloutier, https://www.felixcloutier.com/x86/imul

[^3]: Intel, *Intel 64 and IA-32 Architectures Software Developer's Manual*, Volume 2, IDIV and DIV, December 2023 edition as indexed by F. Cloutier, https://www.felixcloutier.com/x86/idiv and https://www.felixcloutier.com/x86/div

[^4]: Intel, *Intel 64 and IA-32 Architectures Software Developer's Manual*, Volume 2, CWD/CDQ/CQO, December 2023 edition as indexed by F. Cloutier, https://www.felixcloutier.com/x86/cwd:cdq:cqo

[^5]: Intel, *Intel 64 and IA-32 Architectures Software Developer's Manual*, Volume 2, SAL/SAR/SHL/SHR, December 2023 edition as indexed by F. Cloutier, https://www.felixcloutier.com/x86/sal:sar:shl:shr

[^6]: Intel, *Intel 64 and IA-32 Architectures Software Developer's Manual*, Volume 2, MOVSX/MOVSXD and MOVZX, December 2023 edition as indexed by F. Cloutier, https://www.felixcloutier.com/x86/movsx:movsxd and https://www.felixcloutier.com/x86/movzx

[^7]: Intel, *Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 1*, 253665-023US, May 2007, table B-1, https://pdos.csail.mit.edu/6.828/2018/readings/ia32/IA32-1.pdf

[^8]: H.J. Lu and others, *System V Application Binary Interface, AMD64 Architecture Processor Supplement*, version 1.0, March 12, 2025, sections 3.2.2 and 3.2.3, figure 3.3, https://gitlab.com/x86-psABIs/x86-64-ABI

[^9]: Microsoft, *x64 calling convention*, sections "Calling convention defaults", "Parameter passing" and "Varargs", https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention?view=msvc-170

[^10]: Microsoft, *x64 stack usage*, section "Stack allocation", https://learn.microsoft.com/en-us/cpp/build/stack-usage?view=msvc-170

[^11]: Microsoft, *x64 prolog and epilog*, sections "Prolog code" and "Epilog code", https://learn.microsoft.com/en-us/cpp/build/prolog-and-epilog?view=msvc-170

[^12]: A. Fog, *Instruction tables: latencies, throughputs and micro-operation breakdowns for Intel, AMD and VIA CPUs*, Technical University of Denmark, https://www.agner.org/optimize/instruction_tables.pdf

[^13]: Intel, *Intel 64 and IA-32 Architectures Optimization Reference Manual*, order number 248966, https://www.intel.com/content/www/us/en/content-details/671488/intel-64-and-ia-32-architectures-optimization-reference-manual.html

[^14]: AMD, *Software Optimization Guide for AMD Family 19h Processors*, publication 56665, https://www.amd.com/en/support/tech-docs/56665-software-optimization-guide-for-amd-family-19h-processors-pub

[^15]: A. Fog, *Optimizing subroutines in assembly language: an optimization guide for x86 platforms*, Technical University of Denmark, https://www.agner.org/optimize/optimizing_assembly.pdf
