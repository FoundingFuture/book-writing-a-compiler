#include "regalloc.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "select.h"

/* DESIGN: linear scan in the form of Poletto and Sarkar. Each virtual
   register gets one live interval without holes, from its first to its
   last point of liveness in the order of the blocks. Instruction k reads
   at position 2k and writes at 2k + 1, so a register read by an
   instruction can be the one it writes. Physical registers that the
   machine code names, or that a call overwrites, form fixed ranges that
   an interval must not overlap. */

enum { NONE = -1, PREG_LIMIT = 64 };

static void *allocate(size_t count, size_t size)
{
    void *p = calloc(count + 1, size);

    if (p == NULL) {
        fputs("antic: out of memory\n", stderr);
        exit(70);
    }
    return p;
}

/* Sets of virtual registers */

struct set {
    uint64_t *bits;
    size_t words;
};

static bool set_has(const struct set *s, uint32_t v)
{
    return (s->bits[v / 64] >> (v % 64)) & 1;
}

static void set_add(struct set *s, uint32_t v)
{
    s->bits[v / 64] |= (uint64_t)1 << (v % 64);
}

/* Liveness */

struct range {
    int from;
    int to;
};

struct fixed {
    struct range *ranges;
    size_t count;
    size_t capacity;
};

struct interval {
    uint32_t vreg;
    int start;
    int end;
    int hint_preg;
    int hint_vreg;
    int preg;               /* NONE when spilled */
    int slot;               /* the spill slot, NONE when in a register */
    bool fp;                /* in the float register class */
};

struct alloc {
    const struct target_desc *target;
    const struct abi *abi;
    struct mach_function *f;
    struct set *use;
    struct set *def;
    struct set *live_in;
    struct set *live_out;
    size_t words;
    struct interval *intervals;     /* indexed by virtual register */
    struct fixed fixed[PREG_LIMIT];
};

static bool is_reg(const struct mach_operand *o)
{
    return o->kind == MACH_VREG || o->kind == MACH_PREG;
}

/* Call visit for each register that inst reads, then for each register
   it writes. The base of a memory operand is read. */
static void each_register(const struct alloc *a, const struct mach_inst *inst,
                          void (*visit)(void *ctx, const struct mach_operand *o,
                                        bool write),
                          void *ctx)
{
    const struct mach_opcode *op = &a->target->opcodes[inst->op];
    struct mach_operand implicit;
    size_t i;
    int r;

    for (i = 0; i < inst->count; i++) {
        const struct mach_operand *o = &inst->operands[i];
        if (o->kind == MACH_MEM) {
            struct mach_operand base = *o;
            base.kind = o->base_vreg ? MACH_VREG : MACH_PREG;
            visit(ctx, &base, false);
            if (o->scale != 0) {
                base.kind = o->index_vreg ? MACH_VREG : MACH_PREG;
                base.reg = o->index_reg;
                visit(ctx, &base, false);
            }
        } else if (is_reg(o) && (op->roles[i] & ROLE_USE)) {
            visit(ctx, o, false);
        }
    }
    memset(&implicit, 0, sizeof implicit);
    implicit.kind = MACH_PREG;
    implicit.width = 64;
    for (r = 0; r < PREG_LIMIT; r++) {
        if ((inst->uses >> r) & 1) {
            implicit.reg = (uint32_t)r;
            visit(ctx, &implicit, false);
        }
    }
    for (i = 0; i < inst->count; i++) {
        const struct mach_operand *o = &inst->operands[i];
        if (is_reg(o) && (op->roles[i] & ROLE_DEF)) {
            visit(ctx, o, true);
        }
    }
    for (r = 0; r < PREG_LIMIT; r++) {
        if ((inst->defs >> r) & 1) {
            implicit.reg = (uint32_t)r;
            visit(ctx, &implicit, true);
        }
    }
}

struct block_ctx {
    struct alloc *a;
    size_t block;
};

static void gather_use_def(void *ctx, const struct mach_operand *o, bool write)
{
    struct block_ctx *c = ctx;
    struct set *use = &c->a->use[c->block];
    struct set *def = &c->a->def[c->block];

    if (o->kind != MACH_VREG) {
        return;
    }
    if (write) {
        set_add(def, o->reg);
    } else if (!set_has(def, o->reg)) {
        set_add(use, o->reg);
    }
}

