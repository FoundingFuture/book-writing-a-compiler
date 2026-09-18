---
title: "Instruction selection"
description: "How antic lays out types and folds sizes for its target, then turns IR instructions into ARM64 and x86_64 instructions with a table of patterns."
summary: "From IR instructions to target instructions on virtual registers. The back end lays out the types for its target and folds the symbolic values first. A table-driven selector shared in structure by both back ends, with separate patterns per architecture."
date: 2026-09-13T22:32:00+02:00
lastmod: 2026-09-15T03:56:18+02:00
draft: false
weight: 120
tags: [compilers, assembly]
keywords: [instruction selection, virtual registers, pattern table, machine code, immediate encoding, two-operand form, condition codes, type layout]
---

## Previously

[Chapter 11, Targets, object formats and ABIs]({{% relref "/programming/writing-a-compiler/11-targets-and-abis" %}}), describes the six targets as a matrix. It covers ELF, Mach-O and COFF, and the calling conventions System V, Windows x64 and AAPCS64 with Apple's deviations. It gives the widths of `c_long` and `c_wchar` per target, and symbol naming, position-independent code, the entry point and libc linkage per operating system.

## From IR to machine code

The IR is the same on every target, and each processor has its own instruction set. Instruction selection replaces every IR instruction with instructions of the target processor. Its output is machine code, a list of target instructions per block, still in memory and not yet assembly text. The selector does not know which register will hold a value. It writes a virtual register, a numbered placeholder, and the register allocator of chapter 13 replaces each one with a register of the processor.

The option `--dump-select` prints the machine code for the target of `--target`. For `main.anti` of chapter 1 on macos-arm64, the test `dump_select_arm64` compares the output with `tests/dump/main.arm64.sel`.

```text
main.scale:
b0:
    mov t0, x0
    mov t2, #6
    mul t1, t0, t2
    mov x0, t1
    ret
main.main:
b0:
    mov x0, #7
    bl main.scale
    mov t0, x0
    mov x0, t0
    ret
```

A virtual register prints as `t` and a number. The registers `x0` and `w0` are physical registers, the registers of the processor, where the calling convention fixes them. The multiplication shows the case that chapter 1 describes: the ARM64 `mul` instruction takes no constant, so the constant 6 first goes into the register `t2`. The same program for linux-x86_64 is in `tests/dump/main.x86_64.sel`.

```text
main.scale:
b0:
    movq %rdi, %t0
    imulq $6, %t0, %t1
    movq %t1, %rax
    ret
main.main:
b0:
    movq $7, %rdi
    call main.scale
    movq %rax, %t0
    movq %t0, %rax
    ret
```

The x86_64 instruction `imul` has a form with a constant, so it needs no extra register.

## Layout and folding for the target

The IR holds types without sizes, and a machine instruction needs every offset and byte count as a number. The function `select_module` in `src/select.c` therefore prepares the IR module for its target before it selects any instruction. It lays out every aggregate with `layouts_init`, writes the bytes of the constants with `layout_data`, and replaces the symbolic values of each function with `layout_resolve`. Each function that held a symbolic value then goes through the optimizer of chapter 10 again.

```c
bool select_module(enum target t, struct ir_module *m,
                   struct mach_function **out, char *error,
                   size_t error_size)
{
    struct selector s;
    struct layouts layouts;
    size_t i;

    memset(&s, 0, sizeof s);
    for (i = 0; i < m->function_count; i++) {
        out[i] = NULL;
    }
    /* DESIGN: the back end folds the symbolic values of its target, then
       runs the optimizer again on each function that had one. A size
       folded here reaches the same simplifications as a number would
       have before. */
    if (!layouts_init(&layouts, t, m, error, error_size) ||
        !layout_data(&layouts, m)) {
        layouts_free(&layouts);
        return false;
    }
    for (i = 0; i < m->function_count; i++) {
        bool resolved = false;
        if (!layout_resolve(&layouts, m->functions[i], &resolved)) {
            layouts_free(&layouts);
            return false;
        }
        if (resolved && !m->functions[i]->is_extern) {
            ir_optimize_function(m->functions[i]);
        }
    }
    s.target = target_desc(t);
    s.abi = s.target->abi(target_info(t)->convention);
    s.convention = target_info(t)->convention;
    s.m = m;
    s.layouts = &layouts;
    s.error = error;
    s.error_size = error_size;
    for (i = 0; i < m->function_count && !s.failed; i++) {
        out[i] = NULL;
        if (m->functions[i]->is_extern) {
            continue;
        }
        out[i] = calloc(1, sizeof *out[i]);
        if (out[i] == NULL) {
            fputs("antic: out of memory\n", stderr);
            exit(70);
        }
        s.f = m->functions[i];
        s.out = out[i];
        select_function(&s);
    }
    layouts_free(&layouts);
    return !s.failed;
}
```

