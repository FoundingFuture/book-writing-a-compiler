#include "select.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "optimize.h"

/* DESIGN: an IR temporary %n becomes the virtual register tn, so the
   machine code keeps the numbers of the IR. Registers that selection adds,
   such as one for a constant, take the numbers after the temporaries. */

struct mach_operand mach_vreg(uint32_t vreg, uint8_t width)
{
    struct mach_operand o;

    memset(&o, 0, sizeof o);
    o.kind = MACH_VREG;
    o.width = width;
    o.reg = vreg;
    return o;
}

struct mach_operand mach_preg(uint32_t preg, uint8_t width)
{
    struct mach_operand o = mach_vreg(preg, width);

    o.kind = MACH_PREG;
    return o;
}

struct mach_operand mach_imm(int64_t value)
{
    struct mach_operand o;

    memset(&o, 0, sizeof o);
    o.kind = MACH_IMM;
    o.width = 64;
    o.value = value;
    return o;
}

struct mach_inst *select_emit(struct selector *s, uint16_t op, size_t count,
                              const struct mach_operand *operands)
{
    struct mach_inst *inst = mach_append(s->b);

    inst->op = op;
    inst->count = (uint8_t)count;
    memcpy(inst->operands, operands, count * sizeof *operands);
    return inst;
}

struct mach_operand select_new_vreg(struct selector *s, uint8_t width)
{
    return mach_vreg(mach_vreg_add(s->out, false), width);
}

struct mach_operand select_new_fp_vreg(struct selector *s, uint8_t width)
{
    return mach_vreg(mach_vreg_add(s->out, true), width);
}

bool select_is_float(enum ir_type type)
{
    return type == IR_F32 || type == IR_F64;
}

struct mach_operand select_part_register(uint8_t reg, unsigned bytes)
{
    return mach_preg(reg, bytes > 4 ? 64 : 32);
}

void select_store_parts(struct selector *s, const struct arg_location *loc,
                        struct mach_operand address)
{
    size_t i;

    for (i = 0; i < loc->part_count; i++) {
        const struct arg_part *part = &loc->parts[i];
        s->target->store_bytes(s, select_part_register(part->reg, part->bytes),
                               address, part->offset, part->bytes);
    }
}

uint64_t select_load_parts(struct selector *s, const struct arg_location *loc,
                           struct mach_operand address)
{
    uint64_t uses = 0;
    size_t i;

    for (i = 0; i < loc->part_count; i++) {
        const struct arg_part *part = &loc->parts[i];
        s->target->load_bytes(s, select_part_register(part->reg, part->bytes),
                              address, part->offset, part->bytes);
        uses |= (uint64_t)1 << part->reg;
    }
    return uses;
}

/* DESIGN: a slot for an aggregate that register parts fill is a multiple
   of 8 bytes, so a part of 8 bytes never writes past its end. */
static uint32_t aggregate_slot(struct selector *s, const struct layout *agg)
{
    return mach_slot_add(s->out, (agg->size + 7) / 8 * 8,
                         agg->align < 8 ? 8 : agg->align);
}

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

const struct ir_function *select_callee(const struct selector *s,
                                        const struct ir_inst *inst)
{
    return s->m->functions[inst->b.kind == IR_FUNC ? inst->b.as.index
                                                   : inst->a.as.index];
}

bool select_uses_got(const struct selector *s, const struct ir_operand *o)
{
    return o->kind == IR_FUNC && s->m->functions[o->as.index]->module == NULL &&
           s->convention != CONVENTION_WINDOWS_X64 &&
           s->convention != CONVENTION_WINDOWS_ARM64;
}

struct mach_operand select_result_slot(struct selector *s,
                                       const struct ir_inst *inst,
                                       const struct layout *agg)
{
    struct mach_operand address = mach_vreg(inst->result, 64);

    s->target->slot_address(s, address, aggregate_slot(s, agg));
    return address;
}

struct mach_operand select_result(struct selector *s,
                                  const struct ir_inst *inst)
{
    return mach_vreg(inst->result, s->target->width(inst->type));
}

/* The register that holds operand o. A constant is loaded into a new
   virtual register first. */
struct mach_operand select_reg(struct selector *s, const struct ir_operand *o)
{
    uint8_t width = s->target->width(o->type);
    struct mach_operand r;

    if (o->kind == IR_TEMP) {
        return mach_vreg(o->as.temp, width);
    }
    if (o->kind == IR_FLOAT) {
        r = select_new_fp_vreg(s, width);
        s->target->load_float(s, r, o->type, o->as.floating);
        return r;
    }
    r = select_new_vreg(s, width);
    s->target->load(s, r, o->as.integer);
    return r;
}

enum mach_cond select_cond(enum ir_op op)
{
    switch (op) {
    case IR_EQ: return COND_EQ;
    case IR_NE: return COND_NE;
    case IR_SLT: return COND_LT;
    case IR_SLE: return COND_LE;
    case IR_SGT: return COND_GT;
    case IR_SGE: return COND_GE;
    case IR_ULT: return COND_LO;
    case IR_ULE: return COND_LS;
    case IR_UGT: return COND_HI;
    default: return COND_HS;
    }
}

