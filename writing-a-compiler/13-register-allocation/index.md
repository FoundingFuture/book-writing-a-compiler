---
title: "Register allocation and stack frames"
description: "How antic gives every virtual register a physical register with linear scan, spills to the stack, saves callee-saved registers and lays out frames."
summary: "Liveness, a linear-scan allocator, spilling, caller- and callee-saved registers and frame layout. The rule that address-taken locals live in memory."
date: 2026-09-13T22:32:00+02:00
lastmod: 2026-09-15T03:48:32+02:00
draft: false
weight: 130
tags: [compilers, assembly]
keywords: [register allocation, linear scan, liveness analysis, live intervals, spilling, callee-saved registers, stack frame, prologue]
---

## Previously

[Chapter 12, Instruction selection]({{% relref "/programming/writing-a-compiler/12-instruction-selection" %}}), turns IR instructions into target instructions on virtual registers. Before selection, the back end lays out the types of the program for its target and folds the symbolic values. The selector is table-driven, shared in structure by both back ends, with separate patterns per architecture.

## Physical registers for virtual ones

The machine code of chapter 12 writes every value into a virtual register, and a processor has a fixed set of registers. Register allocation replaces each virtual register with a physical one. Two values may share a register when no instruction needs both at the same time. A value that finds no free register moves to the stack, and the function gets a stack frame that holds it.

The option `--dump-alloc` prints the machine code after register allocation. For `main.anti` of chapter 1 on macos-arm64 the test `dump_alloc_arm64` compares the output with `tests/dump/main.arm64.alloc`.

```text
main.scale:
b0:
    mov x9, #6
    mul x0, x0, x9
    ret
main.main:
b0:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    mov x0, #7
    bl main.scale
    ldp x29, x30, [sp], #16
    ret
```

These are the instructions of the assembly listing in chapter 1. The virtual register `t0` of `scale`, the parameter, got `x0`, where the argument arrives, so the move `mov t0, x0` disappeared. The product `t1` got `x0` too, where the result leaves. The constant `t2` got `x9`. The function `main` calls `scale`, so it saves its return address in a frame record.

## Liveness

A virtual register is live at a point of the program when some path from that point reads its value before it is written again. Two registers interfere when both are live at the same point, and then they need different physical registers. Liveness analysis computes, for every block, the virtual registers that are live on entry and on exit.

Each block has two sets. The set `use` holds the registers that the block reads before it writes them, and `def` the registers it writes. A register is live on exit from a block when it is live on entry to a successor. It is live on entry when the block uses it, or when it is live on exit and the block does not write it. The function `compute_liveness` applies both rules to all blocks until no set changes.

```c
static void compute_liveness(struct alloc *a)
{
    size_t n = a->f->block_count;
    size_t b;
    size_t i;
    size_t w;
    bool changed = true;

    a->words = a->f->vreg_count / 64 + 1;
    a->use = allocate(n, sizeof *a->use);
    a->def = allocate(n, sizeof *a->def);
    a->live_in = allocate(n, sizeof *a->live_in);
    a->live_out = allocate(n, sizeof *a->live_out);
    for (b = 0; b < n; b++) {
        struct block_ctx ctx;
        a->use[b].bits = allocate(a->words, sizeof(uint64_t));
        a->def[b].bits = allocate(a->words, sizeof(uint64_t));
        a->live_in[b].bits = allocate(a->words, sizeof(uint64_t));
        a->live_out[b].bits = allocate(a->words, sizeof(uint64_t));
        ctx.a = a;
        ctx.block = b;
        for (i = 0; i < a->f->blocks[b].count; i++) {
            each_register(a, &a->f->blocks[b].insts[i], gather_use_def, &ctx);
        }
    }
    /* live_out(b) is the union of live_in over the successors, and
       live_in(b) is use(b) plus live_out(b) without def(b). */
    while (changed) {
        changed = false;
        for (b = n; b-- > 0;) {
            size_t succ[3];
            size_t count = successors(a, b, succ);
            for (w = 0; w < a->words; w++) {
                uint64_t out = 0;
                uint64_t in;
                for (i = 0; i < count; i++) {
                    out |= a->live_in[succ[i]].bits[w];
                }
                in = a->use[b].bits[w] | (out & ~a->def[b].bits[w]);
                if (out != a->live_out[b].bits[w] ||
                    in != a->live_in[b].bits[w]) {
                    a->live_out[b].bits[w] = out;
                    a->live_in[b].bits[w] = in;
                    changed = true;
                }
            }
        }
    }
}
```