After the layouts, `select_module` fills the selector with the target description, the calling convention and the layouts, and selects each function with a body. An error of the layout or a missing pattern stops it with a message in `error`.

### Layout interface

The header `src/layout.h` declares the layout of the back end, and its opening comment refers to chapter 18 for the rules. That chapter lays out structs, unions, bitfields and the modifiers `packed` and `align(N)`. Instruction selection needs the size and alignment of each aggregate, and the offsets of its fields. The fields `bits`, `members` and `unaligned` serve the bitfields and the calling conventions of chapter 18.

```c
/* The layout of an aggregate on the target. */
struct layout {
    uint64_t size;
    uint64_t align;
    uint64_t *offsets;              /* the offset of each field */
    struct layout_bits *bits;       /* for each field, set for bitfields */
    struct layout_member *members;  /* recorded up to LAYOUT_MEMBER_LIMIT */
    size_t member_count;
    bool unaligned;                 /* a field lies off its alignment */
};
```

```c
/* Lay out every aggregate of m for target t. Returns false and writes a
   message to error for an array length below 1 or a layout that depends
   on itself. layouts_free releases the tables in either case. The tables
   cover the aggregates and symbolic values that m holds at this call. */
bool layouts_init(struct layouts *l, enum target t, const struct ir_module *m,
                  char *error, size_t error_size);
void layouts_free(struct layouts *l);

const struct layout *layout_agg(struct layouts *l, uint32_t agg);
uint64_t layout_size(struct layouts *l, struct ir_vtype v);
uint64_t layout_align(struct layouts *l, struct ir_vtype v);

/* Fold symbolic value sym to its bits on the target. Returns false after
   a division by zero. */
bool layout_fold(struct layouts *l, uint32_t sym, uint64_t *out);

/* Replace every symbolic operand of function f of the module with a
   constant, and set resolved when there was one. Returns false after a
   folding error. */
bool layout_resolve(struct layouts *l, struct ir_function *f, bool *resolved);
```

The function `layout_resolve` visits the result type, the parameter types, the temporaries and every operand of a function. The helper `resolve_type` gives `clong` and `cwchar` their width on the target, with the function `target_type` of chapter 11. The helper `resolve` replaces a symbolic operand with the integer that `layout_fold` computes from the layouts.

```c
/* Give a target-sized type its width on the target. */
static void resolve_type(const struct layouts *l, enum ir_type *type,
                         bool *resolved)
{
    enum ir_type fixed = target_type(l, *type);

    if (fixed != *type) {
        *type = fixed;
        *resolved = true;
    }
}

static bool resolve(struct layouts *l, struct ir_operand *o, bool *resolved)
{
    uint64_t value;

    resolve_type(l, &o->type, resolved);
    if (o->kind == IR_INT) {
        *o = ir_int_op(o->type, o->as.integer);
    }
    if (o->kind != IR_SYM) {
        return true;
    }
    if (!layout_fold(l, o->as.index, &value)) {
        return false;
    }
    *o = ir_int_op(o->type, value);
    *resolved = true;
    return true;
}
```

