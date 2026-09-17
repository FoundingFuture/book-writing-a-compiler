#include "optimize.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* DESIGN: the passes rely on three properties and nothing else.
   1. The IR passes ir_verify, including its check that every path to a
      use of a temporary passes a definition of it.
   2. Integer operations wrap, and chapter 2 leaves division by zero,
      MIN / -1, a shift by the width or more and an out-of-range float
      conversion undefined. Folding skips exactly those cases.
   3. Memory may change at every store and every call. No pass reasons
      about the contents of memory. */

/* Helpers */

static void *allocate(size_t count, size_t size)
{
    void *p = calloc(count + 1, size);

    if (p == NULL) {
        fputs("antic: out of memory\n", stderr);
        exit(70);
    }
    return p;
}

static int bits(enum ir_type type)
{
    switch (type) {
    case IR_I8: return 8;
    case IR_I16: return 16;
    case IR_I32: return 32;
    default: return 64;
    }
}

static uint64_t trim(enum ir_type type, uint64_t v)
{
    int n = bits(type);
    return n == 64 ? v : v & (((uint64_t)1 << n) - 1);
}

static int64_t signed_value(enum ir_type type, uint64_t v)
{
    int n = bits(type);
    uint64_t sign = (uint64_t)1 << (n - 1);

    v = trim(type, v);
    return (int64_t)((v ^ sign) - sign);
}

static struct ir_operand none(void)
{
    struct ir_operand o = {IR_NONE, IR_VOID, {0}};
    return o;
}

static bool is_constant(const struct ir_operand *o)
{
    return o->kind == IR_INT || o->kind == IR_FLOAT;
}

static bool is_int(const struct ir_operand *o, uint64_t v)
{
    return o->kind == IR_INT && o->as.integer == trim(o->type, v);
}

static bool is_temp(const struct ir_operand *o, uint32_t temp)
{
    return o->kind == IR_TEMP && o->as.temp == temp;
}

/* Turn inst into result = copy value, keeping its result and type. */
static void make_copy(struct ir_inst *inst, struct ir_operand value)
{
    free(inst->args);
    inst->args = NULL;
    inst->arg_count = 0;
    inst->op = IR_COPY;
    inst->a = value;
    inst->b = none();
    inst->c = none();
}

static void delete_inst(struct ir_block *b, size_t i)
{
    free(b->insts[i].args);
    memmove(&b->insts[i], &b->insts[i + 1],
            (b->count - i - 1) * sizeof *b->insts);
    b->count--;
}

/* Instructions whose only effect is their result. */
static bool is_pure(enum ir_op op)
{
    return op != IR_STORE && op != IR_BITSTORE && op != IR_MEMCOPY &&
           op != IR_CALL &&
           op != IR_JUMP && op != IR_BRANCH && op != IR_RET;
}

static struct ir_operand *operand(struct ir_inst *inst, size_t i)
{
    switch (i) {
    case 0: return &inst->a;
    case 1: return &inst->b;
    case 2: return &inst->c;
    default: return &inst->args[i - 3];
    }
}

static size_t operand_count(const struct ir_inst *inst)
{
    return 3 + inst->arg_count;
}

/* How often each temporary is defined and used. A parameter counts as
   one definition. For a temporary with one definition, the block and
   the position of that definition. */
struct counts {
    uint32_t *defs;
    uint32_t *uses;
    uint32_t *block;
    size_t *index;
};

static void count(const struct ir_function *f, struct counts *c)
{
    size_t b;
    size_t i;
    size_t k;

    c->defs = allocate(f->temp_count, sizeof *c->defs);
    c->uses = allocate(f->temp_count, sizeof *c->uses);
    c->block = allocate(f->temp_count, sizeof *c->block);
    c->index = allocate(f->temp_count, sizeof *c->index);
    for (i = 0; i < f->param_count; i++) {
        c->defs[f->params[i].temp]++;
    }
    for (b = 0; b < f->block_count; b++) {
        for (i = 0; i < f->blocks[b]->count; i++) {
            struct ir_inst *inst = &f->blocks[b]->insts[i];
            for (k = 0; k < operand_count(inst); k++) {
                if (operand(inst, k)->kind == IR_TEMP) {
                    c->uses[operand(inst, k)->as.temp]++;
                }
            }
            if (inst->result != IR_NO_RESULT) {
                c->defs[inst->result]++;
                c->block[inst->result] = (uint32_t)b;
                c->index[inst->result] = i;
            }
        }
    }
}