The successors of a block are the targets of its jumps and branches. The next block is a successor as well, unless the block ends with a jump or a return. The opcode tables of the targets mark those instructions with the flags `FLAG_JUMP`, `FLAG_BRANCH` and `FLAG_RET`.

### Live intervals

Linear scan is the register allocation algorithm of Poletto and Sarkar[^1]. It numbers the instructions of a function in one order and approximates the liveness of a register by one interval. The interval runs from the first point where the register is live to the last. The compiler antic numbers the instructions in the order of the blocks. Instruction k reads its operands at position 2k and writes its results at position 2k + 1. The interval of `t0` in `scale` therefore starts at 1, where `mov t0, x0` writes it, and ends at 4, where `mul` reads it.

The interval of a register that is live on entry to a block includes the start of the block. The interval of a register that is live on exit includes its end. Inside a loop, a register that is live at the jump back therefore covers the whole loop body. The interval may include points where the register is not live, which costs registers but never correctness[^1].

### Fixed ranges

Some instructions name physical registers directly: the moves into the argument registers, a call, a return. A call also overwrites every caller-saved register, which the field `defs` of the call records. The allocator turns each such register into fixed ranges, from each write to the last read after it. The function `scan_register` builds the intervals and the fixed ranges in one pass.

```c
static void scan_register(void *ctx, const struct mach_operand *o, bool write)
{
    struct scan_ctx *c = ctx;
    struct alloc *a = c->a;
    int position = c->position + (write ? 1 : 0);

    if (o->kind == MACH_VREG) {
        extend(&a->intervals[o->reg], position);
        return;
    }
    if (o->reg >= PREG_LIMIT) {
        return;
    }
    if (write) {
        add_range(&a->fixed[o->reg], position, position);
        c->current[o->reg] = (int)a->fixed[o->reg].count - 1;
    } else if (c->current[o->reg] == NONE) {
        add_range(&a->fixed[o->reg], c->block_start, position);
        c->current[o->reg] = (int)a->fixed[o->reg].count - 1;
    } else {
        a->fixed[o->reg].ranges[c->current[o->reg]].to = position;
    }
}
```

A virtual register may take a physical register only when their ranges do not overlap. The parameter `x0` of `scale` has the fixed range from the start of the function to position 0, where `mov t0, x0` reads it. The interval of `t0` starts at 1, so `t0` may take `x0`.

## Linear scan

The allocator sorts the intervals by their start and walks through them once[^1]. The list `active` holds the intervals that are already allocated and still live. Before each new interval, every active interval that ends before the new one starts gives its register back.

The function `choose` picks a register for an interval. A move between two registers suggests the same register for both, a hint. The move `mov t0, x0` gives `t0` the hint `x0`, and `mov x0, t1` gives `t1` the same hint. When a register is taken by an active interval or a fixed range overlaps, the allocator tries the other registers in the order of the table `allocatable`.

```c
static int choose(const struct alloc *a, struct interval **active,
                  size_t active_count, const struct interval *iv)
{
    int hint = iv->hint_preg;
    size_t count;
    const uint8_t *regs = class_registers(a, iv, &count);
    size_t i;

    if (hint == NONE && iv->hint_vreg != NONE) {
        hint = a->intervals[iv->hint_vreg].preg;
    }
    if (hint != NONE && is_allocatable(a, iv, hint) &&
        is_free(a, active, active_count, hint, iv)) {
        return hint;
    }
    for (i = 0; i < count; i++) {
        if (is_free(a, active, active_count, regs[i], iv)) {
            return regs[i];
        }
    }
    return NONE;
}
```

The function `class_registers` returns the table `allocatable` for an integer interval. Chapter 17 adds a second table for the float registers, and `class_registers` picks the table of the interval's class.

The order prefers registers that no convention gives a role, then the argument registers, and the callee-saved registers last. On ARM64, `x16` and `x17` stay out of the list for spilled values, and `x18` stays out, because Apple and Windows reserve it.