The fold wraps at the width of the value's type, as the same arithmetic does at run time. A division by zero is an error that names the target. After the operands, `layout_resolve` turns a conversion between two types of one width into a copy. A conversion from `c_long` to `int` is such a copy on Linux and macOS. The function also lowers the bitfield access of chapter 18 to plain loads and stores with shifts and masks.

### Optimizer after folding

The design comment in `select_module` gives the reason for the second run of the optimizer. A folded size reaches the same simplifications as a number. The function `f` of chapter 10 keeps an addition and a subtraction around `size_of main.H`, because the optimizer never folds a symbolic size.

```anti
struct H
{
    tag: u8,
    n: i32,
}

fn f() -> int
{
    let n = size_of(H);
    return n * 1 + 4 - 4;
}
```

The struct `H` has 8 bytes on all six targets. After folding, the optimizer computes `8 + 4 - 4`, and the selection for linux-arm64 loads the constant.

```text
main.f:
b0:
    mov x0, #8
    ret
```

The multiplication on `c_long` of chapter 10 folds at the width of each target.

```anti
fn f() -> c_long
{
    let x: c_long = 65536;
    return x * 70000;
}
```

For linux-x86_64 the product 4587520000 needs the 64-bit move `movabsq`.

```text
main.f:
b0:
    movabsq $4587520000, %rax
    ret
```

For windows-x86_64 the product wraps at 32 bits to 292552704, and the `c_long` result returns in `%eax`.

```text
main.f:
b0:
    movl $292552704, %eax
    ret
```

### Layout errors

A layout can fail on one target. The test file `tests/errors/zero_length.anti` declares an array whose length is computed from `size_of`.

```anti
struct Pair
{
    a: i32,
    b: i32,
}

fn main() -> int
{
    let x: [size_of(Pair) - 8]byte = [0; size_of(Pair) - 8];
    return x.len;
}
```

The command `antic -S --target linux-x86_64 -o zero_length.s tests/errors/zero_length.anti` stops in `layouts_init`, and `select_module` returns its message. The test `error_array_length` expects it.

```text
antic: the array length `size_of(Pair) - 8` is 0 on linux-x86_64, and an array length is at least 1
```

## Machine code

A machine instruction holds an opcode of the target and up to four operands. An operand is a virtual register, a physical register, an immediate value, a block label, a function symbol or a condition code. Each register operand carries its width in bits, which selects `x0` or `w0` on ARM64 and `%rax` or `%eax` on x86_64. The fields after `reg` serve operands of later chapters. Memory operands with a base, an index and a scale arrive in chapters 13 to 15. The address forms `pc_relative` and `got` belong to chapters 14 and 20, and `name` holds a helper such as `__chkstk`.

```c
struct mach_operand {
    enum mach_kind kind;
    uint8_t width;          /* The register or memory width in bits. */
    uint32_t reg;           /* MACH_VREG, MACH_PREG, the base of MACH_MEM. */
    bool base_vreg;         /* MACH_MEM: the base is a virtual register. */
    enum mach_index index;  /* MACH_MEM on ARM64. */
    int64_t value;          /* MACH_IMM, BLOCK, FUNC, COND, SLOT, offset. */
    uint32_t index_reg;     /* MACH_MEM with a scale: the index register. */
    bool index_vreg;        /* The index is a virtual register. */
    uint8_t scale;          /* MACH_MEM: 0 without an index, else 1 to 8. */
    bool pc_relative;       /* MACH_FUNC, MACH_GLOBAL as an address. */
    bool got;               /* MACH_FUNC: its entry in the GOT. */
    const char *name;       /* MACH_NAME. */
};
```

```c
/* Operands follow the destination-first order of the target's own
   instruction set. The x86_64 printer reverses them for AT&T syntax. */
struct mach_inst {
    uint16_t op;                    /* an opcode of the target */
    uint8_t count;
    struct mach_operand operands[MACH_MAX_OPERANDS];
    uint64_t uses;                  /* physical registers read implicitly */
    uint64_t defs;                  /* physical registers written implicitly */
};
```