/* The blocks that control can reach from block b. They are the targets
   of its jumps and branches, and the next block unless b ends with a jump
   or a return. */
static size_t successors(const struct alloc *a, size_t b, size_t out[3])
{
    const struct mach_block *block = &a->f->blocks[b];
    size_t n = 0;
    size_t i;
    size_t k;
    bool falls = true;

    for (i = 0; i < block->count; i++) {
        const struct mach_inst *inst = &block->insts[i];
        uint8_t flags = a->target->opcodes[inst->op].flags;
        for (k = 0; k < inst->count && (flags & (FLAG_JUMP | FLAG_BRANCH));
             k++) {
            if (inst->operands[k].kind == MACH_BLOCK && n < 2) {
                out[n++] = (size_t)inst->operands[k].value;
            }
        }
        if (i + 1 == block->count && (flags & (FLAG_JUMP | FLAG_RET))) {
            falls = false;
        }
    }
    if (falls && b + 1 < a->f->block_count) {
        out[n++] = b + 1;
    }
    return n;
}

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

/* Intervals and fixed ranges */

static void extend(struct interval *iv, int position)
{
    if (position < iv->start) {
        iv->start = position;
    }
    if (position > iv->end) {
        iv->end = position;
    }
}

struct scan_ctx {
    struct alloc *a;
    int position;           /* 2k for instruction k */
    int block_start;
    int current[PREG_LIMIT];/* the open fixed range of each register */
};

static void add_range(struct fixed *fx, int from, int to)
{
    if (fx->count == fx->capacity) {
        size_t capacity = fx->capacity == 0 ? 8 : fx->capacity * 2;
        struct range *ranges = realloc(fx->ranges, capacity * sizeof *ranges);
        if (ranges == NULL) {
            fputs("antic: out of memory\n", stderr);
            exit(70);
        }
        fx->ranges = ranges;
        fx->capacity = capacity;
    }
    fx->ranges[fx->count].from = from;
    fx->ranges[fx->count].to = to;
    fx->count++;
}

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

/* A move between two registers suggests the same register for both, so
   that the move disappears. */
static void record_hint(struct alloc *a, const struct mach_inst *inst)
{
    const struct mach_operand *dst = &inst->operands[0];
    const struct mach_operand *src = &inst->operands[1];

    if (!(a->target->opcodes[inst->op].flags & FLAG_MOVE) ||
        inst->count != 2 || !is_reg(dst) || !is_reg(src)) {
        return;
    }
    if (dst->kind == MACH_VREG && src->kind == MACH_PREG &&
        a->intervals[dst->reg].hint_preg == NONE) {
        a->intervals[dst->reg].hint_preg = (int)src->reg;
    } else if (dst->kind == MACH_PREG && src->kind == MACH_VREG &&
               a->intervals[src->reg].hint_preg == NONE) {
        a->intervals[src->reg].hint_preg = (int)dst->reg;
    } else if (dst->kind == MACH_VREG && src->kind == MACH_VREG) {
        if (a->intervals[dst->reg].hint_vreg == NONE) {
            a->intervals[dst->reg].hint_vreg = (int)src->reg;
        }
        if (a->intervals[src->reg].hint_vreg == NONE) {
            a->intervals[src->reg].hint_vreg = (int)dst->reg;
        }
    }
}

static void build_intervals(struct alloc *a)
{
    struct mach_function *f = a->f;
    struct scan_ctx ctx;
    size_t b;
    size_t i;
    uint32_t v;
    int k = 0;

    a->intervals = allocate(f->vreg_count, sizeof *a->intervals);
    for (v = 0; v < f->vreg_count; v++) {
        a->intervals[v].vreg = v;
        a->intervals[v].start = INT_MAX;
        a->intervals[v].end = NONE;
        a->intervals[v].hint_preg = NONE;
        a->intervals[v].hint_vreg = NONE;
        a->intervals[v].preg = NONE;
        a->intervals[v].slot = NONE;
        a->intervals[v].fp = f->fp[v];
    }
    ctx.a = a;
    for (b = 0; b < f->block_count; b++) {
        const struct mach_block *block = &f->blocks[b];
        int start = 2 * k;
        int end = 2 * (k + (int)block->count) - 1;
        if (block->count == 0) {
            end = start;
        }
        for (v = 0; v < f->vreg_count; v++) {
            if (set_has(&a->live_in[b], v)) {
                extend(&a->intervals[v], start);
            }
            if (set_has(&a->live_out[b], v)) {
                extend(&a->intervals[v], end);
            }
        }
        ctx.block_start = start;
        for (i = 0; i < PREG_LIMIT; i++) {
            ctx.current[i] = NONE;
        }
        for (i = 0; i < block->count; i++) {
            ctx.position = 2 * k++;
            each_register(a, &block->insts[i], scan_register, &ctx);
            record_hint(a, &block->insts[i]);
        }
    }
}