```c
/* DESIGN: allocation prefers x9 to x15, which no convention gives a
   role, then the argument registers and the callee-saved ones last. x16
   and x17 are the scratch registers of spilled values. x18 is never
   allocated, because Apple and Windows reserve it. x29 holds the frame
   record and x30 the return address. */
static const uint8_t allocatable[] = {
    X9, X10, X11, X12, X13, X14, X15, X0, X1, X2, X3, X4, X5, X6, X7, X8,
    X19, X20, X21, X22, X23, X24, X25, X26, X27, X28
};
```

On x86_64 the order starts with `rax`, `rcx` and `rdx`. The registers `r10` and `r11` are the scratch registers, and `rbp` holds the frame pointer.

## Caller- and callee-saved registers

A call overwrites every caller-saved register, so a value that is live across a call cannot stay in one. The fixed ranges of the call exclude those registers, and only a callee-saved register remains. The function that uses a callee-saved register must restore it before it returns, so the allocator records every callee-saved register it hands out. The test `dump_alloc_twice_arm64` shows a parameter that two calls of `putchar` must not destroy.

```anti
extern fn putchar(c: i32) -> i32;

fn twice(c: i32) -> i32
{
    putchar(c);
    putchar(c);
    return c;
}
```

```text
twice.twice:
b0:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    sub sp, sp, #16
    str x19, [sp, #8]
    mov w19, w0
    mov w0, w19
    bl putchar
    mov w0, w19
    bl putchar
    mov w0, w19
    ldr x19, [sp, #8]
    mov sp, x29
    ldp x29, x30, [sp], #16
    ret
```

The parameter `c` lives in `w19` across both calls. The prologue saves `x19` in the frame and the epilogue restores it. The result of the first `putchar` got `x0` from its hint, where the call left it, so its move vanished.

## Spilling

When every register is taken, one interval must move to the stack. Poletto and Sarkar spill "the interval that ends last, furthest away from the current point"[^1]. The allocator of antic chooses among the new interval and the active intervals whose register the new interval could take without a fixed conflict.

```c
/* When no register is free, the interval that ends last gives up its
   register. That is the new interval itself, or an active one whose
   register the new interval can take. */
static void linear_scan(struct alloc *a)
{
    struct mach_function *f = a->f;
    struct interval **order = allocate(f->vreg_count, sizeof *order);
    struct interval **active = allocate(f->vreg_count, sizeof *active);
    size_t count = 0;
    size_t active_count = 0;
    size_t i;
    size_t j;
    uint32_t v;

    for (v = 0; v < f->vreg_count; v++) {
        if (a->intervals[v].end != NONE) {
            order[count++] = &a->intervals[v];
        }
    }
    qsort(order, count, sizeof *order, by_start);
    for (i = 0; i < count; i++) {
        struct interval *cur = order[i];
        int preg;
        for (j = 0; j < active_count;) {
            if (active[j]->end < cur->start) {
                active[j] = active[--active_count];
            } else {
                j++;
            }
        }
        preg = choose(a, active, active_count, cur);
        if (preg == NONE) {
            struct interval *victim = NULL;
            for (j = 0; j < active_count; j++) {
                if (active[j]->end > cur->end && active[j]->fp == cur->fp &&
                    !overlaps_fixed(a, active[j]->preg, cur) &&
                    (victim == NULL || active[j]->end > victim->end)) {
                    victim = active[j];
                }
            }
            if (victim == NULL) {
                cur->slot = (int)mach_slot_add(f, 8, 8);
                continue;
            }
            preg = victim->preg;
            victim->preg = NONE;
            victim->slot = (int)mach_slot_add(f, 8, 8);
            for (j = 0; j < active_count; j++) {
                if (active[j] == victim) {
                    active[j] = active[--active_count];
                    break;
                }
            }
        }
        cur->preg = preg;
        active[active_count++] = cur;
    }
    free(order);
    free(active);
}
```

The condition `active[j]->fp == cur->fp` takes a register only from an interval of the same class. Integer intervals are the only class in this chapter, and chapter 17 adds the float class.

A spilled register gets a stack slot of 8 bytes. Every instruction that reads it first loads the slot into a scratch register, and every instruction that writes it stores the scratch register back. An instruction of this chapter reads at most two registers, so two scratch registers suffice: `x16` and `x17` on ARM64, `r10` and `r11` on x86_64. The function `place` returns the physical register of a virtual register within one instruction. A float register of chapter 17 takes its scratch register from `fp_scratch`, and each class counts its loads in `loads`.

