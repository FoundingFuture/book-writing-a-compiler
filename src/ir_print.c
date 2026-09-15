#include "ir.h"

#include <inttypes.h>
#include <string.h>

const char *ir_type_name(enum ir_type type)
{
    static const char *const names[] = {
        [IR_VOID] = "void", [IR_I8] = "i8", [IR_I16] = "i16",
        [IR_I32] = "i32", [IR_I64] = "i64", [IR_F32] = "f32",
        [IR_F64] = "f64", [IR_PTR] = "ptr", [IR_AGG] = "agg",
        [IR_CLONG] = "clong", [IR_CWCHAR] = "cwchar",
    };
    return names[type];
}

const char *ir_op_name(enum ir_op op)
{
    static const char *const names[] = {
        [IR_ADD] = "add", [IR_SUB] = "sub", [IR_MUL] = "mul",
        [IR_SDIV] = "sdiv", [IR_UDIV] = "udiv", [IR_SREM] = "srem",
        [IR_UREM] = "urem", [IR_AND] = "and", [IR_OR] = "or",
        [IR_XOR] = "xor", [IR_SHL] = "shl", [IR_SHR_S] = "sshr",
        [IR_SHR_U] = "ushr", [IR_FADD] = "fadd", [IR_FSUB] = "fsub",
        [IR_FMUL] = "fmul", [IR_FDIV] = "fdiv", [IR_NEG] = "neg",
        [IR_NOT] = "not", [IR_FNEG] = "fneg", [IR_COPY] = "copy",
        [IR_EQ] = "eq", [IR_NE] = "ne", [IR_SLT] = "slt", [IR_SLE] = "sle",
        [IR_SGT] = "sgt", [IR_SGE] = "sge", [IR_ULT] = "ult",
        [IR_ULE] = "ule", [IR_UGT] = "ugt", [IR_UGE] = "uge",
        [IR_FEQ] = "feq", [IR_FNE] = "fne", [IR_FLT] = "flt",
        [IR_FLE] = "fle", [IR_FGT] = "fgt", [IR_FGE] = "fge",
        [IR_TRUNC] = "trunc", [IR_SEXT] = "sext", [IR_ZEXT] = "zext",
        [IR_SITOF] = "sitof", [IR_UITOF] = "uitof", [IR_FTOSI] = "ftosi",
        [IR_FTOUI] = "ftoui", [IR_FEXT] = "fext", [IR_FTRUNC] = "ftrunc",
        [IR_SLOT] = "slot", [IR_LOAD] = "load", [IR_STORE] = "store",
        [IR_PTRADD] = "ptradd", [IR_MEMCOPY] = "memcopy", [IR_ADDR] = "addr",
        [IR_BITLOAD] = "bitload", [IR_BITSTORE] = "bitstore",
        [IR_CALL] = "call", [IR_JUMP] = "jump", [IR_BRANCH] = "branch",
        [IR_RET] = "ret",
    };
    return names[op];
}

static void symbol(struct text *out, const char *module, const char *name)
{
    if (module != NULL) {
        text_appendf(out, "@%s.%s", module, name);
    } else {
        text_appendf(out, "@%s", name);
    }
}

void ir_vtype_print(struct text *out, const struct ir_module *m,
                    struct ir_vtype v)
{
    if (v.type == IR_AGG) {
        text_append(out, m->aggs[v.agg]->name);
    } else {
        text_append(out, ir_type_name(v.type));
    }
}

static void integer(struct text *out, enum ir_type type, uint64_t value)
{
    if (type == IR_I64 || type == IR_PTR || type == IR_CLONG ||
        type == IR_CWCHAR) {
        text_appendf(out, "%" PRId64, (int64_t)value);
    } else if (type == IR_I32) {
        text_appendf(out, "%" PRId32, (int32_t)value);
    } else if (type == IR_I16) {
        text_appendf(out, "%d", (int16_t)value);
    } else {
        text_appendf(out, "%d", (int8_t)value);
    }
}

