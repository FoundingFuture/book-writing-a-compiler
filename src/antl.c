#include "antl.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* DESIGN: the file is a flat sequence of little-endian integers of fixed
   width. A string is a u32 byte count and the bytes. A float is the u64
   of its IEEE 754 bits. No pointer, padding or host byte order reaches the
   file. It stores enum values of types.h, sema.h and ir.h. These checks
   fail when one of them changes, and the version changes with it. */
_Static_assert(TYPE_STRUCT == 23, "raise ANTL_VERSION, then update this");
_Static_assert(SYMBOL_GLOBAL == 7, "raise ANTL_VERSION, then update this");
_Static_assert(CONST_SYMBOLIC == 8, "raise ANTL_VERSION, then update this");
_Static_assert(SYMBOLIC_CAST == 4, "raise ANTL_VERSION, then update this");
_Static_assert(TOKEN_KIND_COUNT == 144, "raise ANTL_VERSION, then update this");
_Static_assert(IR_CWCHAR == 10, "raise ANTL_VERSION, then update this");
_Static_assert(IR_RET == 57, "raise ANTL_VERSION, then update this");
_Static_assert(IR_SYM == 7, "raise ANTL_VERSION, then update this");
_Static_assert(IR_EXT_ZERO == 2, "raise ANTL_VERSION, then update this");
_Static_assert(IR_CONST_AGG == 6, "raise ANTL_VERSION, then update this");
_Static_assert(IR_AGG_ARRAY == 2, "raise ANTL_VERSION, then update this");
_Static_assert(IR_SYM_OP == 3, "raise ANTL_VERSION, then update this");

static const uint8_t magic[4] = {'A', 'N', 'T', 'L'};

/* Writing */

struct writer {
    struct text *out;
    const struct interface *iface;
    bool strip_docs;
    const struct type **types;      /* the type table in index order */
    size_t type_count;
    size_t type_capacity;
};

static void put_u8(struct writer *w, uint8_t v)
{
    text_append_bytes(w->out, &v, 1);
}

static void put_u32(struct writer *w, uint32_t v)
{
    uint8_t b[4];
    int i;

    for (i = 0; i < 4; i++) {
        b[i] = (uint8_t)(v >> (8 * i));
    }
    text_append_bytes(w->out, b, sizeof b);
}

static void put_u64(struct writer *w, uint64_t v)
{
    uint8_t b[8];
    int i;

    for (i = 0; i < 8; i++) {
        b[i] = (uint8_t)(v >> (8 * i));
    }
    text_append_bytes(w->out, b, sizeof b);
}

static void put_bytes(struct writer *w, const char *s, size_t length)
{
    put_u32(w, (uint32_t)length);
    text_append_bytes(w->out, s, length);
}

static void put_str(struct writer *w, const char *s)
{
    put_bytes(w, s, s == NULL ? 0 : strlen(s));
}

/* Doc text, or nothing for --strip-docs. */
static void put_doc(struct writer *w, const char *text, size_t length)
{
    put_bytes(w, text, w->strip_docs || text == NULL ? 0 : length);
}

static uint64_t float_bits(double d)
{
    uint64_t bits;

    memcpy(&bits, &d, sizeof bits);
    return bits;
}

static bool is_local_struct(const struct writer *w, const struct type *t)
{
    const char *module = w->iface->module;

    return type_has_fields(t) && t->module.length == strlen(module) &&
           memcmp(t->module.text, module, t->module.length) == 0;
}

/* The count of functions of the body of t that another module may see.
   A private function is never one of them, because no other module can
   name it. A protected one is, because a class below it may. */
static size_t public_members(const struct type *t)
{
    size_t count = 0;
    size_t i;

    for (i = 0; i < t->member_count; i++) {
        const struct item *m = t->members[i];
        if (m->kind == ITEM_FN && m->vis != VIS_PRIVATE &&
            m->symbol != NULL) {
            count++;
        }
    }
    return count;
}

/* Whether t is a pub struct or union of the interface. */
static bool is_public_struct(const struct writer *w, const struct type *t)
{
    size_t i;

    for (i = 0; i < w->iface->item_count; i++) {
        if (w->iface->items[i]->kind == SYMBOL_STRUCT &&
            w->iface->items[i]->type == t) {
            return true;
        }
    }
    return false;
}

static long find_type(const struct writer *w, const struct type *t)
{
    size_t i;

    for (i = 0; i < w->type_count; i++) {
        if (w->types[i] == t) {
            return (long)i;
        }
    }
    return -1;
}

static void add_type(struct writer *w, const struct type *t)
{
    if (w->type_count == w->type_capacity) {
        size_t capacity = w->type_capacity == 0 ? 16 : w->type_capacity * 2;
        const struct type **types = realloc((void *)w->types,
                                            capacity * sizeof *types);
        if (types == NULL) {
            fputs("antic: out of memory\n", stderr);
            exit(70);
        }
        w->types = types;
        w->type_capacity = capacity;
    }
    w->types[w->type_count++] = t;
}

/* Give t and every type inside it an index. A type comes after the types
   it is built from. A struct comes before its field types, so a struct
   can hold a pointer to itself. */
static void visit_type(struct writer *w, const struct type *t);

/* The types a symbolic value names: its own and those it measures. */
static void visit_symbolic(struct writer *w, const struct symbolic *s)
{
    if (s == NULL) {
        return;
    }
    visit_type(w, s->type);
    if (s->of != NULL) {
        visit_type(w, s->of);
    }
    visit_symbolic(w, s->a);
    visit_symbolic(w, s->b);
}

static void visit_value(struct writer *w, const struct const_value *v)
{
    size_t i;

    if (v->kind == CONST_SYMBOLIC) {
        visit_symbolic(w, v->as.symbolic);
    } else if (v->kind == CONST_ARRAY || v->kind == CONST_STRUCT) {
        for (i = 0; i < v->as.aggregate.count; i++) {
            visit_value(w, &v->as.aggregate.items[i]);
        }
    }
}

static void visit_type(struct writer *w, const struct type *t)
{
    size_t i;

    if (find_type(w, t) >= 0) {
        return;
    }
    switch (t->kind) {
    case TYPE_ARRAY:
        visit_symbolic(w, t->length_of);
        visit_type(w, t->element);
        break;
    case TYPE_POINTER:
    case TYPE_SLICE:
        visit_type(w, t->element);
        break;
    case TYPE_FN:
        for (i = 0; i < t->param_count; i++) {
            visit_type(w, t->params[i]);
        }
        visit_type(w, t->result);
        break;
    case TYPE_STRUCT:
    case TYPE_CLASS:
        add_type(w, t);
        if (is_local_struct(w, t)) {
            for (i = 0; i < t->field_count; i++) {
                visit_type(w, t->fields[i].type);
            }
            for (i = 0; i < t->member_count; i++) {
                const struct item *m = t->members[i];
                if (m->kind == ITEM_FN && m->vis != VIS_PRIVATE &&
            m->symbol != NULL) {
                    visit_type(w, m->symbol->type);
                }
            }
        }
        return;
    /* The underlying integer comes first, because an enum names it by
       index and a reference reaches back only. */
    case TYPE_ENUM:
        visit_type(w, t->base);
        add_type(w, t);
        return;
    default:
        break;
    }
    add_type(w, t);
}

static void put_type_ref(struct writer *w, const struct type *t)
{
    put_u32(w, (uint32_t)find_type(w, t));
}

static void put_symbolic(struct writer *w, const struct symbolic *s)
{
    put_u8(w, (uint8_t)s->kind);
    put_type_ref(w, s->type);
    switch (s->kind) {
    case SYMBOLIC_INT:
        put_u64(w, s->value);
        break;
    case SYMBOLIC_SIZE_OF:
        put_type_ref(w, s->of);
        break;
    case SYMBOLIC_UNARY:
    case SYMBOLIC_CAST:
        put_u8(w, (uint8_t)s->op);
        put_symbolic(w, s->a);
        break;
    case SYMBOLIC_BINARY:
        put_u8(w, (uint8_t)s->op);
        put_symbolic(w, s->a);
        put_symbolic(w, s->b);
        break;
    }
}