The fields `uses` and `defs` name the physical registers that an instruction reads or writes without an operand for them. A call reads the argument registers and overwrites every register that the convention lets a callee change. The allocator of chapter 13 needs both sets to keep a value out of a register that a call destroys.

Every target describes its opcodes in a table with a name and a role for each operand. The role says whether the instruction reads the operand, writes it, or both. The memory operands of chapter 13 always read their base register.

```c
/* How an opcode treats each register operand. The base register of a
   memory operand is always read. */
enum mach_role { ROLE_USE = 1, ROLE_DEF = 2 };
```

## Selector

The shared part of the selector is `src/select.c`. It turns each IR temporary into the virtual register with the same number, so `%1` of the IR becomes `t1`. A register that selection adds, such as `t2` for the constant 6, takes the next number after the temporaries.

```c
static void select_function(struct selector *s)
{
    const struct ir_function *f = s->f;
    size_t b;
    size_t i;
    size_t k;
    uint32_t v;

    s->out->ir = f;
    for (v = 0; v < f->temp_count; v++) {
        mach_vreg_add(s->out, select_is_float(f->temps[v]));
    }
    s->out->block_count = f->block_count;
    s->out->blocks = calloc(f->block_count + 1, sizeof *s->out->blocks);
    if (s->out->blocks == NULL) {
        fputs("antic: out of memory\n", stderr);
        exit(70);
    }
    count_uses(s);
    s->b = &s->out->blocks[0];
    select_params(s);
    for (b = 0; b < f->block_count && !s->failed; b++) {
        const struct ir_block *block = f->blocks[b];
        s->block = b;
        s->b = &s->out->blocks[b];
        s->b->loop_depth = block->loop_depth;
        for (i = 0; i < block->count && !s->failed; i++) {
            const struct ir_inst *inst = &block->insts[i];
            const struct pattern *p;
            if (fuses(s, block, i)) {
                s->fused = inst;
                continue;
            }
            k = fold_address(s, block, i);
            if (k > 0) {
                i += k - 1;
                continue;
            }
            p = find_pattern(s, inst);
            if (p == NULL) {
                select_refuse(s, s->fused != NULL ? s->fused : inst);
                break;
            }
            p->emit(s, inst);
            s->fused = NULL;
            s->has_address = false;
        }
    }
    free(s->uses);
    s->uses = NULL;
}
```

The parameters come first. They arrive in the argument registers of the calling convention, and the selector moves each one into the virtual register of its temporary. The loop then selects every instruction of every block, in the order of the IR. The first loop gives each virtual register its class, which the float registers of chapter 17 need. The call of `fold_address` joins an address into the memory operand of the next load or store, which chapters 14 and 15 describe.

### Pattern tables

Each target has a table of patterns. A pattern names an IR operation, a match function and an emit function. The selector takes the first row whose operation matches and whose match function accepts the instruction, and calls its emit function. A row without a match function accepts every instruction of its operation, so the more specific rows of an operation come first.

```c
/* One row of a target's pattern table. The first row whose operation
   matches and whose match function accepts the instruction emits it. A
   NULL match function accepts every instruction of the operation. */
struct pattern {
    enum ir_op op;
    bool (*match)(const struct selector *s, const struct ir_inst *inst);
    void (*emit)(struct selector *s, const struct ir_inst *inst);
};
```

```c
static const struct pattern *find_pattern(const struct selector *s,
                                          const struct ir_inst *inst)
{
    const struct target_desc *t = s->target;
    size_t i;

    for (i = 0; i < t->pattern_count; i++) {
        if (t->patterns[i].op == inst->op &&
            (t->patterns[i].match == NULL || t->patterns[i].match(s, inst))) {
            return &t->patterns[i];
        }
    }
    return NULL;
}
```