/* Linear scan */

static bool overlaps_fixed(const struct alloc *a, int preg,
                           const struct interval *iv)
{
    const struct fixed *fx = &a->fixed[preg];
    size_t i;

    for (i = 0; i < fx->count; i++) {
        if (fx->ranges[i].from <= iv->end && iv->start <= fx->ranges[i].to) {
            return true;
        }
    }
    return false;
}

static bool is_free(const struct alloc *a, struct interval **active,
                    size_t active_count, int preg, const struct interval *iv)
{
    size_t i;

    for (i = 0; i < active_count; i++) {
        if (active[i]->preg == preg) {
            return false;
        }
    }
    return !overlaps_fixed(a, preg, iv);
}

/* The registers that the allocator hands out for the class of iv, in the
   order of preference. */
static const uint8_t *class_registers(const struct alloc *a,
                                      const struct interval *iv, size_t *count)
{
    *count = iv->fp ? a->abi->fp_allocatable_count
                    : a->abi->allocatable_count;
    return iv->fp ? a->abi->fp_allocatable : a->abi->allocatable;
}

static bool is_allocatable(const struct alloc *a, const struct interval *iv,
                           int preg)
{
    size_t count;
    const uint8_t *regs = class_registers(a, iv, &count);
    size_t i;

    for (i = 0; i < count; i++) {
        if (regs[i] == preg) {
            return true;
        }
    }
    return false;
}

static int by_start(const void *x, const void *y)
{
    const struct interval *const *a = x;
    const struct interval *const *b = y;

    if ((*a)->start != (*b)->start) {
        return (*a)->start < (*b)->start ? -1 : 1;
    }
    return (*a)->vreg < (*b)->vreg ? -1 : (*a)->vreg > (*b)->vreg;
}

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

/* Rewriting */

struct rewrite {
    struct alloc *a;
    struct mach_block out;
    const struct frame *frame;
};

static void emit(struct mach_block *b, const struct mach_inst *inst)
{
    *mach_append(b) = *inst;
}

/* The scratch register of a spilled virtual register within one
   instruction. */
struct spill_map {
    uint32_t vreg[MACH_MAX_OPERANDS];
    uint8_t scratch[MACH_MAX_OPERANDS];
    size_t count;
    size_t loads[2];        /* per class: integer, float */
};

static int find_spill(const struct spill_map *map, uint32_t vreg)
{
    size_t i;

    for (i = 0; i < map->count; i++) {
        if (map->vreg[i] == vreg) {
            return (int)i;
        }
    }
    return NONE;
}

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

/* Replace the virtual registers of inst, drop a move of a register into
   itself and add the epilogue before a return. */
static void rewrite_inst(struct rewrite *rw, const struct mach_inst *inst)
{
    struct alloc *a = rw->a;
    const struct mach_opcode *op = &a->target->opcodes[inst->op];
    struct mach_inst copy = *inst;
    struct spill_map map;
    size_t i;
    int k;

    memset(&map, 0, sizeof map);
    for (i = 0; i < copy.count; i++) {
        struct mach_operand *o = &copy.operands[i];
        if (o->kind == MACH_MEM && o->base_vreg) {
            o->reg = place(rw, &map, o->reg, true);
            o->base_vreg = false;
        }
        if (o->kind == MACH_MEM && o->scale != 0 && o->index_vreg) {
            o->index_reg = place(rw, &map, o->index_reg, true);
            o->index_vreg = false;
        }
        if (o->kind == MACH_VREG && (op->roles[i] & ROLE_USE)) {
            o->reg = place(rw, &map, o->reg, true);
            o->kind = MACH_PREG;
        }
    }
    for (i = 0; i < copy.count; i++) {
        struct mach_operand *o = &copy.operands[i];
        if (o->kind == MACH_VREG) {
            o->reg = place(rw, &map, o->reg, false);
            o->kind = MACH_PREG;
        }
    }
    if ((op->flags & FLAG_MOVE) && copy.count == 2 &&
        copy.operands[0].kind == MACH_PREG &&
        copy.operands[1].kind == MACH_PREG &&
        copy.operands[0].reg == copy.operands[1].reg) {
        return;
    }
    if (op->flags & FLAG_RET) {
        a->target->epilogue(&rw->out, rw->frame);
    }
    if (a->target->expand != NULL) {
        a->target->expand(&rw->out, &copy);
    } else {
        emit(&rw->out, &copy);
    }
    for (i = 0; i < inst->count; i++) {
        const struct mach_operand *o = &inst->operands[i];
        if (o->kind == MACH_VREG && (op->roles[i] & ROLE_DEF) &&
            a->intervals[o->reg].slot != NONE) {
            k = find_spill(&map, o->reg);
            a->target->store_spill(
                &rw->out, map.scratch[k],
                a->f->slots[a->intervals[o->reg].slot].offset);
        }
    }
}