static void free_counts(struct counts *c)
{
    free(c->defs);
    free(c->uses);
    free(c->block);
    free(c->index);
}

/* Constant folding */

static bool fold_int(enum ir_op op, enum ir_type t, uint64_t a, uint64_t b,
                     uint64_t *out)
{
    int n = bits(t);
    uint64_t min = trim(t, (uint64_t)1 << (n - 1));
    int64_t x = signed_value(t, a);
    int64_t y = signed_value(t, b);

    switch (op) {
    case IR_ADD: *out = a + b; break;
    case IR_SUB: *out = a - b; break;
    case IR_MUL: *out = a * b; break;
    case IR_SDIV:
    case IR_SREM:
        if (b == 0 || (a == min && b == trim(t, (uint64_t)-1))) {
            return false;
        }
        *out = (uint64_t)(op == IR_SDIV ? x / y : x % y);
        break;
    case IR_UDIV:
    case IR_UREM:
        if (b == 0) {
            return false;
        }
        *out = op == IR_UDIV ? a / b : a % b;
        break;
    case IR_AND: *out = a & b; break;
    case IR_OR: *out = a | b; break;
    case IR_XOR: *out = a ^ b; break;
    case IR_SHL:
    case IR_SHR_S:
    case IR_SHR_U:
        if (b >= (uint64_t)n) {
            return false;
        }
        if (op == IR_SHL) {
            *out = a << b;
        } else if (op == IR_SHR_U) {
            *out = a >> b;
        } else {
            /* C leaves >> of a negative value to the implementation. */
            *out = x < 0 ? ~(~(uint64_t)x >> b) : (uint64_t)x >> b;
        }
        break;
    case IR_EQ: *out = a == b; return true;
    case IR_NE: *out = a != b; return true;
    case IR_SLT: *out = x < y; return true;
    case IR_SLE: *out = x <= y; return true;
    case IR_SGT: *out = x > y; return true;
    case IR_SGE: *out = x >= y; return true;
    case IR_ULT: *out = a < b; return true;
    case IR_ULE: *out = a <= b; return true;
    case IR_UGT: *out = a > b; return true;
    case IR_UGE: *out = a >= b; return true;
    default:
        return false;
    }
    *out = trim(t, *out);
    return true;
}

/* f32 arithmetic happens in float, so the result is the f32 result. */
static bool fold_float(enum ir_op op, enum ir_type t, double a, double b,
                       struct ir_operand *out)
{
    double r;
    float fa = (float)a;
    float fb = (float)b;
    bool single = t == IR_F32;

    switch (op) {
    case IR_FADD: r = single ? (double)(fa + fb) : a + b; break;
    case IR_FSUB: r = single ? (double)(fa - fb) : a - b; break;
    case IR_FMUL: r = single ? (double)(fa * fb) : a * b; break;
    case IR_FDIV: r = single ? (double)(fa / fb) : a / b; break;
    case IR_FEQ: *out = ir_int_op(IR_I8, a == b); return true;
    case IR_FNE: *out = ir_int_op(IR_I8, a != b); return true;
    case IR_FLT: *out = ir_int_op(IR_I8, a < b); return true;
    case IR_FLE: *out = ir_int_op(IR_I8, a <= b); return true;
    case IR_FGT: *out = ir_int_op(IR_I8, a > b); return true;
    case IR_FGE: *out = ir_int_op(IR_I8, a >= b); return true;
    default:
        return false;
    }
    *out = ir_float_op(t, r);
    return true;
}

static bool fold_conversion(const struct ir_inst *inst, struct ir_operand *out)
{
    enum ir_type from = inst->a.type;
    enum ir_type to = inst->type;
    int n = bits(to);
    uint64_t v = inst->a.as.integer;
    double d = inst->a.as.floating;
    double limit = n == 64 ? 18446744073709551616.0
                           : (double)((uint64_t)1 << n);

    switch (inst->op) {
    case IR_TRUNC:
    case IR_ZEXT:
        *out = ir_int_op(to, v);
        return true;
    case IR_SEXT:
        *out = ir_int_op(to, (uint64_t)signed_value(from, v));
        return true;
    case IR_SITOF:
    case IR_UITOF:
        /* One rounding step to the target type. A detour through f64
           rounds twice and can land on the other f32 neighbour. */
        if (inst->op == IR_SITOF) {
            *out = ir_float_op(to, to == IR_F32
                                       ? (double)(float)signed_value(from, v)
                                       : (double)signed_value(from, v));
        } else {
            *out = ir_float_op(to, to == IR_F32 ? (double)(float)v
                                                : (double)v);
        }
        return true;
    case IR_FTOSI:
        if (!(d >= -limit / 2 && d < limit / 2)) {
            return false;
        }
        *out = ir_int_op(to, (uint64_t)(int64_t)d);
        return true;
    case IR_FTOUI:
        if (!(d > -1.0 && d < limit)) {
            return false;
        }
        *out = ir_int_op(to, (uint64_t)d);
        return true;
    case IR_FEXT:
    case IR_FTRUNC:
        break;
    default:
        return false;
    }
    *out = ir_float_op(to, to == IR_F32 ? (double)(float)d : d);
    return true;
}