Among the operations of this chapter, `copy`, `xor` and `branch` have two rows in the table of ARM64. The second `copy` row and the rows before `add` hold the floats of chapter 17. The rows after `ret` belong to chapter 13 for slots, loads, stores and `ptradd`, to chapter 18 for `memcopy` and to chapter 15 for the rest. The first `xor` row matches the negation of a `bool`, `x = xor x, 1`, and emits `eor` with the immediate 1. The first `branch` row matches a branch whose comparison the selector has fused, as described under Comparisons and branches below.

```c
static const struct pattern patterns[] = {
    {IR_COPY, match_copy, emit_copy},
    {IR_COPY, match_float, emit_float_copy},
    {IR_FADD, NULL, emit_float_binary},
    {IR_FSUB, NULL, emit_float_binary},
    {IR_FMUL, NULL, emit_float_binary},
    {IR_FDIV, NULL, emit_float_binary},
    {IR_FNEG, NULL, emit_float_neg},
    {IR_SITOF, NULL, emit_float_convert},
    {IR_UITOF, NULL, emit_float_convert},
    {IR_FTOSI, NULL, emit_float_convert},
    {IR_FTOUI, NULL, emit_float_convert},
    {IR_FEXT, NULL, emit_float_convert},
    {IR_FTRUNC, NULL, emit_float_convert},
    {IR_FEQ, NULL, emit_float_compare},
    {IR_FNE, NULL, emit_float_compare},
    {IR_FLT, NULL, emit_float_compare},
    {IR_FLE, NULL, emit_float_compare},
    {IR_FGT, NULL, emit_float_compare},
    {IR_FGE, NULL, emit_float_compare},
    {IR_ADD, match_arith, emit_add_sub},
    {IR_SUB, match_arith, emit_add_sub},
    {IR_MUL, match_arith, emit_binary},
    {IR_AND, match_arith, emit_binary},
    {IR_OR, match_arith, emit_binary},
    {IR_XOR, match_not_bool, emit_not_bool},
    {IR_XOR, match_arith, emit_binary},
    {IR_NEG, match_arith, emit_unary},
    {IR_NOT, match_arith, emit_unary},
    {IR_EQ, match_compare, emit_set},
    {IR_NE, match_compare, emit_set},
    {IR_SLT, match_compare, emit_set},
    {IR_SLE, match_compare, emit_set},
    {IR_SGT, match_compare, emit_set},
    {IR_SGE, match_compare, emit_set},
    {IR_ULT, match_compare, emit_set},
    {IR_ULE, match_compare, emit_set},
    {IR_UGT, match_compare, emit_set},
    {IR_UGE, match_compare, emit_set},
    {IR_JUMP, NULL, emit_jump},
    {IR_BRANCH, match_fused, emit_fused_branch},
    {IR_BRANCH, NULL, emit_branch},
    {IR_CALL, match_call, emit_call},
    {IR_RET, NULL, emit_ret},
    {IR_SLOT, NULL, emit_slot},
    {IR_LOAD, match_scalar, emit_load},
    {IR_STORE, match_scalar, emit_store},
    {IR_PTRADD, NULL, emit_add_sub},
    {IR_MEMCOPY, NULL, emit_memcopy},
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

The table of x86_64 in `src/x86_64.c` has the same operations in the same order, with its own match and emit functions. The two tables share the structure and the selector loop. The instructions they emit differ, because the two processors differ.

### Sizes and alignments

A pattern never reads a size from the IR, which holds none. It asks the selector through three functions, which read the layouts of the target.

```c
/* The layout of aggregate agg on the target, NULL for IR_NO_AGG. */
const struct layout *select_layout(const struct selector *s, uint32_t agg);
uint64_t select_size(const struct selector *s, struct ir_vtype v);
uint64_t select_align(const struct selector *s, struct ir_vtype v);
```

```c
const struct layout *select_layout(const struct selector *s, uint32_t agg)
{
    return agg == IR_NO_AGG ? NULL : layout_agg(s->layouts, agg);
}

uint64_t select_size(const struct selector *s, struct ir_vtype v)
{
    return layout_size(s->layouts, v);
}