static void put_type(struct writer *w, const struct type *t)
{
    size_t i;

    put_u8(w, (uint8_t)t->kind);
    switch (t->kind) {
    case TYPE_POINTER:
    case TYPE_SLICE:
        put_type_ref(w, t->element);
        break;
    case TYPE_ARRAY:
        put_type_ref(w, t->element);
        put_u8(w, t->length_of != NULL);
        if (t->length_of != NULL) {
            put_symbolic(w, t->length_of);
        } else {
            put_u64(w, t->length);
        }
        break;
    case TYPE_FN:
        put_u32(w, (uint32_t)t->param_count);
        for (i = 0; i < t->param_count; i++) {
            put_type_ref(w, t->params[i]);
        }
        put_type_ref(w, t->result);
        break;
    /* DESIGN: a class is written like a struct, with the form and the
       own bit of each field. Its base is the type of field 0, so the
       chain follows the ordinary type references. */
    case TYPE_STRUCT:
    case TYPE_CLASS:
        put_bytes(w, t->module.text, t->module.length);
        put_bytes(w, t->name.text, t->name.length);
        if (is_local_struct(w, t)) {
            put_u8(w, (uint8_t)((unsigned)t->is_union |
                                (unsigned)t->packed << 1 |
                                (unsigned)t->has_abstract << 2 |
                                (unsigned)t->is_final << 3));
            put_u64(w, t->align);
            put_u32(w, (uint32_t)t->field_count);
            for (i = 0; i < t->field_count; i++) {
                put_bytes(w, t->fields[i].name.text, t->fields[i].name.length);
                put_type_ref(w, t->fields[i].type);
                put_u8(w, t->fields[i].bits);
                put_u8(w, (uint8_t)((unsigned)t->fields[i].form |
                                    (unsigned)t->fields[i].owned << 4 |
                                    (unsigned)t->fields[i].atomic << 5 |
                                    (unsigned)t->fields[i].writable << 6));
                put_u8(w, (uint8_t)t->fields[i].vis);
                /* DESIGN: /// on a private item is never stored, and
                   the fields of a private struct are private items. */
                put_doc(w, t->fields[i].doc.text,
                        is_public_struct(w, t) ? t->fields[i].doc.length : 0);
            }
            /* The public functions of the body, so a call on a value of
               another module resolves and reaches the right symbol. */
            put_u32(w, (uint32_t)public_members(t));
            for (i = 0; i < t->member_count; i++) {
                const struct item *m = t->members[i];
                if (m->kind != ITEM_FN || m->vis == VIS_PRIVATE ||
                    m->symbol == NULL) {
                    continue;
                }
                put_bytes(w, m->name.text, m->name.length);
                put_type_ref(w, m->symbol->type);
                put_u8(w, (uint8_t)((unsigned)m->contract |
                                    (unsigned)m->is_final << 4 |
                                    (unsigned)m->is_operator << 5));
                put_u8(w, (uint8_t)m->vis);
                put_doc(w, m->doc.text, m->doc.length);
            }
        }
        break;
    /* An enum is its module, its name, its underlying integer and the
       name and number of each value. */
    case TYPE_ENUM:
        put_bytes(w, t->module.text, t->module.length);
        put_bytes(w, t->name.text, t->name.length);
        put_type_ref(w, t->base);
        put_u32(w, (uint32_t)t->field_count);
        for (i = 0; i < t->field_count; i++) {
            put_bytes(w, t->fields[i].name.text, t->fields[i].name.length);
            put_u64(w, t->fields[i].number);
            put_doc(w, t->fields[i].doc.text, t->fields[i].doc.length);
        }
        break;
    default:
        break;
    }
}

static void put_value(struct writer *w, const struct const_value *v)
{
    size_t i;

    put_u8(w, (uint8_t)v->kind);
    switch (v->kind) {
    case CONST_INT:
        put_u64(w, v->as.integer);
        break;
    case CONST_FLOAT:
        put_u64(w, float_bits(v->as.floating));
        break;
    case CONST_BOOL:
        put_u8(w, v->as.boolean);
        break;
    case CONST_CHAR:
        put_u32(w, v->as.character);
        break;
    case CONST_NULL:
        break;
    case CONST_TEXT:
        put_bytes(w, v->as.text.bytes, v->as.text.length);
        break;
    case CONST_ARRAY:
    case CONST_STRUCT:
        put_u32(w, (uint32_t)v->as.aggregate.count);
        for (i = 0; i < v->as.aggregate.count; i++) {
            put_value(w, &v->as.aggregate.items[i]);
        }
        break;
    case CONST_SYMBOLIC:
        put_symbolic(w, v->as.symbolic);
        break;
    }
}

static void put_vtype(struct writer *w, struct ir_vtype v)
{
    put_u8(w, (uint8_t)v.type);
    put_u32(w, v.agg);
}

/* An aggregate constant, as the tree the back end lays out. Its type and
   its symbolic values are written as indices, which the reader maps. */
static void put_const(struct writer *w, const struct ir_const *c)
{
    size_t i;

    put_u8(w, (uint8_t)c->kind);
    put_u8(w, (uint8_t)c->scalar);
    switch (c->kind) {
    case IR_CONST_INT:
        put_u64(w, c->integer);
        break;
    case IR_CONST_FLOAT:
        put_u64(w, float_bits(c->floating));
        break;
    case IR_CONST_SYM:
        put_u64(w, c->sym);
        break;
    case IR_CONST_ADDR:
    case IR_CONST_FUNC:
        put_u64(w, c->global);
        break;
    case IR_CONST_AGG:
        put_u64(w, c->item_count);
        put_vtype(w, c->type);
        for (i = 0; i < c->item_count; i++) {
            put_const(w, &c->items[i]);
        }
        break;
    default: /* IR_CONST_NONE */
        put_u64(w, 0);
        break;
    }
}

static void put_operand(struct writer *w, const struct ir_operand *o)
{
    put_u8(w, (uint8_t)o->kind);
    put_u8(w, (uint8_t)o->type);
    switch (o->kind) {
    case IR_TEMP:
        put_u64(w, o->as.temp);
        break;
    case IR_INT:
        put_u64(w, o->as.integer);
        break;
    case IR_FLOAT:
        put_u64(w, float_bits(o->as.floating));
        break;
    case IR_GLOBAL:
    case IR_FUNC:
    case IR_BLOCK:
    case IR_SYM:
        put_u64(w, o->as.index);
        break;
    default:
        put_u64(w, 0);
        break;
    }
}

static void put_inst(struct writer *w, const struct ir_inst *inst)
{
    size_t i;

    put_u8(w, (uint8_t)inst->op);
    put_u8(w, (uint8_t)inst->type);
    put_u32(w, inst->result);
    put_operand(w, &inst->a);
    put_operand(w, &inst->b);
    put_operand(w, &inst->c);
    put_vtype(w, inst->of);
    put_u32(w, inst->field);
    put_u32(w, (uint32_t)inst->arg_count);
    for (i = 0; i < inst->arg_count; i++) {
        put_operand(w, &inst->args[i]);
    }
}