static bool is_target_sized(enum ir_type type)
{
    return type == IR_CLONG || type == IR_CWCHAR;
}

/* DESIGN: an operation on c_long or c_wchar wraps at a width that only the
   back end knows, so no pass computes one. */
static bool fold_inst(const struct ir_inst *inst, struct ir_operand *out)
{
    uint64_t v;

    if (is_target_sized(inst->type) || is_target_sized(inst->a.type) ||
        is_target_sized(inst->b.type)) {
        return false;
    }
    switch (inst->op) {
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_SDIV: case IR_UDIV:
    case IR_SREM: case IR_UREM: case IR_AND: case IR_OR: case IR_XOR:
    case IR_SHL: case IR_SHR_S: case IR_SHR_U:
    case IR_EQ: case IR_NE: case IR_SLT: case IR_SLE: case IR_SGT:
    case IR_SGE: case IR_ULT: case IR_ULE: case IR_UGT: case IR_UGE:
        if (inst->a.kind != IR_INT || inst->b.kind != IR_INT ||
            !fold_int(inst->op, inst->a.type, inst->a.as.integer,
                      inst->b.as.integer, &v)) {
            return false;
        }
        *out = ir_int_op(inst->type, v);
        return true;
    case IR_FADD: case IR_FSUB: case IR_FMUL: case IR_FDIV:
    case IR_FEQ: case IR_FNE: case IR_FLT: case IR_FLE: case IR_FGT:
    case IR_FGE:
        return inst->a.kind == IR_FLOAT && inst->b.kind == IR_FLOAT &&
               fold_float(inst->op, inst->a.type, inst->a.as.floating,
                          inst->b.as.floating, out);
    case IR_NEG:
    case IR_NOT:
        if (inst->a.kind != IR_INT) {
            return false;
        }
        v = inst->op == IR_NEG ? 0 - inst->a.as.integer : ~inst->a.as.integer;
        *out = ir_int_op(inst->type, v);
        return true;
    case IR_FNEG:
        if (inst->a.kind != IR_FLOAT) {
            return false;
        }
        *out = ir_float_op(inst->type, -inst->a.as.floating);
        return true;
    case IR_TRUNC: case IR_SEXT: case IR_ZEXT: case IR_SITOF: case IR_UITOF:
    case IR_FTOSI: case IR_FTOUI: case IR_FEXT: case IR_FTRUNC:
        return is_constant(&inst->a) && fold_conversion(inst, out);
    default:
        return false;
    }
}

/* Compute every operation whose operands are constants, and turn a branch
   on a constant into a jump. */
static bool fold_constants(struct ir_function *f)
{
    bool changed = false;
    size_t b;
    size_t i;

    for (b = 0; b < f->block_count; b++) {
        for (i = 0; i < f->blocks[b]->count; i++) {
            struct ir_inst *inst = &f->blocks[b]->insts[i];
            struct ir_operand value;
            if (inst->op == IR_BRANCH && inst->a.kind == IR_INT) {
                inst->a = inst->a.as.integer != 0 ? inst->b : inst->c;
                inst->op = IR_JUMP;
                inst->b = none();
                inst->c = none();
                changed = true;
            } else if (fold_inst(inst, &value)) {
                make_copy(inst, value);
                changed = true;
            }
        }
    }
    return changed;
}

/* Copy propagation */

static bool replace_uses(struct ir_function *f, uint32_t temp,
                         struct ir_operand value)
{
    bool changed = false;
    size_t b;
    size_t i;
    size_t k;

    for (b = 0; b < f->block_count; b++) {
        for (i = 0; i < f->blocks[b]->count; i++) {
            struct ir_inst *inst = &f->blocks[b]->insts[i];
            for (k = 0; k < operand_count(inst); k++) {
                if (is_temp(operand(inst, k), temp)) {
                    *operand(inst, k) = value;
                    changed = true;
                }
            }
        }
    }
    return changed;
}