uint64_t select_align(const struct selector *s, struct ir_vtype v)
{
    return layout_align(s->layouts, v);
}
```

The `slot` patterns of chapter 13 take the byte count and the alignment of a slot from `select_size` and `select_align`. The aggregate arguments and results of chapter 18 are classified from `select_layout`.

## ARM64 patterns

ARM64 instructions have three operands, as in `add t2, t0, t1`, so an IR instruction usually becomes one instruction. The limits lie in the immediates. The assembler llvm-mc 23.1.1 accepts `add x0, x0, #4095` and rejects `add x0, x0, #4097` with the message `expected compatible register, symbol or integer in range [0, 4095]`. The function `emit_add_sub` uses the immediate form for a constant that `fits_imm12` accepts. A negative constant turns `add` into `sub` and `sub` into `add`, as `sub t3, t1, #-7` becomes `add t3, t1, #7`. Chapter 15 extends `fits_imm12` to the shifted form, and the `ptradd` of chapter 13 takes the same function.

```c
/* A negative immediate turns add into sub and sub into add. */
static void emit_add_sub(struct selector *s, const struct ir_inst *inst)
{
    struct mach_operand ops[4];
    enum a64_op op = inst->op == IR_SUB ? A64_SUB : A64_ADD;
    enum a64_op other = inst->op == IR_SUB ? A64_ADD : A64_SUB;
    int64_t v = inst->b.kind == IR_INT
                    ? signed_value(inst->b.as.integer, bits(inst->b.type))
                    : 0;

    ops[0] = select_result(s, inst);
    ops[1] = select_reg(s, &inst->a);
    if (inst->b.kind == IR_INT && fits_imm12(v)) {
        emit_imm12(s, op, 2, ops, v);
    } else if (inst->b.kind == IR_INT && v < 0 && fits_imm12(-v)) {
        emit_imm12(s, other, 2, ops, -v);
    } else {
        emit3(s, op, ops[0], ops[1], select_reg(s, &inst->b));
    }
}
```

A constant in a register needs `mov`. llvm-mc accepts `mov x0, #65535` and `mov x0, #-2`, and rejects `mov x0, #65537` with the message `expected compatible register or logical immediate`. The function `load_into` therefore uses `mov` for a value that fits 16 bits, or whose bitwise inverse does. Any other value goes in as 16-bit pieces, with `movz` for the first piece and `movk` for each further one. The patterns reach it through `load`, and the prologue of chapter 15 calls it directly with a block. The unit test `test_select` loads 5000000000 as three pieces.

```c
/* An instruction holds a 16-bit immediate. mov takes a value that fits,
   or whose bitwise inverse fits. Other values go in as 16-bit pieces with
   movz and movk. */
static void load_into(struct mach_block *b, struct mach_operand dst,
                      uint64_t value)
{
    uint64_t mask = dst.width == 64 ? UINT64_MAX : UINT32_MAX;
    uint64_t v = value & mask;
    int64_t signed_v = dst.width == 64 ? (int64_t)v : (int32_t)(uint32_t)v;
    struct mach_operand ops[3];
    bool first = true;
    int shift;

    ops[0] = dst;
    if (v <= 0xffff || (~v & mask) <= 0xffff) {
        ops[1] = mach_imm(v <= 0xffff ? (int64_t)v : signed_v);
        append(b, A64_MOV, 2, ops);
        return;
    }
    for (shift = 0; shift < dst.width; shift += 16) {
        uint64_t piece = (v >> shift) & 0xffff;
        if (piece == 0) {
            continue;
        }
        ops[1] = mach_imm((int64_t)piece);
        ops[2] = mach_imm(shift);
        append(b, first ? A64_MOVZ : A64_MOVK, shift == 0 ? 2 : 3, ops);
        first = false;
    }
}
```

Values of 8, 16 and 32 bits live in the 32-bit registers `w0` to `w30`. Arithmetic on 8-bit and 16-bit values needs extensions that chapter 15 adds. This chapter selects `bool` values only in copies, negations and equality comparisons, where they hold 0 or 1.

## x86_64 patterns