```c
/* The physical register of virtual register vreg within one instruction.
   A spilled register that the instruction reads is loaded into a scratch
   register first. A spilled register that it only writes takes the first
   scratch register, which the instruction writes after it has read its
   operands. */
static uint32_t place(struct rewrite *rw, struct spill_map *map, uint32_t vreg,
                      bool reads)
{
    struct alloc *a = rw->a;
    const struct interval *iv = &a->intervals[vreg];
    int k;

    if (iv->slot == NONE) {
        return (uint32_t)iv->preg;
    }
    k = find_spill(map, vreg);
    if (k == NONE) {
        const uint8_t *scratch = iv->fp ? a->abi->fp_scratch : a->abi->scratch;
        k = (int)map->count++;
        map->vreg[k] = vreg;
        map->scratch[k] = scratch[reads ? map->loads[iv->fp] : 0];
        if (reads) {
            a->target->load_spill(&rw->out, map->scratch[k],
                                  a->f->slots[iv->slot].offset);
            map->loads[iv->fp]++;
        }
    }
    return map->scratch[k];
}
```

The test `dump_alloc_spill_x86_64` keeps seven values across a call on linux-x86_64, which has five callee-saved registers.

```anti
extern fn tick() -> int;

fn seven(a: int) -> int
{
    let v1 = a + 1;
    let v2 = a + 2;
    let v3 = a + 3;
    let v4 = a + 4;
    let v5 = a + 5;
    let v6 = a + 6;
    let v7 = a + 7;
    tick();
    return v1 + v2 + v3 + v4 + v5 + v6 + v7;
}
```

```text
spill7.seven:
b0:
    pushq %rbp
    movq %rsp, %rbp
    subq $64, %rsp
    movq %rbx, 56(%rsp)
    movq %r12, 48(%rsp)
    movq %r13, 40(%rsp)
    movq %r14, 32(%rsp)
    movq %r15, 24(%rsp)
    movq %rdi, %rbx
    addq $1, %rbx
    movq %rdi, %r12
    addq $2, %r12
    movq %rdi, %r13
    addq $3, %r13
    movq %rdi, %r14
    addq $4, %r14
    movq %rdi, %r15
    addq $5, %r15
    movq %rdi, %r10
    movq %r10, (%rsp)
    movq (%rsp), %r10
    addq $6, %r10
    movq %r10, (%rsp)
    movq %rdi, %r10
    movq %r10, 8(%rsp)
    movq 8(%rsp), %r10
    addq $7, %r10
    movq %r10, 8(%rsp)
    call tick
    addq %r12, %rbx
    addq %r13, %rbx
    addq %r14, %rbx
    addq %r15, %rbx
    movq (%rsp), %r10
    addq %r10, %rbx
    movq %rbx, %rax
    movq 8(%rsp), %r10
    addq %r10, %rax
    movq 56(%rsp), %rbx
    movq 48(%rsp), %r12
    movq 40(%rsp), %r13
    movq 32(%rsp), %r14
    movq 24(%rsp), %r15
    movq %rbp, %rsp
    popq %rbp
    ret
```

The values `v1` to `v5` take `rbx` and `r12` to `r15`. The values `v6` and `v7` end last and live in the slots at `(%rsp)` and `8(%rsp)`, reached through `r10`. The sum reuses `rbx` as soon as `v1` is dead. The test `dump_alloc_spill_arm64` does the same with twelve values on macos-arm64, where `x19` to `x28` hold ten.

## Stack frames

A function needs a stack frame when it calls another function or uses the stack. The function `layout_frame` places the slots at the bottom of the frame and the saved callee-saved registers at the top. The size is a multiple of 16, the stack alignment that all three calling conventions of chapter 11 require. The slots start above `f->outgoing`, the bytes that the calls of the function need at the bottom of the frame. Chapters 14 and 15 set it from the stack arguments of each call, and on windows-x86_64 it holds the 32 bytes of the shadow store.