static uint64_t align_up(uint64_t value, uint64_t align)
{
    return (value + align - 1) / align * align;
}

/* Slots from the bottom of the frame up, above the space a call needs,
   and the saved callee-saved registers at the top. */
static bool is_fp_register(const struct alloc *a, uint8_t preg)
{
    size_t i;

    for (i = 0; i < a->abi->fp_allocatable_count; i++) {
        if (a->abi->fp_allocatable[i] == preg) {
            return true;
        }
    }
    return false;
}

/* The bytes from the top of the frame to the end of the save of register
   preg. The saves above it take the first used bytes. A 16-byte save
   starts at a multiple of 16, which UWOP_SAVE_XMM128 of the Windows x64
   unwind data requires. */
static uint64_t save_end(const struct alloc *a, uint8_t preg, uint64_t used)
{
    uint64_t size = is_fp_register(a, preg) ? a->abi->fp_save_size : 8;

    return align_up(used, size) + size;
}

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

bool regalloc_function(enum target t, struct mach_function *f, char *error,
                       size_t error_size)
{
    struct alloc a;
    struct frame frame;
    struct rewrite rw;
    size_t b;
    size_t i;
    size_t k;
    bool ok;

    memset(&a, 0, sizeof a);
    a.target = target_desc(t);
    a.abi = a.target->abi(target_info(t)->convention);
    a.f = f;
    compute_liveness(&a);
    build_intervals(&a);
    linear_scan(&a);
    layout_frame(&a, &frame);
    frame.convention = target_info(t)->convention;
    frame.unwind = target_info(t)->format == FORMAT_COFF;
    f->unwind = frame.unwind && frame.needed;
    ok = a.target->frame_limit == 0 || frame.size <= a.target->frame_limit;
    if (!ok) {
        snprintf(error, error_size, "the stack frame of `%s.%s` needs %llu "
                 "bytes, and an %s frame holds at most %llu", f->ir->module,
                 f->ir->name, (unsigned long long)frame.size, a.target->name,
                 (unsigned long long)a.target->frame_limit);
    }
    for (b = 0; ok && b < f->block_count; b++) {
        memset(&rw, 0, sizeof rw);
        rw.a = &a;
        rw.frame = &frame;
        if (b == 0) {
            a.target->prologue(&rw.out, &frame);
        }
        for (i = 0; i < f->blocks[b].count; i++) {
            struct mach_inst *inst = &f->blocks[b].insts[i];
            for (k = 0; k < inst->count; k++) {
                if (inst->operands[k].kind == MACH_SLOT) {
                    a.target->resolve_slot(
                        &inst->operands[k],
                        f->slots[inst->operands[k].value].offset);
                }
            }
            rewrite_inst(&rw, inst);
        }
        free(f->blocks[b].insts);
        f->blocks[b] = rw.out;
    }
    for (b = 0; b < f->block_count; b++) {
        free(a.use[b].bits);
        free(a.def[b].bits);
        free(a.live_in[b].bits);
        free(a.live_out[b].bits);
    }
    for (i = 0; i < PREG_LIMIT; i++) {
        free(a.fixed[i].ranges);
    }
    free(a.use);
    free(a.def);
    free(a.live_in);
    free(a.live_out);
    free(a.intervals);
    return ok;
}