The x86_64 back end writes AT&T syntax, which llvm-mc 23.1.1 reads without a directive. The source comes before the destination, a register has the prefix `%` and an immediate the prefix `$`. The mnemonic ends in a size suffix: `q` for 64 bits, `l` for 32, `w` for 16 and `b` for 8. The machine code keeps the destination first, like ARM64, and only the printer reverses the operands.

Most x86_64 instructions have two operands, and the first one is read and written. An IR instruction `r = add a, b` becomes `mov a, r` followed by `add b, r`. When `r` is `b` itself, as in `i = 10 - i` inside a loop, the `mov` would destroy `b`. A commutative operation then applies `a` directly to `r`. A subtraction first saves `b` in a new register.

```c
/* r = a op b becomes mov a, r and op b, r in two-operand form. When r is
   b itself, the mov would destroy b. A commutative operation then applies
   a to r, and another one saves b in a new register first. */
static void two_operand(struct selector *s, const struct ir_inst *inst,
                        enum x64_op op, bool commutative)
{
    struct mach_operand r = select_result(s, inst);
    struct mach_operand b;

    if (fits_imm(&inst->b, r.width)) {
        load_into(s, r, &inst->a);
        emit2(s, op, r, mach_imm(signed_value(inst->b.as.integer, r.width)));
        return;
    }
    if (inst->b.kind == IR_TEMP && inst->b.as.temp == inst->result) {
        if (commutative) {
            b = fits_imm(&inst->a, r.width)
                    ? mach_imm(signed_value(inst->a.as.integer, r.width))
                    : select_reg(s, &inst->a);
            emit2(s, op, r, b);
            return;
        }
        b = select_new_vreg(s, r.width);
        move(s, b, select_reg(s, &inst->b));
    } else {
        b = select_reg(s, &inst->b);
    }
    load_into(s, r, &inst->a);
    emit2(s, op, r, b);
}
```

An immediate of a 64-bit instruction holds 32 bits and is sign-extended. llvm-mc accepts `addq $2147483647, %rax` and rejects `addq $2147483648, %rax` with `invalid operand for instruction`. The function `fits_imm` tests that limit, and a larger constant goes into a register with `movabsq`, the move with a 64-bit immediate.

```c
/* An immediate of a 64-bit instruction is 32 bits, sign-extended. Smaller
   instructions take an immediate of their own width. */
static bool fits_imm(const struct ir_operand *o, uint8_t w)
{
    int64_t v = signed_value(o->as.integer, w);
    return o->kind == IR_INT && (w < 64 || (v >= INT32_MIN && v <= INT32_MAX));
}
```

## Comparisons and branches

A comparison in the IR computes a `bool`. Both processors compare with an instruction that sets flags, `cmp`, and then read the flags with a condition. When the only use of a comparison is the branch right after it, the selector writes no `bool` at all. The function `fuses` detects that case, and the branch pattern emits `cmp` followed by a conditional jump.

```c
/* A comparison whose only use is the branch right after it becomes flags
   and a conditional jump, without a value in a register. */
static bool fuses(const struct selector *s, const struct ir_block *b,
                  size_t i)
{
    const struct ir_inst *inst = &b->insts[i];
    const struct ir_inst *next = i + 1 < b->count ? &b->insts[i + 1] : NULL;

    return is_comparison(inst->op) && next != NULL &&
           next->op == IR_BRANCH && next->a.kind == IR_TEMP &&
           next->a.as.temp == inst->result && s->uses[inst->result] == 1 &&
           find_pattern(s, inst) != NULL;
}
```

Blocks stay in the order of the IR, and control falls from the end of one block into the next. A jump to the next block is left out. When the true block of a branch follows, the selector negates the condition and jumps to the false block. The loop of chapter 8, after the optimizer, is in `tests/dump/loop.anti`.

```anti
fn g(n: int) -> int
{
    let i = 0;
    while i < n do {
        i += 1;
    }
    if i > 3 {
        return 1;
    } else {
        return 0;
    }
}
```