/* Inside one block, a use after x = copy v and before the next
   definition of x or of v reads v. */
static bool propagate_in_block(struct ir_function *f, struct ir_block *b)
{
    struct ir_operand *known = allocate(f->temp_count, sizeof *known);
    uint32_t *active = allocate(f->temp_count, sizeof *active);
    size_t active_count = 0;
    bool changed = false;
    size_t i;
    size_t k;

    for (i = 0; i < b->count; i++) {
        struct ir_inst *inst = &b->insts[i];
        uint32_t r = inst->result;
        for (k = 0; k < operand_count(inst); k++) {
            struct ir_operand *o = operand(inst, k);
            if (o->kind == IR_TEMP && known[o->as.temp].kind != IR_NONE) {
                *o = known[o->as.temp];
                changed = true;
            }
        }
        if (r == IR_NO_RESULT) {
            continue;
        }
        for (k = 0; k < active_count;) {
            uint32_t t = active[k];
            if (t == r || is_temp(&known[t], r)) {
                known[t] = none();
                active[k] = active[--active_count];
            } else {
                k++;
            }
        }
        if (inst->op == IR_COPY && !is_temp(&inst->a, r)) {
            known[r] = inst->a;
            active[active_count++] = r;
        }
    }
    free(known);
    free(active);
    return changed;
}

/* Across blocks, x = copy v replaces every use of x when x has a single
   definition. v must be a constant, a parameter that no instruction
   assigns, or a temporary with a single definition earlier in the same
   block. Definite assignment, which the verifier checks, makes each case
   exact. */
static bool propagate_copies(struct ir_function *f)
{
    struct counts c;
    bool changed = false;
    size_t b;
    size_t i;

    for (b = 0; b < f->block_count; b++) {
        changed = propagate_in_block(f, f->blocks[b]) || changed;
    }
    count(f, &c);
    for (b = 0; b < f->block_count; b++) {
        for (i = 0; i < f->blocks[b]->count; i++) {
            struct ir_inst *inst = &f->blocks[b]->insts[i];
            uint32_t x = inst->result;
            uint32_t y = inst->a.as.temp;
            bool exact;
            if (inst->op != IR_COPY || c.defs[x] != 1 || is_temp(&inst->a, x)) {
                continue;
            }
            exact = is_constant(&inst->a) ||
                    (inst->a.kind == IR_TEMP && c.defs[y] == 1 &&
                     (y < f->param_count ||
                      (c.block[y] == b && c.index[y] < i)));
            if (exact) {
                changed = replace_uses(f, x, inst->a) || changed;
            }
        }
    }
    free_counts(&c);
    return changed;
}

/* Peephole rules */

static bool is_commutative(enum ir_op op)
{
    return op == IR_ADD || op == IR_MUL || op == IR_AND || op == IR_OR ||
           op == IR_XOR || op == IR_EQ || op == IR_NE || op == IR_FADD ||
           op == IR_FMUL || op == IR_FEQ || op == IR_FNE;
}

static int power_of_two(const struct ir_operand *o)
{
    uint64_t v = o->as.integer;
    int k = 0;

    if (o->kind != IR_INT || v < 2 || (v & (v - 1)) != 0) {
        return 0;
    }
    while (v > 1) {
        v >>= 1;
        k++;
    }
    return k;
}