```c
static void layout_frame(struct alloc *a, struct frame *frame)
{
    struct mach_function *f = a->f;
    uint64_t saved_bytes = 0;
    uint64_t offset = 0;
    uint64_t used = 0;
    bool calls = false;
    size_t b;
    size_t i;
    uint32_t v;

    for (b = 0; b < f->block_count; b++) {
        for (i = 0; i < f->blocks[b].count; i++) {
            calls = calls ||
                    (a->target->opcodes[f->blocks[b].insts[i].op].flags &
                     FLAG_CALL);
        }
    }
    for (v = 0; v < f->vreg_count; v++) {
        if (a->intervals[v].preg != NONE) {
            used |= (uint64_t)1 << a->intervals[v].preg;
        }
    }
    memset(frame, 0, sizeof *frame);
    for (i = 0; i < PREG_LIMIT; i++) {
        if (((used & a->abi->callee_saved) >> i) & 1) {
            frame->saved[frame->saved_count++] = (uint8_t)i;
        }
    }
    offset = f->outgoing;
    for (i = 0; i < f->slot_count; i++) {
        offset = align_up(offset, f->slots[i].align);
        f->slots[i].offset = (int64_t)offset;
        offset += f->slots[i].size;
    }
    for (i = 0; i < frame->saved_count; i++) {
        saved_bytes = save_end(a, frame->saved[i], saved_bytes);
    }
    frame->size = align_up(offset + saved_bytes, 16);
    saved_bytes = 0;
    for (i = 0; i < frame->saved_count; i++) {
        saved_bytes = save_end(a, frame->saved[i], saved_bytes);
        frame->saved_offset[i] = (int64_t)(frame->size - saved_bytes);
    }
    frame->needed = calls || frame->size > 0 || f->stack_params;
    frame->probe = a->abi->probe_stack && frame->size >= 4096;
}
```

A float callee-saved register of chapter 17 takes `fp_save_size` bytes in the frame, and `saved_offset` records the place of each saved register. The function `save_end` returns the end of each save below the top of the frame, and chapter 16 explains why a 16-byte save starts at a multiple of 16. A function that reads stack parameters needs a frame too. The flag `probe` marks a frame of 4096 bytes or more on a target whose convention probes the stack, which chapters 14 and 15 add for Windows.

On ARM64 the prologue stores the frame pointer `x29` and the return address `x30` as a frame record and points `x29` at it. It then moves `sp` down by the frame size and stores the callee-saved registers. The epilogue before each `ret` undoes those steps in reverse order.

```c
/* The frame record of x29 and x30 sits above the frame, and x29 points to
   it. Callee-saved registers take the top of the frame, slots its
   bottom.
   DESIGN: Windows unwinding maps each prologue instruction to one unwind
   code, and a save code holds an offset from sp of at most 504. With
   unwind data the prologue therefore allocates the save area first, saves
   into it and points x29 at the frame record. The rest of the frame
   follows in the body, which x29 allows, as Microsoft's ARM64 exception
   handling page states for a dedicated frame pointer. */
static void prologue(struct mach_block *b, const struct frame *frame)
{
    struct mach_operand ops[3];
    uint64_t area = save_area(frame);
    size_t i;

    if (!frame->needed) {
        return;
    }
    ops[0] = mach_preg(X29, 64);
    ops[1] = mach_preg(X30, 64);
    ops[2] = stack(-16, INDEX_PRE);
    append(b, A64_STP, 3, ops);
    ops[0] = mach_imm(16);
    unwind(b, frame, A64_SEH_SAVE_FPLR_X, 1, ops);
    ops[0] = mach_preg(X29, 64);
    ops[1] = mach_preg(SP, 64);
    if (!frame->unwind) {
        append(b, A64_MOV, 2, ops);
        if (frame->size > 0) {
            allocate_frame(b, frame, frame->size);
        }
        for (i = 0; i < frame->saved_count; i++) {
            store_spill(b, frame->saved[i], frame->saved_offset[i]);
        }
        return;
    }
    if (area == 0) {
        append(b, A64_MOV, 2, ops);
        unwind(b, frame, A64_SEH_SET_FP, 0, ops);
    } else {
        ops[0] = mach_preg(SP, 64);
        append_imm12(b, A64_SUB, 2, ops, (int64_t)area);
        ops[0] = mach_imm((int64_t)area);
        unwind(b, frame, A64_SEH_STACKALLOC, 1, ops);
        for (i = 0; i < frame->saved_count; i++) {
            int64_t offset = frame->saved_offset[i] -
                             (int64_t)(frame->size - area);
            store_spill(b, frame->saved[i], offset);
            unwind_save(b, frame, frame->saved[i], offset);
        }
        ops[0] = mach_preg(X29, 64);
        ops[1] = mach_preg(SP, 64);
        append_imm12(b, A64_ADD, 2, ops, (int64_t)area);
        ops[0] = mach_imm((int64_t)area);
        unwind(b, frame, A64_SEH_ADD_FP, 1, ops);
    }
    unwind(b, frame, A64_SEH_ENDPROLOGUE, 0, ops);
    if (frame->size > area) {
        allocate_frame(b, frame, frame->size - area);
    }
}
```