static void put_ir(struct writer *w, const struct ir_module *ir)
{
    size_t i;
    size_t j;
    size_t k;

    put_u32(w, (uint32_t)ir->sym_count);
    for (i = 0; i < ir->sym_count; i++) {
        const struct ir_sym *sym = &ir->syms[i];
        put_u8(w, (uint8_t)sym->kind);
        put_u8(w, (uint8_t)sym->type);
        put_u64(w, sym->value);
        put_vtype(w, sym->of);
        put_u32(w, sym->field);
        put_u8(w, (uint8_t)sym->op);
        put_u32(w, sym->a);
        put_u32(w, sym->b);
    }
    put_u32(w, (uint32_t)ir->agg_count);
    for (i = 0; i < ir->agg_count; i++) {
        const struct ir_aggtype *t = ir->aggs[i];
        put_u8(w, (uint8_t)t->kind);
        put_str(w, t->name);
        put_u8(w, t->packed);
        put_u64(w, t->align);
        put_u32(w, t->length);
        put_str(w, t->length_text);
        put_u32(w, (uint32_t)t->field_count);
        for (j = 0; j < t->field_count; j++) {
            put_str(w, t->fields[j].name);
            put_vtype(w, t->fields[j].type);
            put_u8(w, t->fields[j].bits);
            put_u8(w, (uint8_t)t->fields[j].ext);
        }
    }
    put_u32(w, (uint32_t)ir->global_count);
    for (i = 0; i < ir->global_count; i++) {
        const struct ir_global *g = ir->globals[i];
        put_str(w, g->module);
        put_str(w, g->name);
        put_u64(w, g->size);
        put_u64(w, g->align);
        if (g->size > 0) {
            text_append_bytes(w->out, g->bytes, g->size);
        }
        put_u32(w, (uint32_t)g->reloc_count);
        for (j = 0; j < g->reloc_count; j++) {
            put_u64(w, g->relocs[j].offset);
            put_u32(w, g->relocs[j].global);
            put_u8(w, g->relocs[j].fn ? 1 : 0);
        }
        put_u8(w, (uint8_t)((g->value != NULL ? 1 : 0) |
                            (g->exported ? 2 : 0) |
                            (g->is_extern ? 4 : 0) |
                            (g->mutable ? 8 : 0)));
        if (g->value != NULL) {
            put_const(w, g->value);
        }
    }
    /* All signatures come before the first body, so a body can call a
       function that the file lists later. */
    put_u32(w, (uint32_t)ir->function_count);
    for (i = 0; i < ir->function_count; i++) {
        const struct ir_function *f = ir->functions[i];
        put_u8(w, (uint8_t)((f->is_extern ? 1 : 0) | (f->variadic ? 2 : 0) |
                            (f->exported ? 4 : 0)));
        put_str(w, f->module);
        put_str(w, f->name);
        put_u8(w, (uint8_t)f->result);
        put_u32(w, f->result_agg);
        put_u32(w, (uint32_t)f->param_count);
        for (j = 0; j < f->param_count; j++) {
            put_u8(w, (uint8_t)f->params[j].type);
            put_u8(w, (uint8_t)f->params[j].ext);
            put_u32(w, f->params[j].agg);
        }
    }
    for (i = 0; i < ir->function_count; i++) {
        const struct ir_function *f = ir->functions[i];
        if (f->is_extern) {
            continue;
        }
        put_u32(w, f->temp_count);
        for (j = 0; j < f->temp_count; j++) {
            put_u8(w, (uint8_t)f->temps[j]);
        }
        put_u32(w, (uint32_t)f->block_count);
        for (j = 0; j < f->block_count; j++) {
            /* The failure block of an assertion carries its flag, so the
               build that compiles the program can still drop it. */
            put_u8(w, f->blocks[j]->assert_fail);
            put_u32(w, (uint32_t)f->blocks[j]->count);
            for (k = 0; k < f->blocks[j]->count; k++) {
                put_inst(w, &f->blocks[j]->insts[k]);
            }
        }
    }
}

/* The magic, the version, the package header, the module path, the
   imports and the module's doc text. */
static void put_header(struct writer *w, const struct interface *iface)
{
    const struct package *p = &iface->package;
    struct text *out = w->out;
    size_t i;

    text_append_bytes(out, magic, sizeof magic);
    put_u32(w, ANTL_VERSION);
    put_str(w, p->name != NULL ? p->name : iface->module);
    put_str(w, p->version != NULL ? p->version : "0.0.0");
    put_u32(w, (uint32_t)p->dependency_count);
    for (i = 0; i < p->dependency_count; i++) {
        put_str(w, p->dependencies[i].name);
        put_str(w, p->dependencies[i].constraint);
        put_str(w, p->dependencies[i].url);
    }
    put_str(w, p->license);
    put_str(w, p->license_text);
    put_u32(w, (uint32_t)p->attribution_count);
    for (i = 0; i < p->attribution_count; i++) {
        put_str(w, p->attribution[i]);
    }
    put_str(w, iface->module);
    put_u32(w, (uint32_t)iface->import_count);
    for (i = 0; i < iface->import_count; i++) {
        put_str(w, iface->imports[i]);
    }
    put_doc(w, iface->doc, iface->doc != NULL ? strlen(iface->doc) : 0);
}

void antl_write_header(struct text *out, const struct interface *iface)
{
    struct writer w;

    memset(&w, 0, sizeof w);
    w.out = out;
    w.iface = iface;
    put_header(&w, iface);
}

void antl_write(struct text *out, const struct interface *iface,
                const struct ir_module *ir, bool strip_docs)
{
    struct writer w;
    size_t i;

    memset(&w, 0, sizeof w);
    w.out = out;
    w.iface = iface;
    w.strip_docs = strip_docs;
    for (i = 0; i < iface->item_count; i++) {
        visit_type(&w, iface->items[i]->type);
        if (iface->items[i]->kind == SYMBOL_CONST) {
            visit_value(&w, iface->items[i]->value);
        }
    }
    put_header(&w, iface);
    put_u32(&w, (uint32_t)w.type_count);
    for (i = 0; i < w.type_count; i++) {
        put_type(&w, w.types[i]);
    }
    put_u32(&w, (uint32_t)iface->item_count);
    for (i = 0; i < iface->item_count; i++) {
        const struct symbol *sym = iface->items[i];
        put_u8(&w, (uint8_t)sym->kind);
        put_bytes(&w, sym->name.text, sym->name.length);
        put_type_ref(&w, sym->type);
        put_u8(&w, (uint8_t)((unsigned)sym->exported |
                             (unsigned)sym->internal << 1));
        put_doc(&w, sym->doc.text, sym->doc.length);
        if (sym->kind == SYMBOL_FN || sym->kind == SYMBOL_EXTERN_FN) {
            size_t j;
            for (j = 0; j < sym->type->param_count; j++) {
                put_bytes(&w, sym->params[j].text, sym->params[j].length);
            }
        }
        if (sym->kind == SYMBOL_EXTERN_FN) {
            put_u8(&w, sym->variadic);
        } else if (sym->kind == SYMBOL_CONST) {
            put_value(&w, sym->value);
        }
    }
    put_ir(&w, ir);
    free((void *)w.types);
}

/* Reading */

struct reader {
    const uint8_t *data;
    size_t size;
    size_t pos;
    bool failed;
    char *error;
    size_t error_size;
    struct arena *arena;
    struct types *types;
    const struct interface *const *libraries;
    size_t library_count;
    struct interface *iface;
    struct type **table;
    uint32_t table_count;
};