/* Rewrite inst into a simpler instruction with the same result. */
static bool simplify(struct ir_inst *inst)
{
    enum ir_type t = inst->type;
    int k;

    if (is_commutative(inst->op) && is_constant(&inst->a) &&
        !is_constant(&inst->b)) {
        struct ir_operand swap = inst->a;
        inst->a = inst->b;
        inst->b = swap;
        return true;
    }
    switch (inst->op) {
    case IR_ADD: case IR_SUB: case IR_OR: case IR_XOR:
    case IR_SHL: case IR_SHR_S: case IR_SHR_U:
        if (is_int(&inst->b, 0)) {
            make_copy(inst, inst->a);
            return true;
        }
        return false;
    case IR_MUL:
        if (is_int(&inst->b, 0)) {
            make_copy(inst, inst->b);
            return true;
        }
        if (is_int(&inst->b, 1)) {
            make_copy(inst, inst->a);
            return true;
        }
        if ((k = power_of_two(&inst->b)) > 0) {
            inst->op = IR_SHL;
            inst->b = ir_int_op(t, (uint64_t)k);
            return true;
        }
        return false;
    case IR_SDIV:
    case IR_UDIV:
        if (is_int(&inst->b, 1)) {
            make_copy(inst, inst->a);
            return true;
        }
        if (inst->op == IR_UDIV && (k = power_of_two(&inst->b)) > 0) {
            inst->op = IR_SHR_U;
            inst->b = ir_int_op(t, (uint64_t)k);
            return true;
        }
        return false;
    case IR_UREM:
        if (power_of_two(&inst->b) > 0) {
            inst->op = IR_AND;
            inst->b = ir_int_op(t, inst->b.as.integer - 1);
            return true;
        }
        return false;
    case IR_AND:
        if (is_int(&inst->b, 0)) {
            make_copy(inst, inst->b);
            return true;
        }
        if (is_int(&inst->b, (uint64_t)-1)) {
            make_copy(inst, inst->a);
            return true;
        }
        return false;
    case IR_BRANCH:
        if (inst->b.as.index == inst->c.as.index) {
            inst->op = IR_JUMP;
            inst->a = inst->b;
            inst->b = none();
            inst->c = none();
            return true;
        }
        return false;
    default:
        return false;
    }
}

/* The block that a jump to b reaches after blocks that only jump. Jumps
   that form a cycle keep b, so a jump never moves around a cycle. */
static uint32_t final_target(const struct ir_function *f, uint32_t b)
{
    uint32_t target = b;
    size_t steps;

    for (steps = 0; steps <= f->block_count; steps++) {
        const struct ir_block *block = f->blocks[target];
        if (block->count != 1 || block->insts[0].op != IR_JUMP) {
            return target;
        }
        target = block->insts[0].a.as.index;
    }
    return b;
}

static bool retarget(const struct ir_function *f, struct ir_operand *o)
{
    uint32_t target = final_target(f, o->as.index);

    if (target == o->as.index) {
        return false;
    }
    o->as.index = target;
    return true;
}

static bool apply_peephole_rules(struct ir_function *f)
{
    struct counts c;
    bool changed = false;
    size_t b;
    size_t i;

    count(f, &c);
    for (b = 0; b < f->block_count; b++) {
        struct ir_block *block = f->blocks[b];
        for (i = 0; i < block->count; i++) {
            struct ir_inst *inst = &block->insts[i];
            struct ir_inst *next = i + 1 < block->count ? inst + 1 : NULL;
            changed = simplify(inst) || changed;
            if (inst->op == IR_COPY && is_temp(&inst->a, inst->result)) {
                delete_inst(block, i--);
                changed = true;
                continue;
            }
            /* The pair t = op and x = copy t becomes x = op, when the
               copy is the only use of t. */
            if (next != NULL && next->op == IR_COPY &&
                inst->result != IR_NO_RESULT &&
                is_temp(&next->a, inst->result) &&
                c.defs[inst->result] == 1 && c.uses[inst->result] == 1 &&
                inst->result >= f->param_count) {
                inst->result = next->result;
                delete_inst(block, i + 1);
                changed = true;
                continue;
            }
            if (inst->op == IR_JUMP) {
                changed = retarget(f, &inst->a) || changed;
            } else if (inst->op == IR_BRANCH) {
                changed = retarget(f, &inst->b) || changed;
                changed = retarget(f, &inst->c) || changed;
            }
        }
    }
    free_counts(&c);
    return changed;
}

/* Dead code elimination */

static bool remove_unused_results(struct ir_function *f)
{
    struct counts c;
    bool changed = false;
    size_t b;
    size_t i;
    size_t j;
    size_t k;

    count(f, &c);
    for (b = 0; b < f->block_count; b++) {
        struct ir_block *block = f->blocks[b];
        for (i = block->count; i-- > 0;) {
            struct ir_inst *inst = &block->insts[i];
            bool dead;
            if (inst->result == IR_NO_RESULT || !is_pure(inst->op)) {
                continue;
            }
            dead = c.uses[inst->result] == 0;
            /* A definition that the block overwrites before any use. */
            for (j = i + 1; j < block->count && !dead; j++) {
                bool used = false;
                for (k = 0; k < operand_count(&block->insts[j]); k++) {
                    used = used || is_temp(operand(&block->insts[j], k),
                                           inst->result);
                }
                if (used) {
                    break;
                }
                dead = block->insts[j].result == inst->result;
            }
            if (dead) {
                delete_inst(block, i);
                changed = true;
            }
        }
    }
    free_counts(&c);
    return changed;
}