The function `allocate_frame` moves `sp` down by a part of the frame. Chapter 15 gives it the frames whose size no 12-bit immediate holds and the stack probe of Windows ARM64. The flag `unwind` marks a frame on Windows, whose prologue carries unwind directives. That prologue allocates the save area before the saves and points `x29` at the frame record after them, the order that chapter 16 derives. Without the flag, `unwind` adds nothing.

On x86_64 the prologue pushes `rbp`, copies `rsp` into `rbp` and subtracts the frame size from `rsp`. The `call` instruction has pushed 8 bytes of return address and the `push` another 8. With a frame size that is a multiple of 16, `rsp` is a multiple of 16 at the next call, as chapter 11 requires.

| Offset from `sp` or `rsp` | Contents |
|---|---|
| From 0, or from 32 on windows-x86_64 | Stack slots of the IR and spilled registers |
| Up to the frame size | Saved callee-saved registers, 8 bytes each |
| At the frame size | The frame record, or the saved `rbp` |

The offsets of this chapter address the frame with instructions whose immediates have limits. A frame larger than 4095 bytes stops with ``stack frames over 4095 bytes arrive in chapter 15`` on ARM64 and the same message with chapter 14 on x86_64.

## Address-taken locals

Chapter 8 keeps a local variable whose address the program takes in a stack slot of the IR. In this chapter the `slot` instruction becomes an address in the frame, and every use of the variable goes through `load` and `store`. The patterns for `slot`, `load`, `store` and `ptradd` join the pattern tables of chapter 12.

The instruction `slot` names the type of the value in the slot. For the variable `x` of the function `cell` below, `--dump-opt` prints `%2 = slot i64`. The IR has no sizes. The pattern takes the size and the alignment of the type on the target from the layout of chapter 12, through `select_size` and `select_align`. It adds a slot to the machine function with both values, and frame layout gives the slot its offset.

```c
/* A slot is an offset from sp that frame layout fills in. */
static void emit_slot(struct selector *s, const struct ir_inst *inst)
{
    struct mach_operand slot = mach_imm(mach_slot_add(s->out, select_size(s, inst->of),
                                                      select_align(s, inst->of)));

    slot.kind = MACH_SLOT;
    emit3(s, A64_ADD, select_result(s, inst), mach_preg(SP, 64), slot);
}
```

On ARM64 the address of the slot is `add` of an offset to `sp`. The x86_64 pattern writes `lea` of a memory operand on `rsp`. The test `dump_alloc_cell_arm64` shows a variable `x` that the program changes through a pointer.

```anti
fn cell(a: int, b: int) -> int
{
    let x = a;
    let p = &x;
    *p = *p + b;
    return x;
}
```

```text
cell.cell:
b0:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    sub sp, sp, #16
    add x9, sp, #0
    str x0, [x9]
    ldr x10, [x9]
    add x10, x10, x1
    str x10, [x9]
    ldr x0, [x9]
    mov sp, x29
    ldp x29, x30, [sp], #16
    ret
```

The slot of `x` takes the 8 bytes of an `i64` at offset 0 from `sp`, and the frame rounds them up to 16 bytes. The register `x9` holds the address of the slot. The stores and loads through `x9` stay in the code, because a store through a pointer may change the variable, as chapter 10 assumes.

## Tests

The unit tests in `tests/unit/test_regalloc.c` compile programs through register allocation and compare the result on two ARM64 and two x86_64 targets. They cover the function `scale` and a loop. Two further programs keep a value across calls and change an address-taken local. The dump tests pin the listings of this chapter, including the two spill examples. The build has zero warnings, and every `ctest` test passes.

## Next

[Chapter 14, The x86_64 back end]({{% relref "/programming/writing-a-compiler/14-x86-64-back-end" %}}), gives the instruction patterns, addressing modes, condition codes and calls under System V and Windows x64. It shows the emitted assembly for the test programs.

## References

[^1]: M. Poletto and V. Sarkar, *Linear Scan Register Allocation*, ACM TOPLAS 21(5), 1999, sections 3 and 4, https://web.cs.ucla.edu/~palsberg/course/cs132/linearscan.pdf