void ir_sym_print(struct text *out, const struct ir_module *m, uint32_t sym)
{
    const struct ir_sym *s = &m->syms[sym];

    switch (s->kind) {
    case IR_SYM_INT:
        integer(out, s->type, s->value);
        break;
    case IR_SYM_SIZE_OF:
        text_append(out, "size_of ");
        ir_vtype_print(out, m, s->of);
        break;
    case IR_SYM_OFFSET_OF:
        text_appendf(out, "offset_of %s.%s", m->aggs[s->of.agg]->name,
                     m->aggs[s->of.agg]->fields[s->field].name);
        break;
    case IR_SYM_OP:
        text_appendf(out, "%s %s(", ir_op_name((enum ir_op)s->op),
                     ir_type_name(s->type));
        ir_sym_print(out, m, s->a);
        if (s->b != IR_NO_AGG) {
            text_append(out, ", ");
            ir_sym_print(out, m, s->b);
        }
        text_append(out, ")");
        break;
    }
}

static void operand(struct text *out, const struct ir_module *m,
                    const struct ir_operand *o)
{
    switch (o->kind) {
    case IR_NONE:
        break;
    case IR_TEMP:
        text_appendf(out, "%%%" PRIu32, o->as.temp);
        break;
    case IR_INT:
        integer(out, o->type, o->as.integer);
        break;
    case IR_SYM:
        ir_sym_print(out, m, o->as.index);
        break;
    case IR_FLOAT:
        text_appendf(out, "%.17g", o->as.floating);
        break;
    case IR_GLOBAL:
        symbol(out, m->globals[o->as.index]->module,
               m->globals[o->as.index]->name);
        break;
    case IR_FUNC:
        symbol(out, m->functions[o->as.index]->module,
               m->functions[o->as.index]->name);
        break;
    case IR_BLOCK:
        text_appendf(out, "b%" PRIu32, o->as.index);
        break;
    }
}

static void param_type(struct text *out, const struct ir_module *m,
                       const struct ir_param *p)
{
    if (p->type == IR_AGG) {
        text_appendf(out, "agg %s", m->aggs[p->agg]->name);
    } else {
        text_append(out, ir_type_name(p->type));
    }
    if (p->ext != IR_EXT_NONE) {
        text_append(out, p->ext == IR_EXT_SIGN ? " signext" : " zeroext");
    }
}

static void signature(struct text *out, const struct ir_module *m,
                      const struct ir_function *f)
{
    size_t i;

    text_append(out, f->exported ? "export " : "");
    text_append(out, f->is_extern ? "extern fn " : "fn ");
    if (f->module != NULL) {
        text_appendf(out, "%s.", f->module);
    }
    text_appendf(out, "%s(", f->name);
    for (i = 0; i < f->param_count; i++) {
        if (i > 0) {
            text_append(out, ", ");
        }
        if (!f->is_extern) {
            text_appendf(out, "%%%" PRIu32 ": ", f->params[i].temp);
        }
        param_type(out, m, &f->params[i]);
    }
    if (f->variadic) {
        text_append(out, f->param_count > 0 ? ", ..." : "...");
    }
    text_append(out, ")");
    if (f->result == IR_AGG) {
        text_appendf(out, " -> agg %s", m->aggs[f->result_agg]->name);
    } else if (f->result != IR_VOID) {
        text_appendf(out, " -> %s", ir_type_name(f->result));
    }
}

static void instruction(struct text *out, const struct ir_module *m,
                        const struct ir_inst *inst)
{
    size_t i;

    text_append(out, "    ");
    if (inst->result != IR_NO_RESULT) {
        text_appendf(out, "%%%" PRIu32 " = ", inst->result);
    }
    text_append(out, ir_op_name(inst->op));
    switch (inst->op) {
    case IR_SLOT:
        text_append(out, " ");
        ir_vtype_print(out, m, inst->of);
        break;
    case IR_PTRADD:
    case IR_BRANCH:
        text_append(out, " ");
        operand(out, m, &inst->a);
        text_append(out, ", ");
        operand(out, m, &inst->b);
        if (inst->op == IR_BRANCH) {
            text_append(out, ", ");
            operand(out, m, &inst->c);
        }
        break;
    case IR_MEMCOPY:
        text_append(out, " ");
        operand(out, m, &inst->a);
        text_append(out, ", ");
        operand(out, m, &inst->b);
        text_append(out, ", ");
        ir_vtype_print(out, m, inst->of);
        break;
    case IR_ADDR:
    case IR_JUMP:
        text_append(out, " ");
        operand(out, m, &inst->a);
        break;
    case IR_BITLOAD:
    case IR_BITSTORE:
        text_appendf(out, " %s ", ir_type_name(inst->type));
        operand(out, m, &inst->a);
        if (inst->op == IR_BITSTORE) {
            text_append(out, ", ");
            operand(out, m, &inst->b);
        }
        text_appendf(out, ", %s.%s", m->aggs[inst->of.agg]->name,
                     m->aggs[inst->of.agg]->fields[inst->field].name);
        break;
    case IR_CALL:
        text_appendf(out, " %s ", ir_type_name(inst->type));
        operand(out, m, &inst->a);
        if (inst->b.kind == IR_FUNC) {
            text_append(out, " via ");
            operand(out, m, &inst->b);
        }
        text_append(out, "(");
        for (i = 0; i < inst->arg_count; i++) {
            if (i > 0) {
                text_append(out, ", ");
            }
            operand(out, m, &inst->args[i]);
        }
        text_append(out, ")");
        break;
    case IR_RET:
        if (inst->type != IR_VOID) {
            text_appendf(out, " %s ", ir_type_name(inst->type));
            operand(out, m, &inst->a);
        }
        break;
    default:
        text_appendf(out, " %s ", ir_type_name(inst->type));
        operand(out, m, &inst->a);
        if (inst->b.kind != IR_NONE) {
            text_append(out, ", ");
            operand(out, m, &inst->b);
        }
        break;
    }
    text_append(out, "\n");
}