static void mark_reachable(const struct ir_function *f, uint32_t b,
                           bool *reached)
{
    const struct ir_block *block;
    const struct ir_inst *last;

    if (reached[b]) {
        return;
    }
    reached[b] = true;
    block = f->blocks[b];
    if (block->count == 0) {
        return;
    }
    last = &block->insts[block->count - 1];
    if (last->op == IR_JUMP) {
        mark_reachable(f, last->a.as.index, reached);
    } else if (last->op == IR_BRANCH) {
        mark_reachable(f, last->b.as.index, reached);
        mark_reachable(f, last->c.as.index, reached);
    }
}

/* Remove the blocks that no path from the entry reaches, and number the
   others again in their order. */
static bool remove_unreachable_blocks(struct ir_function *f)
{
    bool *reached = allocate(f->block_count, sizeof *reached);
    uint32_t *map = allocate(f->block_count, sizeof *map);
    size_t n = 0;
    size_t b;
    size_t i;
    bool changed;

    mark_reachable(f, 0, reached);
    for (b = 0; b < f->block_count; b++) {
        if (reached[b]) {
            map[b] = (uint32_t)n;
            f->blocks[n] = f->blocks[b];
            f->blocks[n]->index = (uint32_t)n;
            n++;
        } else {
            ir_block_free(f->blocks[b]);
        }
    }
    changed = n != f->block_count;
    f->block_count = n;
    for (b = 0; b < n && changed; b++) {
        for (i = 0; i < f->blocks[b]->count; i++) {
            struct ir_inst *inst = &f->blocks[b]->insts[i];
            if (inst->a.kind == IR_BLOCK) {
                inst->a.as.index = map[inst->a.as.index];
            }
            if (inst->b.kind == IR_BLOCK) {
                inst->b.as.index = map[inst->b.as.index];
            }
            if (inst->c.kind == IR_BLOCK) {
                inst->c.as.index = map[inst->c.as.index];
            }
        }
    }
    free(reached);
    free(map);
    return changed;
}

/* A block that ends with a jump to a block with no other predecessor
   takes over that block's instructions. */
static bool merge_blocks(struct ir_function *f)
{
    uint32_t *preds = allocate(f->block_count, sizeof *preds);
    bool changed = false;
    size_t b;

    for (b = 0; b < f->block_count; b++) {
        const struct ir_block *block = f->blocks[b];
        const struct ir_inst *last = block->count > 0
                                         ? &block->insts[block->count - 1]
                                         : NULL;
        if (last == NULL) {
            continue;
        }
        if (last->op == IR_JUMP) {
            preds[last->a.as.index]++;
        } else if (last->op == IR_BRANCH) {
            preds[last->b.as.index]++;
            preds[last->c.as.index]++;
        }
    }
    for (b = 0; b < f->block_count; b++) {
        struct ir_block *block = f->blocks[b];
        struct ir_block *next;
        struct ir_inst *insts;
        uint32_t target;
        if (block->count == 0 ||
            block->insts[block->count - 1].op != IR_JUMP) {
            continue;
        }
        target = block->insts[block->count - 1].a.as.index;
        next = f->blocks[target];
        if (target == b || target == 0 || preds[target] != 1 ||
            next->count == 0) {
            continue;
        }
        insts = realloc(block->insts,
                        (block->count - 1 + next->count) * sizeof *insts);
        if (insts == NULL) {
            fputs("antic: out of memory\n", stderr);
            exit(70);
        }
        memcpy(insts + block->count - 1, next->insts,
               next->count * sizeof *insts);
        block->insts = insts;
        block->count = block->count - 1 + next->count;
        block->capacity = block->count;
        free(next->insts);
        next->insts = NULL;
        next->count = 0;
        next->capacity = 0;
        preds[target] = 0;
        changed = true;
        b--;
    }
    free(preds);
    return changed;
}

static bool remove_dead_code(struct ir_function *f)
{
    bool changed = remove_unused_results(f);

    changed = remove_unreachable_blocks(f) || changed;
    if (merge_blocks(f)) {
        remove_unreachable_blocks(f);
        changed = true;
    }
    return changed;
}

/* Number the temporaries again: the parameters first, then every other
   temporary in the order of its first appearance. */