static void fail(struct reader *r, const char *format, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

static void fail(struct reader *r, const char *format, ...)
{
    va_list args;

    if (r->failed) {
        return;
    }
    va_start(args, format);
    vsnprintf(r->error, r->error_size, format, args);
    va_end(args);
    r->failed = true;
}

static void damaged(struct reader *r)
{
    fail(r, "is damaged at byte %zu", r->pos);
}

static bool take(struct reader *r, size_t n)
{
    if (r->failed || n > r->size - r->pos) {
        if (!r->failed) {
            r->pos = r->size;
            damaged(r);
        }
        return false;
    }
    return true;
}

static uint64_t get_uint(struct reader *r, int bytes)
{
    uint64_t v = 0;
    int i;

    if (!take(r, (size_t)bytes)) {
        return 0;
    }
    for (i = 0; i < bytes; i++) {
        v |= (uint64_t)r->data[r->pos + (size_t)i] << (8 * i);
    }
    r->pos += (size_t)bytes;
    return v;
}

static uint8_t get_u8(struct reader *r)
{
    return (uint8_t)get_uint(r, 1);
}

static uint32_t get_u32(struct reader *r)
{
    return (uint32_t)get_uint(r, 4);
}

static uint64_t get_u64(struct reader *r)
{
    return get_uint(r, 8);
}

/* A count of records that each take at least min bytes. A larger count
   cannot fit in the rest of the file, which keeps a damaged count from
   causing a huge allocation. */
static uint32_t get_count(struct reader *r, size_t min)
{
    uint32_t n = get_u32(r);

    if (!r->failed && n > (r->size - r->pos) / min) {
        damaged(r);
        return 0;
    }
    return n;
}

static void *allocate(struct reader *r, size_t count, size_t size)
{
    return arena_alloc(r->arena, (count + 1) * size);
}

/* A string as a name that points into the memory pool. */
static struct name get_name(struct reader *r)
{
    struct name n = {"", 0};
    uint32_t length = get_count(r, 1);
    char *text;

    if (r->failed || !take(r, length)) {
        return n;
    }
    text = allocate(r, length, 1);
    memcpy(text, r->data + r->pos, length);
    r->pos += length;
    n.text = text;
    n.length = length;
    return n;
}

static const char *get_cstr(struct reader *r)
{
    return get_name(r).text;
}

static bool name_equals(const struct name *n, const char *s)
{
    return n->length == strlen(s) && memcmp(n->text, s, n->length) == 0;
}

static const struct interface *library(const struct reader *r,
                                       const struct name *module)
{
    size_t i;

    for (i = 0; i < r->library_count; i++) {
        if (name_equals(module, r->libraries[i]->module)) {
            return r->libraries[i];
        }
    }
    return NULL;
}

static bool read_magic(struct reader *r)
{
    uint32_t version;

    if (r->size < sizeof magic || memcmp(r->data, magic, sizeof magic) != 0) {
        fail(r, "is not a library file");
        return false;
    }
    r->pos = sizeof magic;
    version = get_u32(r);
    if (!r->failed && version != ANTL_VERSION) {
        fail(r, "has format version %u, and antic reads version %u",
             (unsigned)version, (unsigned)ANTL_VERSION);
    }
    return !r->failed;
}

static void read_header(struct reader *r, struct interface *out)
{
    struct package *p = &out->package;
    struct package_dependency *deps;
    const char **lines;
    uint32_t i;

    memset(out, 0, sizeof *out);
    if (!read_magic(r)) {
        return;
    }
    p->name = get_cstr(r);
    p->version = get_cstr(r);
    p->dependency_count = get_count(r, 12);
    deps = allocate(r, p->dependency_count, sizeof *deps);
    for (i = 0; i < p->dependency_count && !r->failed; i++) {
        deps[i].name = get_cstr(r);
        deps[i].constraint = get_cstr(r);
        deps[i].url = get_cstr(r);
    }
    p->dependencies = deps;
    p->license = get_cstr(r);
    p->license_text = get_cstr(r);
    p->attribution_count = get_count(r, 4);
    lines = allocate(r, p->attribution_count, sizeof *lines);
    for (i = 0; i < p->attribution_count && !r->failed; i++) {
        lines[i] = get_cstr(r);
    }
    p->attribution = lines;
    out->module = get_cstr(r);
    out->import_count = get_count(r, 4);
    out->imports = allocate(r, out->import_count, sizeof *out->imports);
    for (i = 0; i < out->import_count && !r->failed; i++) {
        out->imports[i] = get_cstr(r);
    }
    out->doc = get_cstr(r);
}

bool antl_header(const uint8_t *data, size_t size, struct arena *arena,
                 struct interface *out, char *error, size_t error_size)
{
    struct reader r;

    memset(&r, 0, sizeof r);
    r.data = data;
    r.size = size;
    r.error = error;
    r.error_size = error_size;
    r.arena = arena;
    read_header(&r, out);
    return !r.failed;
}

static struct type *type_ref(struct reader *r, uint32_t limit)
{
    uint32_t index = get_u32(r);

    if (r->failed || index >= limit) {
        damaged(r);
        return NULL;
    }
    return r->table[index];
}

static bool name_equals_name(const struct name *a, const struct name *b)
{
    return a->length == b->length &&
           memcmp(a->text, b->text, a->length) == 0;
}

/* A struct of another module is the struct that module's library file
   declared. */
static struct type *foreign_struct(struct reader *r, const struct name *module,
                                   const struct name *name)
{
    static const struct name root_module = {"anti.rt", 7};
    static const struct name root_name = {"Object", 6};
    const struct interface *lib;
    size_t i;

    /* The root of every class chain is the compiler's own, not a module
       any library file declares. */
    if (name_equals_name(module, &root_module) &&
        name_equals_name(name, &root_name)) {
        return types_object(r->types);
    }
    lib = library(r, module);
    if (lib == NULL) {
        fail(r, "needs module `%.*s`", (int)module->length, module->text);
        return NULL;
    }
    for (i = 0; i < lib->item_count; i++) {
        const struct symbol *sym = lib->items[i];
        if (sym->kind == SYMBOL_STRUCT && sym->name.length == name->length &&
            memcmp(sym->name.text, name->text, name->length) == 0) {
            return sym->type;
        }
    }
    fail(r, "needs struct `%.*s.%.*s`", (int)module->length, module->text,
         (int)name->length, name->text);
    return NULL;
}

static bool symbolic_op_ok(enum symbolic_kind kind, uint8_t op)
{
    switch (kind) {
    case SYMBOLIC_UNARY:
        return op == TOKEN_MINUS || op == TOKEN_TILDE || op == TOKEN_BANG;
    case SYMBOLIC_CAST:
        return op == TOKEN_AS;
    default:
        return (op >= TOKEN_PLUS && op <= TOKEN_CARET) ||
               (op >= TOKEN_SHL && op <= TOKEN_GE);
    }
}

/* A symbolic value whose types are the first limit entries of the type
   table. */
static const struct symbolic *read_symbolic(struct reader *r, uint32_t limit,
                                            int depth)
{
    struct symbolic key;
    uint8_t kind = get_u8(r);

    memset(&key, 0, sizeof key);
    key.kind = (enum symbolic_kind)kind;
    key.type = type_ref(r, limit);
    if (r->failed || kind > SYMBOLIC_CAST || depth > 64) {
        damaged(r);
        return NULL;
    }
    switch (key.kind) {
    case SYMBOLIC_INT:
        key.value = get_u64(r);
        break;
    case SYMBOLIC_SIZE_OF:
        key.of = type_ref(r, limit);
        break;
    case SYMBOLIC_UNARY:
    case SYMBOLIC_CAST:
    case SYMBOLIC_BINARY:
        key.op = (enum token_kind)get_u8(r);
        if (!r->failed && !symbolic_op_ok(key.kind, (uint8_t)key.op)) {
            damaged(r);
            return NULL;
        }
        key.a = read_symbolic(r, limit, depth + 1);
        if (key.kind == SYMBOLIC_BINARY && !r->failed) {
            key.b = read_symbolic(r, limit, depth + 1);
        }
        break;
    }
    if (r->failed || !(type_is_integer(key.type) || key.type->kind == TYPE_BOOL)) {
        damaged(r);
        return NULL;
    }
    return types_symbolic(r->types, &key);
}

struct field_refs {
    struct type *s;
    uint32_t count;
    struct struct_field *fields;
    uint32_t *types;
    /* The public functions of a class body, with the index of the
       function type of each. */
    uint32_t member_count;
    struct item **members;
    uint32_t *member_types;
};

static void read_types(struct reader *r)
{
    uint32_t count = get_count(r, 1);
    struct field_refs *structs = calloc((size_t)count + 1, sizeof *structs);
    uint32_t struct_count = 0;
    uint32_t i;
    uint32_t j;

    if (structs == NULL) {
        fputs("antic: out of memory\n", stderr);
        exit(70);
    }
    r->table = allocate(r, count, sizeof *r->table);
    for (i = 0; i < count && !r->failed; i++) {
        uint8_t kind = get_u8(r);
        struct type *t = NULL;
        switch (kind) {
        case TYPE_POINTER:
        case TYPE_SLICE:
            t = type_ref(r, i);
            if (t != NULL) {
                t = kind == TYPE_POINTER ? types_pointer(r->types, t)
                                         : types_slice(r->types, t);
            }
            break;
        case TYPE_ARRAY: {
            struct type *element = type_ref(r, i);
            if (get_u8(r) != 0) {
                const struct symbolic *length = read_symbolic(r, i, 0);
                if (element != NULL && length != NULL) {
                    t = types_array_symbolic(r->types, element, length);
                }
            } else {
                uint64_t length = get_u64(r);
                if (element != NULL && !r->failed && length > 0) {
                    t = types_array(r->types, element, length);
                }
            }
            break;
        }
        case TYPE_FN: {
            uint32_t n = get_count(r, 4);
            struct type **params = allocate(r, n, sizeof *params);
            for (j = 0; j < n && !r->failed; j++) {
                params[j] = type_ref(r, i);
            }
            t = type_ref(r, i);
            if (!r->failed) {
                t = types_fn(r->types, params, n, t);
            }
            break;
        }
        case TYPE_STRUCT:
        case TYPE_CLASS: {
            struct name module = get_name(r);
            struct name name = get_name(r);
            uint8_t flags;
            if (r->failed) {
                break;
            }
            if (!name_equals(&module, r->iface->module)) {
                t = foreign_struct(r, &module, &name);
                break;
            }
            struct field_refs *s = &structs[struct_count++];
            t = types_struct(r->types, module, name);
            t->kind = (enum type_kind)kind;
            flags = get_u8(r);
            t->is_union = (flags & 1) != 0;
            t->packed = (flags & 2) != 0;
            t->has_abstract = (flags & 4) != 0;
            t->is_final = (flags & 8) != 0;
            t->align = get_u64(r);
            if (flags > 15 || (t->align & (t->align - 1)) != 0) {
                damaged(r);
            }
            s->s = t;
            s->count = get_count(r, 9);
            s->fields = allocate(r, s->count, sizeof *s->fields);
            s->types = allocate(r, s->count, sizeof *s->types);
            for (j = 0; j < s->count && !r->failed; j++) {
                struct name doc;
                uint8_t form;
                s->fields[j].name = get_name(r);
                s->types[j] = get_u32(r);
                s->fields[j].bits = get_u8(r);
                form = get_u8(r);
                s->fields[j].form = (enum field_form)(form & 15);
                s->fields[j].owned = (form >> 4 & 1) != 0;
                s->fields[j].atomic = (form >> 5 & 1) != 0;
                s->fields[j].writable = (form >> 6 & 1) != 0;
                s->fields[j].vis = (enum visibility)get_u8(r);
                if ((form & 15) > FIELD_IMPL || s->fields[j].vis > VIS_PUB) {
                    damaged(r);
                }
                doc = get_name(r);
                s->fields[j].doc.text = doc.text;
                s->fields[j].doc.length = doc.length;
            }
            if (s->count == 0) {
                damaged(r);
            }
            s->member_count = get_count(r, 8);
            s->members = allocate(r, s->member_count, sizeof *s->members);
            s->member_types =
                allocate(r, s->member_count, sizeof *s->member_types);
            for (j = 0; j < s->member_count && !r->failed; j++) {
                struct item *m = arena_alloc(r->arena, sizeof *m);
                struct symbol *sym = arena_alloc(r->arena, sizeof *sym);
                uint8_t marks;
                struct name note;
                memset(m, 0, sizeof *m);
                memset(sym, 0, sizeof *sym);
                m->name = get_name(r);
                s->member_types[j] = get_u32(r);
                marks = get_u8(r);
                m->contract = (enum fn_contract)(marks & 15);
                m->is_final = (marks >> 4 & 1) != 0;
                m->is_operator = (marks >> 5 & 1) != 0;
                m->vis = (enum visibility)get_u8(r);
                if ((marks & 15) > FN_CONCRETE || m->vis > VIS_PUB) {
                    damaged(r);
                }
                note = get_name(r);
                m->doc.text = note.text;
                m->doc.length = note.length;
                m->kind = ITEM_FN;
                m->pub = m->vis == VIS_PUB;
                m->has_self = true;
                m->symbol = sym;
                sym->kind = SYMBOL_FN;
                sym->item = m;
                sym->home = r->iface;
                sym->doc = m->doc;
                s->members[j] = m;
            }
            break;
        }
        /* An enum carries its values, each with a name and a number. */
        case TYPE_ENUM: {
            struct name module = get_name(r);
            struct name name = get_name(r);
            struct type *base;
            uint32_t n;
            struct struct_field *values;
            if (r->failed) {
                break;
            }
            base = type_ref(r, i);
            n = get_count(r, 8);
            values = allocate(r, n, sizeof *values);
            t = base != NULL ? types_enum(r->types, module, name, base) : NULL;
            for (j = 0; j < n && !r->failed; j++) {
                struct name doc;
                memset(&values[j], 0, sizeof values[j]);
                values[j].name = get_name(r);
                values[j].number = get_u64(r);
                values[j].type = t;
                doc = get_name(r);
                values[j].doc.text = doc.text;
                values[j].doc.length = doc.length;
            }
            if (n == 0 || t == NULL) {
                damaged(r);
            } else if (!r->failed) {
                types_set_fields(r->types, t, values, n);
            }
            break;
        }
        default:
            if (kind < TYPE_NULL) {
                t = types_builtin(r->types, (enum type_kind)kind);
            } else {
                damaged(r);
            }
            break;
        }
        if (t == NULL) {
            damaged(r);
        }
        r->table[i] = t;
    }
    r->table_count = r->failed ? 0 : count;
    for (i = 0; i < struct_count && !r->failed; i++) {
        struct field_refs *s = &structs[i];
        for (j = 0; j < s->count; j++) {
            if (s->types[j] >= count ||
                r->table[s->types[j]]->kind == TYPE_VOID) {
                damaged(r);
                break;
            }
            s->fields[j].type = r->table[s->types[j]];
        }
        if (!r->failed) {
            types_set_fields(r->types, s->s, s->fields, s->count);
            /* The base of a class is the type of its field 0, so the
               chain is whole once the field types are in place. */
            if (s->s->kind == TYPE_CLASS && s->count > 0 &&
                s->fields[0].form == FIELD_BASE) {
                s->s->base = s->fields[0].type;
            }
        }
        /* DESIGN: a class of a library carries the public functions of
           its body. The reader builds one item per function, with the
           name `T.f` that its symbol has, so a call resolves and reaches
           the symbol the library defines. */
        for (j = 0; j < s->member_count && !r->failed; j++) {
            struct item *m = s->members[j];
            size_t length = s->s->name.length + 1 + m->name.length;
            char *qualified = arena_alloc(r->arena, length + 1);
            if (s->member_types[j] >= count) {
                damaged(r);
                break;
            }
            m->symbol->type = r->table[s->member_types[j]];
            memcpy(qualified, s->s->name.text, s->s->name.length);
            qualified[s->s->name.length] = '.';
            memcpy(qualified + s->s->name.length + 1, m->name.text,
                   m->name.length);
            qualified[length] = '\0';
            m->symbol->name.text = qualified;
            m->symbol->name.length = length;
        }
        if (!r->failed && s->member_count > 0) {
            s->s->members = s->members;
            s->s->member_count = s->member_count;
        }
    }
    for (i = 0; i < struct_count && !r->failed; i++) {
        if (types_find_cycle(structs[i].s) != NULL) {
            damaged(r);
        }
    }
    free(structs);
}

/* A constant of type t. Aggregates hold one value per element or field,
   which the constant evaluator relies on. */
static bool read_value(struct reader *r, struct type *t, struct const_value *v,
                       int depth)
{
    uint8_t kind = get_u8(r);
    uint32_t i;
    uint32_t n;

    memset(v, 0, sizeof *v);
    v->type = t;
    v->kind = (enum const_kind)kind;
    if (r->failed || depth > 64) {
        damaged(r);
        return false;
    }
    switch (kind) {
    case CONST_INT:
        v->as.integer = get_u64(r);
        return !r->failed && (type_is_integer(t) || t->kind == TYPE_BOOL);
    case CONST_FLOAT: {
        uint64_t bits = get_u64(r);
        memcpy(&v->as.floating, &bits, sizeof bits);
        return !r->failed && type_is_float(t);
    }
    case CONST_BOOL:
        v->as.boolean = get_u8(r) != 0;
        return !r->failed && t->kind == TYPE_BOOL;
    case CONST_CHAR:
        v->as.character = get_u32(r);
        return !r->failed && t->kind == TYPE_CHAR;
    case CONST_NULL:
        return t->kind == TYPE_POINTER || t->kind == TYPE_FN;
    case CONST_TEXT: {
        struct name bytes = get_name(r);
        v->as.text.bytes = bytes.text;
        v->as.text.length = bytes.length;
        return !r->failed && (t->kind == TYPE_STR || t->kind == TYPE_SLICE);
    }
    case CONST_SYMBOLIC:
        v->as.symbolic = read_symbolic(r, r->table_count, 0);
        return !r->failed && v->as.symbolic->type == t;
    case CONST_ARRAY:
    case CONST_STRUCT:
        n = get_count(r, 1);
        if (r->failed || (kind == CONST_ARRAY
                              ? t->kind != TYPE_ARRAY || n != t->length
                              : !type_has_fields(t) ||
                                    n != t->field_count)) {
            return false;
        }
        v->as.aggregate.count = n;
        v->as.aggregate.items = allocate(r, n, sizeof *v->as.aggregate.items);
        for (i = 0; i < n; i++) {
            struct type *item = kind == CONST_ARRAY ? t->element
                                                    : t->fields[i].type;
            if (!read_value(r, item, &v->as.aggregate.items[i], depth + 1)) {
                return false;
            }
        }
        return true;
    default:
        return false;
    }
}

static void read_items(struct reader *r)
{
    struct interface *iface = r->iface;
    uint32_t count = get_count(r, 14);
    uint32_t i;

    iface->items = allocate(r, count, sizeof *iface->items);
    for (i = 0; i < count && !r->failed; i++) {
        struct symbol *sym = arena_alloc(r->arena, sizeof *sym);
        uint8_t kind = get_u8(r);
        bool ok;
        sym->name = get_name(r);
        sym->type = type_ref(r, r->table_count);
        {
            uint8_t marks = get_u8(r);
            sym->exported = (marks & 1) != 0;
            sym->internal = (marks >> 1 & 1) != 0;
        }
        {
            struct name doc = get_name(r);
            sym->doc.text = doc.text;
            sym->doc.length = doc.length;
        }
        if (sym->exported && kind == SYMBOL_STRUCT && sym->type != NULL) {
            sym->type->item_exported = true;
        }
        sym->kind = (enum symbol_kind)kind;
        sym->state = EVAL_DONE;
        sym->home = iface;
        if (r->failed) {
            break;
        }
        switch (kind) {
        case SYMBOL_FN:
        case SYMBOL_EXTERN_FN:
            ok = sym->type->kind == TYPE_FN;
            if (ok) {
                size_t j;
                struct name *names = allocate(r, sym->type->param_count,
                                              sizeof *names);
                for (j = 0; j < sym->type->param_count; j++) {
                    names[j] = get_name(r);
                }
                sym->params = names;
            }
            if (kind == SYMBOL_EXTERN_FN) {
                sym->variadic = get_u8(r) != 0;
            }
            ok = ok && !r->failed;
            break;
        case SYMBOL_STRUCT:
            /* A struct, a class and an enum share this symbol kind. */
            ok = (type_has_fields(sym->type) ||
                  sym->type->kind == TYPE_ENUM) &&
                 name_equals(&sym->type->module, iface->module);
            break;
        case SYMBOL_CONST:
            sym->value = arena_alloc(r->arena, sizeof *sym->value);
            ok = read_value(r, sym->type, sym->value, 0);
            break;
        default:
            ok = false;
            break;
        }
        if (!ok) {
            damaged(r);
        }
        iface->items[iface->item_count++] = sym;
    }
}

/* The IR */

static bool valid_type(uint8_t type)
{
    return type <= IR_CWCHAR;
}

/* Where each function, global, aggregate and symbolic value of the file
   went in the program. */
struct ir_maps {
    uint32_t *functions;
    uint32_t function_count;
    uint32_t *globals;
    uint32_t global_count;
    struct ir_aggtype *aggs;        /* as read, with indices of the file */
    uint32_t *agg_map;
    uint8_t *agg_state;
    uint32_t agg_count;
    struct ir_sym *syms;            /* as read, with indices of the file */
    uint32_t *sym_map;
    uint8_t *sym_state;
    uint32_t sym_count;
};

enum { MAP_NONE, MAP_BUSY, MAP_DONE };

static uint32_t map_sym(struct reader *r, struct ir_module *program,
                        struct ir_maps *maps, uint32_t sym);

/* The program's index of aggregate agg of the file. The types an
   aggregate is built from are added before it. */
static uint32_t map_agg(struct reader *r, struct ir_module *program,
                        struct ir_maps *maps, uint32_t agg)
{
    struct ir_aggtype *t;
    size_t i;

    if (agg >= maps->agg_count || maps->agg_state[agg] == MAP_BUSY) {
        damaged(r);
        return 0;
    }
    if (maps->agg_state[agg] == MAP_DONE) {
        return maps->agg_map[agg];
    }
    maps->agg_state[agg] = MAP_BUSY;
    t = &maps->aggs[agg];
    for (i = 0; i < t->field_count && !r->failed; i++) {
        if (t->fields[i].type.type == IR_AGG) {
            t->fields[i].type.agg = map_agg(r, program, maps,
                                            t->fields[i].type.agg);
        }
    }
    if (r->failed) {
        return 0;
    }
    if (t->kind == IR_AGG_ARRAY) {
        uint32_t length = map_sym(r, program, maps, t->length);
        maps->agg_map[agg] = r->failed ? 0
                                       : ir_array_add(program, t->name,
                                                      t->fields[0].type,
                                                      length, t->length_text);
    } else {
        maps->agg_map[agg] = ir_struct_add(program, t->kind, t->name, t->fields,
                                           t->field_count, t->packed,
                                           t->align);
    }
    maps->agg_state[agg] = MAP_DONE;
    return maps->agg_map[agg];
}

static uint32_t map_sym(struct reader *r, struct ir_module *program,
                        struct ir_maps *maps, uint32_t sym)
{
    struct ir_sym s;

    if (sym >= maps->sym_count || maps->sym_state[sym] == MAP_BUSY) {
        damaged(r);
        return 0;
    }
    if (maps->sym_state[sym] == MAP_DONE) {
        return maps->sym_map[sym];
    }
    maps->sym_state[sym] = MAP_BUSY;
    s = maps->syms[sym];
    if (s.kind == IR_SYM_SIZE_OF || s.kind == IR_SYM_OFFSET_OF) {
        if (s.of.type == IR_AGG) {
            s.of.agg = map_agg(r, program, maps, s.of.agg);
        }
        if (!r->failed && s.kind == IR_SYM_OFFSET_OF &&
            (s.of.type != IR_AGG ||
             s.field >= program->aggs[s.of.agg]->field_count)) {
            damaged(r);
        }
    } else if (s.kind == IR_SYM_OP) {
        s.a = map_sym(r, program, maps, s.a);
        if (s.b != IR_NO_AGG) {
            s.b = map_sym(r, program, maps, s.b);
        }
    }
    if (r->failed) {
        return 0;
    }
    switch (s.kind) {
    case IR_SYM_INT:
        maps->sym_map[sym] = ir_sym_int(program, s.type, s.value);
        break;
    case IR_SYM_SIZE_OF:
        maps->sym_map[sym] = ir_sym_size_of(program, s.of);
        break;
    case IR_SYM_OFFSET_OF:
        maps->sym_map[sym] = ir_sym_offset_of(program, s.of.agg, s.field);
        break;
    case IR_SYM_OP:
        maps->sym_map[sym] = ir_sym_op(program, (enum ir_op)s.op, s.type, s.a,
                                       s.b);
        break;
    }
    maps->sym_state[sym] = MAP_DONE;
    return maps->sym_map[sym];
}

static struct ir_vtype read_vtype(struct reader *r, bool scalar_only)
{
    struct ir_vtype v;
    uint8_t type = get_u8(r);

    v.agg = get_u32(r);
    v.type = (enum ir_type)type;
    if (!r->failed && (!valid_type(type) || (scalar_only && type == IR_AGG) ||
                       ((type == IR_AGG) != (v.agg != IR_NO_AGG)))) {
        damaged(r);
    }
    return v;
}

/* The aggregate and symbolic tables of the file. They refer to each other
   by index, so both are read before either is added to the program. */
static void read_tables(struct reader *r, struct ir_module *program,
                        struct ir_maps *maps)
{
    uint32_t i;
    uint32_t j;

    maps->sym_count = get_count(r, 27);
    maps->syms = allocate(r, maps->sym_count, sizeof *maps->syms);
    maps->sym_map = allocate(r, maps->sym_count, sizeof *maps->sym_map);
    maps->sym_state = allocate(r, maps->sym_count, sizeof *maps->sym_state);
    for (i = 0; i < maps->sym_count && !r->failed; i++) {
        struct ir_sym *s = &maps->syms[i];
        uint8_t kind = get_u8(r);
        uint8_t type = get_u8(r);
        s->kind = (enum ir_sym_kind)kind;
        s->type = (enum ir_type)type;
        s->value = get_u64(r);
        s->of = read_vtype(r, false);
        s->field = get_u32(r);
        s->op = get_u8(r);
        s->a = get_u32(r);
        s->b = get_u32(r);
        if (kind > IR_SYM_OP || type < IR_I8 ||
            (type > IR_I64 && type != IR_CLONG && type != IR_CWCHAR) ||
            s->op > IR_RET ||
            ((kind == IR_SYM_SIZE_OF || kind == IR_SYM_OFFSET_OF) &&
             s->of.type == IR_VOID)) {
            damaged(r);
        }
    }
    maps->agg_count = get_count(r, 26);
    maps->aggs = allocate(r, maps->agg_count, sizeof *maps->aggs);
    maps->agg_map = allocate(r, maps->agg_count, sizeof *maps->agg_map);
    maps->agg_state = allocate(r, maps->agg_count, sizeof *maps->agg_state);
    for (i = 0; i < maps->agg_count && !r->failed; i++) {
        struct ir_aggtype *t = &maps->aggs[i];
        uint8_t kind = get_u8(r);
        t->kind = (enum ir_agg_kind)kind;
        t->name = get_cstr(r);
        t->packed = get_u8(r) != 0;
        t->align = get_u64(r);
        t->length = get_u32(r);
        t->length_text = get_cstr(r);
        t->field_count = get_count(r, 10);
        t->fields = allocate(r, t->field_count, sizeof *t->fields);
        for (j = 0; j < t->field_count && !r->failed; j++) {
            uint8_t ext;
            t->fields[j].name = get_cstr(r);
            t->fields[j].type = read_vtype(r, false);
            t->fields[j].bits = get_u8(r);
            ext = get_u8(r);
            t->fields[j].ext = (enum ir_ext)ext;
            if (ext > IR_EXT_ZERO || t->fields[j].type.type == IR_VOID) {
                damaged(r);
            }
        }
        if (kind > IR_AGG_ARRAY || t->name[0] == '\0' ||
            t->field_count == 0 ||
            (kind == IR_AGG_ARRAY && t->field_count != 1)) {
            damaged(r);
        }
    }
    for (i = 0; i < maps->agg_count && !r->failed; i++) {
        map_agg(r, program, maps, i);
    }
    for (i = 0; i < maps->sym_count && !r->failed; i++) {
        map_sym(r, program, maps, i);
    }
}

static uint32_t read_agg_ref(struct reader *r, struct ir_module *program,
                             struct ir_maps *maps, uint8_t type)
{
    uint32_t agg = get_u32(r);

    if (r->failed || (type == IR_AGG) != (agg != IR_NO_AGG)) {
        damaged(r);
        return IR_NO_AGG;
    }
    return type == IR_AGG ? map_agg(r, program, maps, agg) : IR_NO_AGG;
}

/* Read a constant tree, mapping its aggregate indices. A global index
   stays as written, because a constant may name a global that comes
   later. remap_const moves them. */
static struct ir_const *read_const(struct reader *r, struct ir_module *program,
                                   struct ir_maps *maps, int depth)
{
    struct ir_const *c;
    uint8_t kind;
    uint8_t scalar;
    uint64_t payload;
    uint64_t i;

    if (depth > 32) {
        damaged(r);
        return NULL;
    }
    kind = get_u8(r);
    scalar = get_u8(r);
    payload = get_u64(r);
    if (r->failed || kind > IR_CONST_AGG || !valid_type(scalar)) {
        damaged(r);
        return NULL;
    }
    c = arena_alloc(r->arena, sizeof *c);
    c->kind = (enum ir_const_kind)kind;
    c->scalar = (enum ir_type)scalar;
    switch (c->kind) {
    case IR_CONST_INT:
        c->integer = payload;
        break;
    case IR_CONST_FLOAT:
        memcpy(&c->floating, &payload, sizeof payload);
        break;
    case IR_CONST_SYM:
        if (payload >= maps->sym_count) {
            damaged(r);
            return NULL;
        }
        c->sym = map_sym(r, program, maps, (uint32_t)payload);
        break;
    case IR_CONST_ADDR:
    case IR_CONST_FUNC:
        c->global = (uint32_t)payload;
        break;
    case IR_CONST_AGG:
        c->type = read_vtype(r, false);
        if (r->failed || c->type.type != IR_AGG) {
            damaged(r);
            return NULL;
        }
        c->type.agg = map_agg(r, program, maps, c->type.agg);
        /* An item costs at least its kind, its type and its payload, so
           a count past that many bytes cannot be honest. */
        if (payload > (uint64_t)(r->size - r->pos) / 10) {
            damaged(r);
            return NULL;
        }
        c->item_count = (size_t)payload;
        c->items = arena_alloc(r->arena,
                               (payload == 0 ? 1 : payload) * sizeof *c->items);
        for (i = 0; i < payload && !r->failed; i++) {
            struct ir_const *item = read_const(r, program, maps, depth + 1);
            if (item == NULL) {
                return NULL;
            }
            c->items[i] = *item;
        }
        break;
    default: /* IR_CONST_NONE */
        break;
    }
    return r->failed ? NULL : c;
}

/* DESIGN: move the addresses inside a constant to the indices of the
   program. A table entry names a function, and the file lists the
   functions after the globals. The two kinds of address are therefore
   moved in two passes, and `functions` says which pass this is. */
static void remap_const(struct reader *r, struct ir_const *c,
                        const struct ir_maps *maps, bool functions)
{
    size_t i;

    if (c->kind == IR_CONST_ADDR && !functions) {
        if (c->global >= maps->global_count) {
            damaged(r);
            return;
        }
        c->global = maps->globals[c->global];
    } else if (c->kind == IR_CONST_FUNC && functions) {
        if (c->global >= maps->function_count) {
            damaged(r);
            return;
        }
        c->global = maps->functions[c->global];
    } else if (c->kind == IR_CONST_AGG) {
        for (i = 0; i < c->item_count; i++) {
            remap_const(r, &c->items[i], maps, functions);
        }
    }
}

static struct ir_operand read_operand(struct reader *r,
                                      const struct ir_function *f,
                                      const struct ir_maps *maps)
{
    struct ir_operand o = {IR_NONE, IR_VOID, {0}};
    uint8_t kind = get_u8(r);
    uint8_t type = get_u8(r);
    uint64_t payload = get_u64(r);

    if (r->failed) {
        return o;
    }
    if (kind > IR_SYM || !valid_type(type)) {
        damaged(r);
        return o;
    }
    o.kind = (enum ir_operand_kind)kind;
    o.type = (enum ir_type)type;
    switch (o.kind) {
    case IR_TEMP:
        if (payload >= f->temp_count || f->temps[payload] != o.type) {
            damaged(r);
        } else {
            o.as.temp = (uint32_t)payload;
        }
        break;
    case IR_INT:
        o.as.integer = payload;
        break;
    case IR_FLOAT:
        memcpy(&o.as.floating, &payload, sizeof payload);
        break;
    case IR_GLOBAL:
        if (payload >= maps->global_count) {
            damaged(r);
        } else {
            o.as.index = maps->globals[payload];
        }
        break;
    case IR_FUNC:
        if (payload >= maps->function_count) {
            damaged(r);
        } else {
            o.as.index = maps->functions[payload];
        }
        break;
    case IR_BLOCK:
        if (payload >= f->block_count) {
            damaged(r);
        } else {
            o.as.index = (uint32_t)payload;
        }
        break;
    case IR_SYM:
        if (payload >= maps->sym_count ||
            maps->syms[payload].type != o.type) {
            damaged(r);
        } else {
            o.as.index = maps->sym_map[payload];
        }
        break;
    case IR_NONE:
        break;
    }
    return o;
}

static void read_body(struct reader *r, struct ir_module *program,
                      struct ir_function *f, struct ir_maps *maps)
{
    uint32_t temps = get_count(r, 1);
    uint32_t blocks;
    uint32_t i;
    uint32_t j;
    uint32_t k;

    for (i = 0; i < temps && !r->failed; i++) {
        uint8_t type = get_u8(r);
        if (i < f->param_count) {
            if (type != f->temps[i]) {
                damaged(r);
            }
        } else if (!valid_type(type) || type == IR_VOID || type == IR_AGG) {
            damaged(r);
        } else {
            ir_temp(f, (enum ir_type)type);
        }
    }
    if (temps < f->param_count) {
        damaged(r);
    }
    blocks = get_count(r, 4);
    for (i = 0; i < blocks && !r->failed; i++) {
        ir_block_add(f);
    }
    for (i = 0; i < blocks && !r->failed; i++) {
        uint32_t count;
        f->blocks[i]->assert_fail = get_u8(r) != 0;
        count = get_count(r, 46);
        for (j = 0; j < count && !r->failed; j++) {
            struct ir_inst inst;
            struct ir_operand *args;
            uint8_t op = get_u8(r);
            uint8_t type = get_u8(r);
            memset(&inst, 0, sizeof inst);
            inst.result = get_u32(r);
            inst.op = (enum ir_op)op;
            inst.type = (enum ir_type)type;
            if (op > IR_RET || !valid_type(type) ||
                (inst.result != IR_NO_RESULT && inst.result >= f->temp_count)) {
                damaged(r);
                break;
            }
            inst.a = read_operand(r, f, maps);
            inst.b = read_operand(r, f, maps);
            inst.c = read_operand(r, f, maps);
            inst.of = read_vtype(r, false);
            inst.field = get_u32(r);
            if (!r->failed && inst.of.type == IR_AGG) {
                inst.of.agg = map_agg(r, program, maps, inst.of.agg);
            }
            if (!r->failed &&
                ((op == IR_SLOT || op == IR_MEMCOPY || op == IR_BITLOAD ||
                  op == IR_BITSTORE) == (inst.of.type == IR_VOID) ||
                 ((op == IR_BITLOAD || op == IR_BITSTORE) &&
                  (inst.of.type != IR_AGG ||
                   inst.field >= program->aggs[inst.of.agg]->field_count ||
                   program->aggs[inst.of.agg]->fields[inst.field].bits == 0)))) {
                damaged(r);
            }
            inst.arg_count = get_count(r, 10);
            args = allocate(r, inst.arg_count, sizeof *args);
            for (k = 0; k < inst.arg_count && !r->failed; k++) {
                args[k] = read_operand(r, f, maps);
            }
            inst.args = args;
            if (!r->failed) {
                ir_inst_add(f->blocks[i], &inst);
            }
        }
    }
}

/* A function signature, mapped to a function of the program. A C function
   and a declaration share an existing entry of the same name. */
static uint32_t read_signature(struct reader *r, struct ir_module *program,
                               struct ir_maps *maps, bool *has_body)
{
    uint8_t flags = get_u8(r);
    const char *module = get_cstr(r);
    const char *name = get_cstr(r);
    uint8_t result = get_u8(r);
    uint32_t result_agg = read_agg_ref(r, program, maps, result);
    uint32_t param_count = get_count(r, 6);
    struct ir_function *f = NULL;
    size_t i;

    *has_body = false;
    if (r->failed) {
        return 0;
    }
    if (!valid_type(result) || flags > 7 ||
        ((flags & 1) == 0 && module[0] == '\0') ||
        (module[0] != '\0' && (flags & 2) != 0) ||
        (module[0] == '\0' && (flags & 4) != 0)) {
        damaged(r);
        return 0;
    }
    if (module[0] == '\0') {
        module = NULL;
    }
    for (i = 0; i < program->function_count; i++) {
        struct ir_function *g = program->functions[i];
        bool same_module = module == NULL
                               ? g->module == NULL
                               : g->module != NULL &&
                                     strcmp(g->module, module) == 0;
        if (same_module && strcmp(g->name, name) == 0) {
            f = g;
        }
    }
    if (f != NULL && (flags & 1) == 0) {
        fail(r, "defines `%s.%s`, which another library defines", module, name);
        return 0;
    }
    if (f == NULL) {
        if ((flags & 1) == 0) {
            f = ir_function_add(program, module, name, (enum ir_type)result,
                                result_agg);
            *has_body = true;
        } else if (module == NULL) {
            f = ir_extern_add(program, name, (enum ir_type)result, flags & 2);
            f->result_agg = result_agg;
        } else {
            f = ir_declare_add(program, module, name, (enum ir_type)result,
                               result_agg);
        }
        f->exported = (flags & 4) != 0;
        for (i = 0; i < param_count && !r->failed; i++) {
            uint8_t type = get_u8(r);
            uint8_t ext = get_u8(r);
            uint32_t agg = read_agg_ref(r, program, maps, type);
            bool narrow = type == IR_I8 || type == IR_I16;
            if (!valid_type(type) || type == IR_VOID || ext > IR_EXT_ZERO ||
                (ext != IR_EXT_NONE) != narrow) {
                damaged(r);
            } else {
                ir_param_add(f, (enum ir_type)type, agg);
                f->params[f->param_count - 1].ext = (enum ir_ext)ext;
            }
        }
    } else {
        for (i = 0; i < param_count && !r->failed; i++) {
            uint8_t type = get_u8(r);
            get_u8(r);
            read_agg_ref(r, program, maps, type);
        }
    }
    return f->index;
}

struct relocs {
    uint32_t count;
    uint64_t *offsets;
    uint32_t *targets;
    uint8_t *functions;             /* the target is a function */
};

static void read_ir(struct reader *r, struct ir_module *program)
{
    struct ir_maps maps;
    struct relocs *relocs;
    struct ir_function **bodies;
    uint32_t i;
    uint32_t j;

    memset(&maps, 0, sizeof maps);
    read_tables(r, program, &maps);
    maps.global_count = get_count(r, 28);
    maps.globals = allocate(r, maps.global_count, sizeof *maps.globals);
    relocs = allocate(r, maps.global_count, sizeof *relocs);
    for (i = 0; i < maps.global_count && !r->failed; i++) {
        const char *module = get_cstr(r);
        const char *name = get_cstr(r);
        uint64_t size = get_u64(r);
        uint64_t align = get_u64(r);
        struct ir_global *g;
        /* A global of the runtime has no module, and an empty name is
           how the file spells that. */
        if (module != NULL && module[0] == '\0') {
            module = NULL;
        }
        if (!take(r, size)) {
            break;
        }
        g = ir_global_add(program, module, name, r->data + r->pos, size, align);
        r->pos += size;
        maps.globals[i] = g->index;
        relocs[i].count = get_count(r, 13);
        relocs[i].offsets = allocate(r, relocs[i].count, sizeof(uint64_t));
        relocs[i].targets = allocate(r, relocs[i].count, sizeof(uint32_t));
        relocs[i].functions = allocate(r, relocs[i].count, sizeof(uint8_t));
        for (j = 0; j < relocs[i].count && !r->failed; j++) {
            relocs[i].offsets[j] = get_u64(r);
            relocs[i].targets[j] = get_u32(r);
            relocs[i].functions[j] = get_u8(r) != 0 ? 1 : 0;
            if (relocs[i].offsets[j] > size ||
                size - relocs[i].offsets[j] < 8) {
                damaged(r);
            }
        }
        {
            uint8_t marks = get_u8(r);
            g->exported = (marks & 2) != 0;
            g->is_extern = (marks & 4) != 0;
            g->mutable = (marks & 8) != 0;
            if ((marks & 1) != 0 && !r->failed) {
                g->value = read_const(r, program, &maps, 0);
            }
        }
    }
    /* A pointer may name a global that the file lists later. */
    for (i = 0; i < maps.global_count && !r->failed; i++) {
        struct ir_global *g = program->globals[maps.globals[i]];
        if (g->value != NULL) {
            remap_const(r, g->value, &maps, false);
        }
        for (j = 0; j < relocs[i].count; j++) {
            if (relocs[i].functions[j]) {
                continue;
            }
            if (relocs[i].targets[j] >= maps.global_count) {
                damaged(r);
                break;
            }
            ir_global_reloc(program, g, relocs[i].offsets[j],
                            maps.globals[relocs[i].targets[j]]);
        }
    }
    maps.function_count = get_count(r, 15);
    maps.functions = allocate(r, maps.function_count, sizeof *maps.functions);
    bodies = allocate(r, maps.function_count, sizeof *bodies);
    for (i = 0; i < maps.function_count && !r->failed; i++) {
        bool has_body;
        maps.functions[i] = read_signature(r, program, &maps, &has_body);
        bodies[i] = has_body ? program->functions[maps.functions[i]] : NULL;
    }
    for (i = 0; i < maps.function_count && !r->failed; i++) {
        if (bodies[i] != NULL) {
            read_body(r, program, bodies[i], &maps);
        }
    }
    /* A table entry names a function, and the file lists the functions
       after the globals, so those addresses wait until here. */
    for (i = 0; i < maps.global_count && !r->failed; i++) {
        struct ir_global *g = program->globals[maps.globals[i]];
        if (g->value != NULL) {
            remap_const(r, g->value, &maps, true);
        }
        for (j = 0; j < relocs[i].count; j++) {
            if (!relocs[i].functions[j]) {
                continue;
            }
            if (relocs[i].targets[j] >= maps.function_count) {
                damaged(r);
                break;
            }
            ir_global_reloc_fn(program, g, relocs[i].offsets[j],
                               maps.functions[relocs[i].targets[j]]);
        }
    }
}

struct interface *antl_read(const uint8_t *data, size_t size,
                            const struct interface *const *libraries,
                            size_t library_count, struct types *types,
                            struct arena *arena, struct ir_module *program,
                            char *error, size_t error_size)
{
    struct reader r;
    size_t i;

    memset(&r, 0, sizeof r);
    r.data = data;
    r.size = size;
    r.error = error;
    r.error_size = error_size;
    r.arena = arena;
    r.types = types;
    r.libraries = libraries;
    r.library_count = library_count;
    r.iface = arena_alloc(arena, sizeof *r.iface);
    read_header(&r, r.iface);
    for (i = 0; i < r.iface->import_count && !r.failed; i++) {
        struct name imported;
        imported.text = r.iface->imports[i];
        imported.length = strlen(imported.text);
        if (library(&r, &imported) == NULL) {
            fail(&r, "needs module `%s`", imported.text);
        }
    }
    if (!r.failed) {
        read_types(&r);
    }
    if (!r.failed) {
        read_items(&r);
    }
    if (!r.failed) {
        read_ir(&r, program);
    }
    if (!r.failed && r.pos != r.size) {
        damaged(&r);
    }
    return r.failed ? NULL : r.iface;
}