enum mach_cond select_negate(enum mach_cond cond)
{
    static const enum mach_cond negated[] = {
        [COND_EQ] = COND_NE, [COND_NE] = COND_EQ, [COND_LT] = COND_GE,
        [COND_LE] = COND_GT, [COND_GT] = COND_LE, [COND_GE] = COND_LT,
        [COND_LO] = COND_HS, [COND_LS] = COND_HI, [COND_HI] = COND_LS,
        [COND_HS] = COND_LO, [COND_MI] = COND_PL, [COND_PL] = COND_MI,
        [COND_P] = COND_NP, [COND_NP] = COND_P,
    };
    return negated[cond];
}

/* Whether block is the one that follows the current block, so control
   reaches it without a jump. */
bool select_is_next(const struct selector *s, const struct ir_operand *block)
{
    return block->as.index == s->block + 1;
}

static void fail(struct selector *s, const char *format, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

static void fail(struct selector *s, const char *format, ...)
{
    va_list args;

    if (s->failed) {
        return;
    }
    va_start(args, format);
    vsnprintf(s->error, s->error_size, format, args);
    va_end(args);
    s->failed = true;
}

void select_fail(struct selector *s, const char *message)
{
    fail(s, "%s", message);
}

void select_refuse(struct selector *s, const struct ir_inst *inst)
{
    fail(s, "instruction selection for `%s` on %s arrives in chapter %d",
         ir_op_name(inst->op), s->target->name, s->target->chapter);
}

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

static bool is_comparison(enum ir_op op)
{
    return op >= IR_EQ && op <= IR_UGE;
}

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

static void count_uses(struct selector *s)
{
    const struct ir_function *f = s->f;
    size_t b;
    size_t i;
    size_t k;

    s->uses = calloc(f->temp_count + 1, sizeof *s->uses);
    if (s->uses == NULL) {
        fputs("antic: out of memory\n", stderr);
        exit(70);
    }
    for (b = 0; b < f->block_count; b++) {
        for (i = 0; i < f->blocks[b]->count; i++) {
            const struct ir_inst *inst = &f->blocks[b]->insts[i];
            const struct ir_operand *ops[3];
            ops[0] = &inst->a;
            ops[1] = &inst->b;
            ops[2] = &inst->c;
            for (k = 0; k < 3; k++) {
                if (ops[k]->kind == IR_TEMP) {
                    s->uses[ops[k]->as.temp]++;
                }
            }
            for (k = 0; k < inst->arg_count; k++) {
                if (inst->args[k].kind == IR_TEMP) {
                    s->uses[inst->args[k].as.temp]++;
                }
            }
        }
    }
}

/* Whether inst reads temp as the address of a load or a store. */
static bool uses_as_address(const struct ir_inst *inst, uint32_t temp)
{
    return (inst->op == IR_LOAD && inst->a.kind == IR_TEMP &&
            inst->a.as.temp == temp) ||
           (inst->op == IR_STORE && inst->b.kind == IR_TEMP &&
            inst->b.as.temp == temp && !(inst->a.kind == IR_TEMP &&
                                         inst->a.as.temp == temp));
}

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

/* The parameters arrive in the argument registers of the convention, or
   on the stack. A parameter that the function never reads is not moved. */
static void select_params(struct selector *s)
{
    const struct ir_function *f = s->f;
    enum ir_type *types = calloc(f->param_count + 1, sizeof *types);
    struct arg_location *locations =
        calloc(f->param_count + 1, sizeof *locations);
    struct arg_location result;
    size_t i;

    if (types == NULL || locations == NULL) {
        fputs("antic: out of memory\n", stderr);
        exit(70);
    }
    for (i = 0; i < f->param_count; i++) {
        types[i] = f->params[i].type;
    }
    s->target->locate(s, f, types, f->param_count, locations);
    s->target->locate_result(s, f, &result);
    if (f->result == IR_AGG && result.indirect) {
        s->result_address = select_new_vreg(s, 64);
        s->target->move(s, s->result_address, mach_preg(result.reg, 64));
    }
    for (i = 0; i < f->param_count && !s->failed; i++) {
        uint8_t width = s->target->width(types[i]);
        struct mach_operand dst = mach_vreg(f->params[i].temp, width);
        if (s->uses[f->params[i].temp] == 0) {
            continue;
        }
        if (types[i] == IR_AGG) {
            dst = mach_vreg(f->params[i].temp, 64);
            if (locations[i].part_count > 0) {
                s->target->slot_address(s, dst,
                                        aggregate_slot(s, select_layout(
                                                              s, f->params[i].agg)));
                select_store_parts(s, &locations[i], dst);
            } else if (locations[i].indirect && locations[i].stack) {
                s->target->stack_param(s, locations[i].offset, dst);
            } else if (locations[i].indirect) {
                s->target->move(s, dst, mach_preg(locations[i].reg, 64));
            } else {
                s->target->incoming_address(s, dst, locations[i].offset);
            }
            continue;
        }
        if (locations[i].stack) {
            s->target->stack_param(s, locations[i].offset, dst);
        } else {
            s->target->move(s, dst, mach_preg(locations[i].reg, width));
        }
    }
    free(types);
    free(locations);
}

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

const struct target_desc *target_desc(enum target t)
{
    return target_info(t)->arch == ARCH_ARM64 ? target_desc_arm64()
                                              : target_desc_x86_64();
}

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