static void renumber_temps(struct ir_function *f)
{
    uint32_t *map = allocate(f->temp_count, sizeof *map);
    enum ir_type *types = allocate(f->temp_count, sizeof *types);
    uint32_t next = 0;
    size_t b;
    size_t i;
    size_t k;

    for (i = 0; i < f->temp_count; i++) {
        map[i] = IR_NO_RESULT;
    }
    for (i = 0; i < f->param_count; i++) {
        map[f->params[i].temp] = next;
        types[next++] = f->temps[f->params[i].temp];
    }
    for (b = 0; b < f->block_count; b++) {
        for (i = 0; i < f->blocks[b]->count; i++) {
            struct ir_inst *inst = &f->blocks[b]->insts[i];
            if (inst->result != IR_NO_RESULT &&
                map[inst->result] == IR_NO_RESULT) {
                map[inst->result] = next;
                types[next++] = f->temps[inst->result];
            }
            for (k = 0; k < operand_count(inst); k++) {
                struct ir_operand *o = operand(inst, k);
                if (o->kind == IR_TEMP && map[o->as.temp] == IR_NO_RESULT) {
                    map[o->as.temp] = next;
                    types[next++] = f->temps[o->as.temp];
                }
            }
        }
    }
    for (i = 0; i < f->param_count; i++) {
        f->params[i].temp = map[f->params[i].temp];
    }
    for (b = 0; b < f->block_count; b++) {
        for (i = 0; i < f->blocks[b]->count; i++) {
            struct ir_inst *inst = &f->blocks[b]->insts[i];
            if (inst->result != IR_NO_RESULT) {
                inst->result = map[inst->result];
            }
            for (k = 0; k < operand_count(inst); k++) {
                struct ir_operand *o = operand(inst, k);
                if (o->kind == IR_TEMP) {
                    o->as.temp = map[o->as.temp];
                }
            }
        }
    }
    memcpy(f->temps, types, next * sizeof *types);
    f->temp_count = next;
    free(map);
    free(types);
}

void ir_optimize_function(struct ir_function *f)
{
    bool changed = true;

    /* The peephole rules run before copy propagation. Propagation would
       give t in the pair t = op and x = copy t a second use. */
    while (changed) {
        changed = fold_constants(f);
        changed = apply_peephole_rules(f) || changed;
        changed = propagate_copies(f) || changed;
        changed = remove_dead_code(f) || changed;
    }
    renumber_temps(f);
}

/* The whole program */

static void mark_function(struct ir_module *m, uint32_t index, bool *live,
                          bool *live_globals)
{
    struct ir_function *f = m->functions[index];
    size_t b;
    size_t i;
    size_t k;

    if (live[index]) {
        return;
    }
    live[index] = true;
    for (b = 0; b < f->block_count; b++) {
        for (i = 0; i < f->blocks[b]->count; i++) {
            struct ir_inst *inst = &f->blocks[b]->insts[i];
            for (k = 0; k < operand_count(inst); k++) {
                const struct ir_operand *o = operand(inst, k);
                if (o->kind == IR_FUNC) {
                    mark_function(m, o->as.index, live, live_globals);
                } else if (o->kind == IR_GLOBAL) {
                    live_globals[o->as.index] = true;
                }
            }
        }
    }
}

/* Mark the globals that the addresses inside a constant reach, and set
   grew when one of them was not marked before. */
static void mark_const(const struct ir_const *c, bool *live, bool *grew)
{
    size_t i;

    if (c->kind == IR_CONST_ADDR) {
        *grew = *grew || !live[c->global];
        live[c->global] = true;
    } else if (c->kind == IR_CONST_AGG) {
        for (i = 0; i < c->item_count; i++) {
            mark_const(&c->items[i], live, grew);
        }
    }
}

/* Move the addresses inside a constant to the indices of the globals that
   remain. */
static void remap_const(struct ir_const *c, const uint32_t *map)
{
    size_t i;

    if (c->kind == IR_CONST_ADDR) {
        c->global = map[c->global];
    } else if (c->kind == IR_CONST_AGG) {
        for (i = 0; i < c->item_count; i++) {
            remap_const(&c->items[i], map);
        }
    }
}

/* Remove the functions and globals that the entry cannot reach. The entry
   is main in module entry, or every function of that module when it has
   no main or when all is set. Every export fn is an entry too, because C
   code may call it. */