/* A struct or union prints as type name = struct { field: type }, with
   one field: type per field. An array prints as type name = array length
   of element. */
static void aggtype(struct text *out, const struct ir_module *m,
                    const struct ir_aggtype *t)
{
    size_t i;

    text_appendf(out, "type %s = ", t->name);
    if (t->kind == IR_AGG_ARRAY) {
        text_append(out, "array ");
        ir_sym_print(out, m, t->length);
        text_append(out, " of ");
        ir_vtype_print(out, m, t->fields[0].type);
        text_append(out, "\n");
        return;
    }
    text_appendf(out, "%s%s", t->packed ? "packed " : "",
                 t->kind == IR_AGG_UNION ? "union" : "struct");
    if (t->align != 0) {
        text_appendf(out, " align(%" PRIu64 ")", t->align);
    }
    text_append(out, " {");
    for (i = 0; i < t->field_count; i++) {
        const struct ir_field *f = &t->fields[i];
        text_appendf(out, "%s %s: ", i > 0 ? "," : "", f->name);
        ir_vtype_print(out, m, f->type);
        if (f->bits != 0) {
            text_appendf(out, " : %u %s", (unsigned)f->bits,
                         f->ext == IR_EXT_SIGN ? "signext" : "zeroext");
        } else if (strcmp(f->name, "_") == 0) {
            text_append(out, " : 0");
        }
    }
    text_append(out, " }\n");
}

void ir_print(struct text *out, const struct ir_module *m)
{
    size_t i;
    size_t j;
    size_t k;

    for (i = 0; i < m->agg_count; i++) {
        aggtype(out, m, m->aggs[i]);
    }
    for (i = 0; i < m->function_count; i++) {
        if (m->functions[i]->is_extern) {
            signature(out, m, m->functions[i]);
            text_append(out, "\n");
        }
    }
    for (i = 0; i < m->global_count; i++) {
        const struct ir_global *g = m->globals[i];
        text_appendf(out, "global %s.%s", g->module, g->name);
        text_appendf(out, " size %" PRIu64 " align %" PRIu64 " bytes",
                     g->size, g->align);
        for (j = 0; j < g->size; j++) {
            text_appendf(out, " %02x", g->bytes[j]);
        }
        for (j = 0; j < g->reloc_count; j++) {
            text_appendf(out, " reloc %" PRIu64 " ", g->relocs[j].offset);
            symbol(out, m->globals[g->relocs[j].global]->module,
                   m->globals[g->relocs[j].global]->name);
        }
        text_append(out, "\n");
    }
    for (i = 0; i < m->function_count; i++) {
        const struct ir_function *f = m->functions[i];
        if (f->is_extern) {
            continue;
        }
        signature(out, m, f);
        text_append(out, " {\n");
        for (j = 0; j < f->block_count; j++) {
            text_appendf(out, "b%zu:\n", j);
            for (k = 0; k < f->blocks[j]->count; k++) {
                instruction(out, m, &f->blocks[j]->insts[k]);
            }
        }
        text_append(out, "}\n");
    }
}
