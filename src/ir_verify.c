#include "ir.h"

#include <inttypes.h>

/* Check what every later stage relies on. Each block ends with exactly
   one terminator. Every operand refers to something that exists, and
   operand types agree with their instruction. A lowering bug then fails
   here, next to its cause, before it becomes wrong assembly. */

struct verifier {
    const struct ir_module *m;
    const struct ir_function *f;
    const struct ir_block *b;
    struct text *errors;
    bool ok;
};

static void fail(struct verifier *v, const char *format, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(struct verifier *v, const char *format, ...)
{
    char message[160];
    va_list args;

    va_start(args, format);
    vsnprintf(message, sizeof message, format, args);
    va_end(args);
    text_appendf(v->errors, "%s.%s b%" PRIu32 ": %s\n", v->f->module,
                 v->f->name, v->b->index, message);
    v->ok = false;
}

static bool is_terminator(enum ir_op op)
{
    return op == IR_JUMP || op == IR_BRANCH || op == IR_RET;
}

/* The type an operand carries, after checking that it refers to
   something that exists. */
static bool operand_ok(struct verifier *v, const struct ir_inst *inst,
                       const struct ir_operand *o)
{
    switch (o->kind) {
    case IR_TEMP:
        if (o->as.temp >= v->f->temp_count) {
            fail(v, "%s uses %%%" PRIu32 ", which does not exist",
                 ir_op_name(inst->op), o->as.temp);
            return false;
        }
        return true;
    case IR_BLOCK:
        if (o->as.index >= v->f->block_count) {
            fail(v, "%s names b%" PRIu32 ", which does not exist",
                 ir_op_name(inst->op), o->as.index);
            return false;
        }
        return true;
    case IR_FUNC:
        return o->as.index < v->m->function_count;
    case IR_GLOBAL:
        return o->as.index < v->m->global_count;
    case IR_SYM:
        if (o->as.index >= v->m->sym_count) {
            fail(v, "%s uses a symbolic value that does not exist",
                 ir_op_name(inst->op));
            return false;
        }
        return true;
    default:
        return true;
    }
}

static void vtype_ok(struct verifier *v, const struct ir_inst *inst)
{
    if (inst->of.type == IR_VOID ||
        (inst->of.type == IR_AGG && inst->of.agg >= v->m->agg_count)) {
        fail(v, "%s names a type that does not exist", ir_op_name(inst->op));
    }
}

static enum ir_type type_of(const struct verifier *v, const struct ir_operand *o)
{
    return o->kind == IR_TEMP ? v->f->temps[o->as.temp] : o->type;
}

static void same_type(struct verifier *v, const struct ir_inst *inst,
                      const struct ir_operand *o, enum ir_type expected)
{
    if (o->kind != IR_NONE && operand_ok(v, inst, o) &&
        type_of(v, o) != expected) {
        fail(v, "%s %s has an operand of type %s", ir_op_name(inst->op),
             ir_type_name(expected), ir_type_name(type_of(v, o)));
    }
}

static void check_inst(struct verifier *v, const struct ir_inst *inst)
{
    enum ir_type result = v->f->result == IR_AGG ? IR_PTR : v->f->result;

    switch (inst->op) {
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_SDIV: case IR_UDIV:
    case IR_SREM: case IR_UREM: case IR_AND: case IR_OR: case IR_XOR:
    case IR_SHL: case IR_SHR_S: case IR_SHR_U: case IR_FADD: case IR_FSUB:
    case IR_FMUL: case IR_FDIV:
        same_type(v, inst, &inst->a, inst->type);
        same_type(v, inst, &inst->b, inst->type);
        break;
    case IR_NEG: case IR_NOT: case IR_FNEG: case IR_COPY:
        same_type(v, inst, &inst->a, inst->type);
        break;
    case IR_EQ: case IR_NE: case IR_SLT: case IR_SLE: case IR_SGT:
    case IR_SGE: case IR_ULT: case IR_ULE: case IR_UGT: case IR_UGE:
    case IR_FEQ: case IR_FNE: case IR_FLT: case IR_FLE: case IR_FGT:
    case IR_FGE:
        if (operand_ok(v, inst, &inst->a) && operand_ok(v, inst, &inst->b) &&
            type_of(v, &inst->a) != type_of(v, &inst->b)) {
            fail(v, "%s compares %s with %s", ir_op_name(inst->op),
                 ir_type_name(type_of(v, &inst->a)),
                 ir_type_name(type_of(v, &inst->b)));
        }
        if (inst->type != IR_I8) {
            fail(v, "%s gives i8, not %s", ir_op_name(inst->op),
                 ir_type_name(inst->type));
        }
        break;
    case IR_LOAD:
        same_type(v, inst, &inst->a, IR_PTR);
        break;
    case IR_STORE:
        same_type(v, inst, &inst->a, inst->type);
        same_type(v, inst, &inst->b, IR_PTR);
        break;
    case IR_PTRADD:
        same_type(v, inst, &inst->a, IR_PTR);
        same_type(v, inst, &inst->b, IR_I64);
        break;
    case IR_MEMCOPY:
        same_type(v, inst, &inst->a, IR_PTR);
        same_type(v, inst, &inst->b, IR_PTR);
        vtype_ok(v, inst);
        break;
    case IR_SLOT:
        vtype_ok(v, inst);
        break;
    case IR_BITLOAD:
    case IR_BITSTORE:
        same_type(v, inst, inst->op == IR_BITLOAD ? &inst->a : &inst->b,
                  IR_PTR);
        if (inst->op == IR_BITSTORE) {
            same_type(v, inst, &inst->a, inst->type);
        }
        if (inst->of.type != IR_AGG || inst->of.agg >= v->m->agg_count ||
            inst->field >= v->m->aggs[inst->of.agg]->field_count ||
            v->m->aggs[inst->of.agg]->fields[inst->field].bits == 0) {
            fail(v, "%s names a field that is not a bitfield",
                 ir_op_name(inst->op));
        }
        break;
    case IR_CALL: {
        /* A direct call names its callee in a, a call through a pointer
           its signature in b. */
        const struct ir_operand *named =
            inst->b.kind == IR_FUNC ? &inst->b : &inst->a;
        if (inst->a.kind != IR_FUNC) {
            same_type(v, inst, &inst->a, IR_PTR);
        }
        if (named->kind == IR_FUNC && operand_ok(v, inst, named)) {
            const struct ir_function *callee = v->m->functions[named->as.index];
            if (callee->variadic ? inst->arg_count < callee->param_count
                                 : inst->arg_count != callee->param_count) {
                fail(v, "call passes %zu arguments to %s, which takes %zu",
                     inst->arg_count, callee->name, callee->param_count);
            }
        }
        break;
    }
    case IR_JUMP:
        operand_ok(v, inst, &inst->a);
        break;
    case IR_BRANCH:
        same_type(v, inst, &inst->a, IR_I8);
        operand_ok(v, inst, &inst->b);
        operand_ok(v, inst, &inst->c);
        break;
    case IR_RET:
        if (inst->type != result) {
            fail(v, "ret %s in a function that returns %s",
                 ir_type_name(inst->type), ir_type_name(result));
        } else if (inst->type != IR_VOID) {
            same_type(v, inst, &inst->a, inst->type);
        }
        break;
    default:
        operand_ok(v, inst, &inst->a);
        break;
    }
}

static bool is_set(const uint64_t *set, uint32_t temp)
{
    return (set[temp / 64] >> (temp % 64)) & 1;
}

static void set_bit(uint64_t *set, uint32_t temp)
{
    set[temp / 64] |= (uint64_t)1 << (temp % 64);
}

static void use(struct verifier *v, const struct ir_inst *inst,
                const struct ir_operand *o, const uint64_t *defined)
{
    if (o->kind == IR_TEMP && o->as.temp < v->f->temp_count &&
        !is_set(defined, o->as.temp)) {
        fail(v, "%s uses %%%" PRIu32 " before a definition on some path",
             ir_op_name(inst->op), o->as.temp);
    }
}

/* The temporaries defined on every path into block b: the parameters at
   the entry, intersected with what each predecessor defines on exit. */
static void entry_set(const struct ir_function *f, size_t b,
                      const uint64_t *out, size_t words, uint64_t *in)
{
    size_t i;
    size_t k;

    memset(in, b == 0 ? 0 : 0xff, words * sizeof *in);
    if (b == 0) {
        for (i = 0; i < f->param_count; i++) {
            set_bit(in, f->params[i].temp);
        }
    }
    for (i = 0; i < f->block_count; i++) {
        const struct ir_block *pred = f->blocks[i];
        const struct ir_inst *last;
        if (pred->count == 0) {
            continue;
        }
        last = &pred->insts[pred->count - 1];
        if ((last->op == IR_JUMP && last->a.as.index == b) ||
            (last->op == IR_BRANCH &&
             (last->b.as.index == b || last->c.as.index == b))) {
            for (k = 0; k < words; k++) {
                in[k] &= out[i * words + k];
            }
        }
    }
}

/* Every path from the entry to a use of a temporary passes a definition
   of it. The optimizer relies on that. A forward analysis starts every
   block with the full set and shrinks the sets until they stop changing.
   A block that no path reaches keeps the full set and reports nothing. */
static void check_definitions(struct verifier *v)
{
    const struct ir_function *f = v->f;
    size_t words = f->temp_count / 64 + 1;
    size_t n = f->block_count;
    uint64_t *out = malloc((n + 1) * words * sizeof *out);
    uint64_t *in = malloc(words * sizeof *in);
    bool changed = true;
    size_t b;
    size_t i;
    size_t k;

    if (out == NULL || in == NULL) {
        fputs("antic: out of memory\n", stderr);
        exit(70);
    }
    memset(out, 0xff, n * words * sizeof *out);
    while (changed) {
        changed = false;
        for (b = 0; b < n; b++) {
            entry_set(f, b, out, words, in);
            for (i = 0; i < f->blocks[b]->count; i++) {
                if (f->blocks[b]->insts[i].result < f->temp_count) {
                    set_bit(in, f->blocks[b]->insts[i].result);
                }
            }
            if (memcmp(in, out + b * words, words * sizeof *in) != 0) {
                memcpy(out + b * words, in, words * sizeof *in);
                changed = true;
            }
        }
    }
    for (b = 0; b < n; b++) {
        v->b = f->blocks[b];
        entry_set(f, b, out, words, in);
        for (i = 0; i < v->b->count; i++) {
            const struct ir_inst *inst = &v->b->insts[i];
            use(v, inst, &inst->a, in);
            use(v, inst, &inst->b, in);
            use(v, inst, &inst->c, in);
            for (k = 0; k < inst->arg_count; k++) {
                use(v, inst, &inst->args[k], in);
            }
            if (inst->result < f->temp_count) {
                set_bit(in, inst->result);
            }
        }
    }
    free(out);
    free(in);
}

bool ir_verify(const struct ir_module *m, struct text *errors)
{
    struct verifier v = {m, NULL, NULL, errors, true};
    size_t i;
    size_t j;
    size_t k;

    for (i = 0; i < m->function_count; i++) {
        v.f = m->functions[i];
        if (v.f->is_extern) {
            continue;
        }
        for (j = 0; j < v.f->block_count; j++) {
            const struct ir_block *b = v.f->blocks[j];
            v.b = b;
            for (k = 0; k < b->count; k++) {
                check_inst(&v, &b->insts[k]);
                if (is_terminator(b->insts[k].op) && k + 1 < b->count) {
                    fail(&v, "%s is not the last instruction",
                         ir_op_name(b->insts[k].op));
                }
            }
            if (b->count == 0 || !is_terminator(b->insts[b->count - 1].op)) {
                fail(&v, "the block does not end with a terminator");
            }
        }
        /* The analysis follows block operands, which must be valid. */
        if (v.ok) {
            check_definitions(&v);
        }
    }
    return v.ok;
}