static void remove_unused_functions(struct ir_module *m, const char *entry,
                                    bool all)
{
    bool *live = allocate(m->function_count, sizeof *live);
    bool *live_globals = allocate(m->global_count, sizeof *live_globals);
    uint32_t *map = allocate(m->function_count, sizeof *map);
    uint32_t *global_map = allocate(m->global_count, sizeof *global_map);
    bool has_main = false;
    bool grew = true;
    size_t n = 0;
    size_t i;
    size_t j;
    size_t b;
    size_t k;

    for (i = 0; i < m->function_count; i++) {
        const struct ir_function *f = m->functions[i];
        if (!all && !f->is_extern && strcmp(f->module, entry) == 0 &&
            strcmp(f->name, "main") == 0) {
            has_main = true;
        }
    }
    for (i = 0; i < m->function_count; i++) {
        const struct ir_function *f = m->functions[i];
        if (!f->is_extern &&
            (f->exported || (strcmp(f->module, entry) == 0 &&
                             (!has_main || strcmp(f->name, "main") == 0)))) {
            mark_function(m, (uint32_t)i, live, live_globals);
        }
    }
    while (grew) {
        grew = false;
        for (i = 0; i < m->global_count; i++) {
            const struct ir_global *g = m->globals[i];
            for (j = 0; live_globals[i] && j < g->reloc_count; j++) {
                uint32_t target = g->relocs[j].global;
                grew = grew || !live_globals[target];
                live_globals[target] = true;
            }
            if (live_globals[i] && g->value != NULL) {
                mark_const(g->value, live_globals, &grew);
            }
        }
    }
    for (i = 0; i < m->function_count; i++) {
        if (live[i]) {
            map[i] = (uint32_t)n;
            m->functions[n] = m->functions[i];
            m->functions[n]->index = (uint32_t)n;
            n++;
        } else {
            ir_function_free(m->functions[i]);
        }
    }
    m->function_count = n;
    n = 0;
    for (i = 0; i < m->global_count; i++) {
        if (live_globals[i]) {
            global_map[i] = (uint32_t)n;
            m->globals[n] = m->globals[i];
            m->globals[n]->index = (uint32_t)n;
            n++;
        }
    }
    m->global_count = n;
    for (i = 0; i < m->global_count; i++) {
        for (j = 0; j < m->globals[i]->reloc_count; j++) {
            m->globals[i]->relocs[j].global =
                global_map[m->globals[i]->relocs[j].global];
        }
        if (m->globals[i]->value != NULL) {
            remap_const(m->globals[i]->value, global_map);
        }
    }
    for (i = 0; i < m->function_count; i++) {
        struct ir_function *f = m->functions[i];
        for (b = 0; b < f->block_count; b++) {
            for (j = 0; j < f->blocks[b]->count; j++) {
                struct ir_inst *inst = &f->blocks[b]->insts[j];
                for (k = 0; k < operand_count(inst); k++) {
                    struct ir_operand *o = operand(inst, k);
                    if (o->kind == IR_FUNC) {
                        o->as.index = map[o->as.index];
                    } else if (o->kind == IR_GLOBAL) {
                        o->as.index = global_map[o->as.index];
                    }
                }
            }
        }
    }
    free(live);
    free(live_globals);
    free(map);
    free(global_map);
}

void ir_optimize(struct ir_module *program, const char *entry)
{
    size_t i;

    for (i = 0; i < program->function_count; i++) {
        if (!program->functions[i]->is_extern) {
            ir_optimize_function(program->functions[i]);
        }
    }
    remove_unused_functions(program, entry, false);
}

/* Make f a declaration: free its blocks and temporaries and keep its
   parameters, which a call follows. */
static void drop_body(struct ir_function *f)
{
    size_t i;

    for (i = 0; i < f->block_count; i++) {
        ir_block_free(f->blocks[i]);
    }
    free(f->blocks);
    free(f->temps);
    f->blocks = NULL;
    f->block_count = 0;
    f->block_capacity = 0;
    f->temps = NULL;
    f->temp_count = 0;
    f->temp_capacity = 0;
    for (i = 0; i < f->param_count; i++) {
        f->params[i].temp = IR_NO_RESULT;
    }
    f->is_extern = true;
}

void ir_optimize_module(struct ir_module *program, const char *module)
{
    size_t i;

    for (i = 0; i < program->function_count; i++) {
        struct ir_function *f = program->functions[i];
        if (f->is_extern) {
            continue;
        }
        if (strcmp(f->module, module) != 0) {
            drop_body(f);
        } else {
            ir_optimize_function(f);
        }
    }
    remove_unused_functions(program, module, true);
}