For linux-arm64 the test `dump_select_loop_arm64` compares the output with `tests/dump/loop.arm64.sel`.

```text
loop.g:
b0:
    mov t0, x0
    mov t1, #0
b1:
    cmp t1, t0
    b.ge b3
b2:
    add t1, t1, #1
    b b1
b3:
    cmp t1, #3
    b.le b5
b4:
    mov x0, #1
    ret
b5:
    mov x0, #0
    ret
```

The branch of block `b1` on `i < n` goes to `b2`, which follows, so the selector writes `b.ge b3`, the jump when `i >= n`. The jump from `b0` to `b1` is gone, because `b1` follows. The same loop for macos-x86_64 is in `tests/dump/loop.x86_64.sel`.

```text
loop.g:
b0:
    movq %rdi, %t0
    movq $0, %t1
b1:
    cmpq %t0, %t1
    jge b3
b2:
    addq $1, %t1
    jmp b1
b3:
    cmpq $3, %t1
    jle b5
b4:
    movq $1, %rax
    ret
b5:
    movq $0, %rax
    ret
```

The x86_64 condition `ge` is `jge`, and `cmpq %t0, %t1` compares `t1` with `t0` in AT&T order. A comparison used as a value writes its result with `cset` on ARM64 and `setl` or another `set` instruction on x86_64.

## Calls and returns

A call moves its arguments into the argument registers of the convention, which `struct abi` lists per convention. The fields after `caller_saved` belong to register allocation in chapter 13, the frames of chapter 14 and the float registers of chapter 17. A constant argument goes directly into its register. After the call, the result moves from `x0` or `%rax` into the virtual register of the IR result. A return moves its value into the same register and emits `ret`.

```c
/* The registers of a calling convention, as physical register numbers of
   the target. The float registers have numbers above the integer ones. */
struct abi {
    const uint8_t *int_args;
    size_t int_arg_count;
    uint8_t int_result;
    uint64_t caller_saved;
    uint64_t callee_saved;
    const uint8_t *allocatable;         /* in the order of preference */
    size_t allocatable_count;
    uint8_t scratch[2];                 /* for spilled registers */
    uint8_t shadow_space;               /* bytes a caller reserves */
    bool probe_stack;                   /* frames of a page call a probe */
    const uint8_t *fp_args;
    size_t fp_arg_count;
    uint8_t fp_result;
    const uint8_t *fp_allocatable;
    size_t fp_allocatable_count;
    uint8_t fp_scratch[2];
    uint8_t fp_save_size;               /* bytes a callee saves of one */
};
```

The call on linux-x86_64 uses `%rdi` for its first argument, and the one on windows-x86_64 uses `%rcx`, as chapter 11 lists. The unit test `test_select` checks both.

## Limits of this chapter

The patterns handle integer and pointer values of 32 and 64 bits and `bool` values. They select arithmetic, bitwise operations and comparisons, jumps and branches, direct calls with register arguments, and returns. An instruction outside that set stops with a message that names the chapter that adds it.

| Construct | Message |
|---|---|
| `x / 3` on arm64 | ``instruction selection for `sdiv` on arm64 arrives in chapter 15`` |
| `x / 3` on x86_64 | ``instruction selection for `sdiv` on x86_64 arrives in chapter 14`` |
| An `f64` parameter | ``instruction selection for `f64` values arrives in chapter 17`` |
| A fifth parameter on windows-x86_64 | `parameters on the stack arrive in chapter 14` |

Division, shifts, conversions, memory access, stack arguments and variadic calls arrive with the complete patterns of chapters 14 and 15. Floating point follows in chapter 17 and structs in chapter 18.

## Tests

The selected code of this chapter is compared on all six targets, and every listing above is pinned. The build has zero warnings, and every `ctest` test passes.

## Next

[Chapter 13, Register allocation and stack frames]({{% relref "/programming/writing-a-compiler/13-register-allocation" %}}), adds liveness, a linear-scan allocator, spilling, caller- and callee-saved registers and frame layout. It also applies the rule that address-taken locals live in memory.
