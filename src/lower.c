#include "lower.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sema.h"
#include "text.h"
#include "types.h"

/* Lowering walks the checked tree of one function. Expressions become
   instructions in the current block. if, while and do while become new
   blocks joined by jumps and branches. */

struct loop {
    struct ir_block *continue_to;
    struct ir_block *break_to;
    struct loop *outer;
};

struct lowerer {
    struct ir_module *m;
    struct diagnostics *diags;
    const char *module_name;
    struct ir_function *f;
    struct ir_block *b;         /* NULL after a terminator */
    struct loop *loop;
    bool failed;
};

static struct ir_operand none(void)
{
    struct ir_operand o = {IR_NONE, IR_VOID, {0}};
    return o;
}

static struct ir_operand temp(const struct lowerer *l, uint32_t t)
{
    return ir_temp_op(l->f, t);
}

/* bool is i8 and char is i32. Signedness moves into the operations. An
   enum is its underlying integer type. */
static enum ir_type ir_type_of(const struct type *t)
{
    if (t->kind == TYPE_ENUM) {
        t = t->base;
    }
    switch (t->kind) {
    case TYPE_BOOL:
    case TYPE_I8:
    case TYPE_U8:
        return IR_I8;
    case TYPE_I16:
    case TYPE_U16:
        return IR_I16;
    case TYPE_CHAR:
    case TYPE_I32:
    case TYPE_U32:
        return IR_I32;
    case TYPE_I64:
    case TYPE_U64:
        return IR_I64;
    case TYPE_CLONG:
    case TYPE_CULONG:
        return IR_CLONG;
    case TYPE_CWCHAR:
        return IR_CWCHAR;
    case TYPE_F32:
        return IR_F32;
    case TYPE_F64:
        return IR_F64;
    case TYPE_POINTER:
    case TYPE_FN:
        return IR_PTR;
    case TYPE_STRUCT:
    case TYPE_ARRAY:
    case TYPE_STR:
    case TYPE_SLICE:
        return IR_AGG;
    default:
        return IR_VOID;
    }
}

/* A str and a slice are aggregates of a pointer and a length. */
static bool is_aggregate(const struct type *t)
{
    return t->kind == TYPE_STRUCT || t->kind == TYPE_ARRAY ||
           t->kind == TYPE_STR || t->kind == TYPE_SLICE;
}

static struct ir_vtype vtype_of(struct lowerer *l, const struct type *t);

static char *name_of_type(const struct type *t, bool qualified)
{
    struct text name = {0};
    char *copy;

    if (qualified) {
        type_name_qualified(&name, t);
    } else {
        type_name(&name, t);
    }
    copy = malloc(name.length + 1);
    if (copy == NULL) {
        fputs("antic: out of memory\n", stderr);
        exit(70);
    }
    memcpy(copy, text_cstr(&name), name.length + 1);
    text_free(&name);
    return copy;
}

static uint32_t sym_of(struct lowerer *l, const struct symbolic *s);

/* The aggregate of t in the type table of the module. A struct lists its
   fields, a str or slice a pointer and a length, and an array its element
   and its length. */
static uint32_t agg_of(struct lowerer *l, const struct type *t)
{
    char *name = name_of_type(t, true);
    uint32_t agg = ir_agg_find(l->m, name);
    struct ir_field *fields;
    size_t count = t->kind == TYPE_STRUCT ? t->field_count : 2;
    size_t i;

    if (agg != IR_NO_AGG) {
        free(name);
        return agg;
    }
    fields = calloc(count + 1, sizeof *fields);
    if (fields == NULL) {
        fputs("antic: out of memory\n", stderr);
        exit(70);
    }
    if (t->kind == TYPE_ARRAY) {
        struct text text = {0};
        struct ir_vtype element = vtype_of(l, t->element);
        uint32_t length = t->length_of != NULL
                              ? sym_of(l, t->length_of)
                              : ir_sym_int(l->m, IR_I64, t->length);
        if (t->length_of != NULL) {
            symbolic_print(&text, t->length_of, false);
        } else {
            text_appendf(&text, "%llu", (unsigned long long)t->length);
        }
        agg = ir_array_add(l->m, name, element, length, text_cstr(&text));
        text_free(&text);
    } else if (t->kind == TYPE_STRUCT) {
        for (i = 0; i < count; i++) {
            char *field = malloc(t->fields[i].name.length + 1);
            if (field == NULL) {
                fputs("antic: out of memory\n", stderr);
                exit(70);
            }
            memcpy(field, t->fields[i].name.text, t->fields[i].name.length);
            field[t->fields[i].name.length] = '\0';
            fields[i].name = field;
            fields[i].type = vtype_of(l, t->fields[i].type);
            fields[i].bits = t->fields[i].bits;
            fields[i].ext = t->fields[i].bits == 0 ? IR_EXT_NONE
                            : type_is_signed(t->fields[i].type) ? IR_EXT_SIGN
                                                                : IR_EXT_ZERO;
        }
        agg = ir_struct_add(l->m, t->is_union ? IR_AGG_UNION : IR_AGG_STRUCT,
                            name, fields, count, t->packed, t->align);
        for (i = 0; i < count; i++) {
            free((char *)fields[i].name);
        }
    } else {
        fields[0].name = "ptr";
        fields[0].type = ir_scalar(IR_PTR);
        fields[1].name = "len";
        fields[1].type = ir_scalar(IR_I64);
        agg = ir_struct_add(l->m, IR_AGG_STRUCT, name, fields, 2, false, 0);
    }
    free(fields);
    free(name);
    return agg;
}

static struct ir_vtype vtype_of(struct lowerer *l, const struct type *t)
{
    return is_aggregate(t) ? ir_aggregate(agg_of(l, t))
                           : ir_scalar(ir_type_of(t));
}

/* The size of a value of type t as an operand, symbolic until the back
   end folds it. */
static struct ir_operand size_operand(struct lowerer *l, const struct type *t)
{
    return ir_sym_operand(l->m, ir_sym_size_of(l->m, vtype_of(l, t)));
}

/* The narrowest and the widest width of an integer type on the six
   targets. Only a target-sized type has two. */
static int min_bits(enum ir_type type)
{
    switch (type) {
    case IR_I8: return 8;
    case IR_I16:
    case IR_CWCHAR: return 16;
    case IR_I32:
    case IR_CLONG: return 32;
    default: return 64;
    }
}

static int max_bits(enum ir_type type)
{
    return type == IR_CLONG ? 64 : type == IR_CWCHAR ? 32 : min_bits(type);
}

/* DESIGN: a conversion between integer types truncates when the target
   type is never wider than the source, and extends otherwise. With
   c_long and c_wchar both cases are one width on some target, and the
   back end turns such a conversion into a copy. */
static bool narrows(enum ir_type from, enum ir_type to)
{
    return max_bits(to) <= min_bits(from);
}

static struct ir_block *new_block(struct lowerer *l)
{
    return ir_block_add(l->f);
}

/* Names in the tree point into the source and carry a length. */
static char *cstr(const struct name *name)
{
    char *s = malloc(name->length + 1);

    if (s == NULL) {
        fputs("antic: out of memory\n", stderr);
        exit(70);
    }
    memcpy(s, name->text, name->length);
    s[name->length] = '\0';
    return s;
}

/* A function of the IR module by its module and name. A NULL module
   names a C function. */
static struct ir_function *find_function(const struct ir_module *m,
                                         const char *module, const char *name)
{
    size_t i;

    for (i = 0; i < m->function_count; i++) {
        struct ir_function *f = m->functions[i];
        if (strcmp(f->name, name) == 0 &&
            (module == NULL ? f->module == NULL
                            : f->module != NULL &&
                                  strcmp(f->module, module) == 0)) {
            return f;
        }
    }
    return NULL;
}

static uint32_t result_agg(struct lowerer *l, const struct type *t)
{
    return is_aggregate(t) ? agg_of(l, t) : IR_NO_AGG;
}

static enum ir_ext param_ext(const struct type *t)
{
    enum ir_type type = ir_type_of(t);

    if (type != IR_I8 && type != IR_I16) {
        return IR_EXT_NONE;
    }
    return type_is_signed(t) ? IR_EXT_SIGN : IR_EXT_ZERO;
}

/* A parameter of 8 or 16 bits records whether it is signed, and an
   aggregate its layout. */
static void add_param(struct lowerer *l, struct ir_function *f,
                      const struct type *t)
{
    enum ir_type type = ir_type_of(t);

    ir_param_add(f, type, result_agg(l, t));
    f->params[f->param_count - 1].ext = param_ext(t);
}

/* The C library functions behind alloc and free. A module that declares
   one of them itself shares the declaration. */
static struct ir_function *c_function(struct lowerer *l, const char *name,
                                      enum ir_type result, enum ir_type param)
{
    struct ir_function *f = find_function(l->m, NULL, name);

    if (f == NULL) {
        f = ir_extern_add(l->m, name, result, false);
        ir_param_add(f, param, IR_NO_AGG);
    }
    return f;
}

/* The IR function of a function symbol. An imported function is found by
   its module and name, or declared at its first call. */
static struct ir_function *callee_function(struct lowerer *l,
                                           const struct symbol *sym)
{
    const struct type *t = sym->type;
    const char *module;
    char *name;
    struct ir_function *f;
    size_t i;

    if (sym->home == NULL) {
        return l->m->functions[sym->ir];
    }
    module = sym->kind == SYMBOL_EXTERN_FN ? NULL : sym->home->module;
    name = cstr(&sym->name);
    f = find_function(l->m, module, name);
    if (f == NULL) {
        if (module == NULL) {
            f = ir_extern_add(l->m, name, ir_type_of(t->result),
                              sym->variadic);
            f->result_agg = result_agg(l, t->result);
        } else {
            f = ir_declare_add(l->m, module, name, ir_type_of(t->result),
                               result_agg(l, t->result));
            f->exported = sym->exported;
        }
        for (i = 0; i < t->param_count; i++) {
            add_param(l, f, t->params[i]);
        }
    }
    free(name);
    return f;
}

/* Whether declared function g has the parameters and result of t. */
static bool has_signature(struct lowerer *l, const struct ir_function *g,
                          const struct type *t)
{
    size_t i;

    if (g->result != ir_type_of(t->result) ||
        g->result_agg != result_agg(l, t->result) ||
        g->param_count != t->param_count) {
        return false;
    }
    for (i = 0; i < t->param_count; i++) {
        if (g->params[i].type != ir_type_of(t->params[i]) ||
            g->params[i].ext != param_ext(t->params[i]) ||
            g->params[i].agg != result_agg(l, t->params[i])) {
            return false;
        }
    }
    return true;
}

/* DESIGN: a call through a function pointer takes its parameters and
   result from a signature: a function fn.N that the module declares and
   never defines. No identifier contains a dot, so no Anti function has
   that name. Equal signatures share one declaration. */
static const struct ir_function *signature(struct lowerer *l,
                                           const struct type *t)
{
    struct ir_function *f;
    uint32_t count = 0;
    char name[24];
    size_t i;

    for (i = 0; i < l->m->function_count; i++) {
        const struct ir_function *g = l->m->functions[i];
        if (!g->is_extern || g->module == NULL ||
            strcmp(g->module, l->module_name) != 0 ||
            strncmp(g->name, "fn.", 3) != 0) {
            continue;
        }
        if (has_signature(l, g, t)) {
            return g;
        }
        count++;
    }
    snprintf(name, sizeof name, "fn.%u", count);
    f = ir_declare_add(l->m, l->module_name, name, ir_type_of(t->result),
                       result_agg(l, t->result));
    for (i = 0; i < t->param_count; i++) {
        add_param(l, f, t->params[i]);
    }
    return f;
}

/* Expressions */

static struct ir_operand lower_expr(struct lowerer *l, const struct expr *e);

static double float_literal(const struct expr *literal, enum ir_type type)
{
    char digits[128];

    snprintf(digits, sizeof digits, "%.*s", (int)literal->as.text.length,
             literal->as.text.bytes);
    return type == IR_F32 ? (double)strtof(digits, NULL)
                          : strtod(digits, NULL);
}

static struct ir_operand constant(struct lowerer *l,
                                  const struct const_value *v,
                                  enum ir_type type)
{
    switch (v->kind) {
    case CONST_SYMBOLIC:
        return ir_sym_operand(l->m, sym_of(l, v->as.symbolic));
    case CONST_FLOAT:
        return ir_float_op(type, v->as.floating);
    case CONST_BOOL:
        return ir_int_op(type, v->as.boolean);
    case CONST_CHAR:
        return ir_int_op(type, v->as.character);
    case CONST_NULL:
        return ir_int_op(type, 0);
    default:
        return ir_int_op(type, v->as.integer);
    }
}

/* A place is where an assignment writes: a variable that lives in a
   temporary, or an address in memory. */
struct place {
    bool in_temp;
    uint32_t temp;
    struct ir_operand address;      /* of a bitfield: of its aggregate */
    enum ir_type type;
    bool bitfield;
    uint32_t agg;                   /* bitfield */
    uint32_t field;                 /* bitfield */
};

static struct ir_operand lower_address(struct lowerer *l,
                                       const struct expr *e);

/* An address offset bytes after address. An offset of 0 is the address
   itself. */
static struct ir_operand offset_address(struct lowerer *l,
                                        struct ir_operand address,
                                        struct ir_operand offset)
{
    if (offset.kind == IR_INT && offset.as.integer == 0) {
        return address;
    }
    return temp(l, ir_ptradd(l->f, l->b, address, offset));
}

static struct ir_operand zero(void)
{
    return ir_int_op(IR_I64, 0);
}

static const struct struct_field *field_of(const struct type *s,
                                           const struct name *name)
{
    size_t i;

    for (i = 0; i < s->field_count; i++) {
        if (s->fields[i].name.length == name->length &&
            memcmp(s->fields[i].name.text, name->text, name->length) == 0) {
            return &s->fields[i];
        }
    }
    return NULL;
}

static bool name_is(const struct name *name, const char *text)
{
    return name->length == strlen(text) &&
           memcmp(name->text, text, name->length) == 0;
}

static const struct name len_name = {"len", 3};

/* The offset of a field as an operand. C places the first field and every
   field of a union at offset 0 on every target. Only a later field of a
   struct needs a symbolic offset. The ptr of a str or slice is its first
   field and len its second. */
static struct ir_operand field_offset(struct lowerer *l, const struct type *s,
                                      const struct name *name)
{
    uint32_t index = s->kind == TYPE_STRUCT
                         ? (uint32_t)(field_of(s, name) - s->fields)
                         : name_is(name, "len") ? 1 : 0;

    if (index == 0 || s->is_union) {
        return zero();
    }
    return ir_sym_operand(l->m, ir_sym_offset_of(l->m, agg_of(l, s), index));
}

/* The offset of element index of an array of element type t: index times
   the size of t. */
static struct ir_operand element_offset(struct lowerer *l,
                                        const struct type *t, uint64_t index)
{
    if (index == 0) {
        return zero();
    }
    return temp(l, ir_binary(l->f, l->b, IR_MUL, IR_I64,
                             ir_int_op(IR_I64, index), size_operand(l, t)));
}

/* The number that names the next global of the module. */
static uint32_t globals_of_module(struct lowerer *l)
{
    uint32_t count = 0;
    size_t i;

    for (i = 0; i < l->m->global_count; i++) {
        if (strcmp(l->m->globals[i]->module, l->module_name) == 0) {
            count++;
        }
    }
    return count;
}

/* DESIGN: the bytes of a literal and a NUL go into a global of the module,
   named by its index there. No identifier starts with a digit, so no
   function has that name. Literals with equal bytes share the global. */
static const struct ir_global *literal_global(struct lowerer *l,
                                              const struct token_text *text)
{
    struct ir_module *m = l->m;
    uint8_t *bytes;
    char name[16];
    size_t i;

    for (i = 0; i < m->global_count; i++) {
        const struct ir_global *g = m->globals[i];
        if (strcmp(g->module, l->module_name) != 0 || g->bytes == NULL) {
            continue;
        }
        if (g->size == text->length + 1 && g->bytes[text->length] == 0 &&
            memcmp(g->bytes, text->bytes, text->length) == 0) {
            return g;
        }
    }
    bytes = arena_alloc(m->arena, text->length + 1);
    memcpy(bytes, text->bytes, text->length);
    bytes[text->length] = 0;
    snprintf(name, sizeof name, "%u", globals_of_module(l));
    return ir_global_add(m, l->module_name, name, bytes, text->length + 1, 1);
}

static struct ir_operand literal_address(struct lowerer *l,
                                         const struct token_text *text)
{
    return temp(l, ir_addr(l->f, l->b, ir_global_op(literal_global(l, text))));
}

/* v.f is f's offset after the address of v. A pointer base p.f uses the
   pointer. */
static struct ir_operand field_address(struct lowerer *l,
                                       const struct expr *e)
{
    const struct expr *base = e->as.field.base;
    bool pointer = base->type->kind == TYPE_POINTER;
    const struct type *s = pointer ? base->type->element : base->type;
    struct ir_operand address;

    address = pointer ? lower_expr(l, base) : lower_address(l, base);
    if (l->failed) {
        return none();
    }
    return offset_address(l, address, field_offset(l, s, &e->as.field.name));
}

/* The address of element 0: an array starts at its own address, a str or
   a slice at its pointer, and a pointer is the address. */
static struct ir_operand first_element(struct lowerer *l, const struct expr *e)
{
    struct ir_operand address;

    switch (e->type->kind) {
    case TYPE_ARRAY:
        return lower_address(l, e);
    case TYPE_STR:
    case TYPE_SLICE:
        address = lower_address(l, e);
        return l->failed ? none()
                         : temp(l, ir_load(l->f, l->b, IR_PTR, address));
    default:
        return lower_expr(l, e);
    }
}

static struct ir_operand element_address(struct lowerer *l,
                                         const struct expr *e)
{
    struct ir_operand base = first_element(l, e->as.index.base);
    struct ir_operand index = lower_expr(l, e->as.index.index);
    uint32_t offset;

    if (l->failed) {
        return none();
    }
    offset = ir_binary(l->f, l->b, IR_MUL, IR_I64, index,
                       size_operand(l, e->type));
    return temp(l, ir_ptradd(l->f, l->b, base, temp(l, offset)));
}

/* The struct field that e reads when it is a bitfield, or NULL. */
static const struct struct_field *bitfield_of(const struct expr *e)
{
    const struct type *s;
    const struct struct_field *f;

    if (e->kind != EXPR_FIELD || e->symbol != NULL) {
        return NULL;
    }
    s = e->as.field.base->type;
    s = s->kind == TYPE_POINTER ? s->element : s;
    if (s->kind != TYPE_STRUCT) {
        return NULL;
    }
    f = field_of(s, &e->as.field.name);
    return f != NULL && f->bits != 0 ? f : NULL;
}

static bool lower_place(struct lowerer *l, const struct expr *e,
                        struct place *p)
{
    const struct symbol *sym = e->symbol;

    p->bitfield = false;
    p->in_temp = false;
    p->type = ir_type_of(e->type);
    switch (e->kind) {
    case EXPR_NAME:
        p->in_temp = !sym->address_taken && !is_aggregate(e->type);
        p->temp = sym->ir;
        p->address = p->in_temp ? none() : temp(l, sym->ir);
        return true;
    case EXPR_UNARY:
        p->address = lower_expr(l, e->as.unary.operand);
        return !l->failed;
    case EXPR_INDEX:
        p->address = element_address(l, e);
        return !l->failed;
    default: /* EXPR_FIELD */
        if (bitfield_of(e) != NULL) {
            const struct expr *base = e->as.field.base;
            const struct type *s = base->type->kind == TYPE_POINTER
                                       ? base->type->element
                                       : base->type;
            p->bitfield = true;
            p->agg = agg_of(l, s);
            p->field = (uint32_t)(field_of(s, &e->as.field.name) - s->fields);
            p->address = base->type->kind == TYPE_POINTER
                             ? lower_expr(l, base)
                             : lower_address(l, base);
            return !l->failed;
        }
        p->address = field_address(l, e);
        return !l->failed;
    }
}

/* The scalar constant v as a leaf of a constant tree. */
static void const_scalar(struct lowerer *l, const struct const_value *v,
                         enum ir_type type, struct ir_const *out)
{
    struct ir_operand o = constant(l, v, type);

    out->scalar = type;
    if (o.kind == IR_FLOAT) {
        out->kind = IR_CONST_FLOAT;
        out->floating = o.as.floating;
    } else if (o.kind == IR_SYM) {
        out->kind = IR_CONST_SYM;
        out->sym = o.as.index;
    } else {
        out->kind = IR_CONST_INT;
        out->integer = o.as.integer;
    }
}

/* The constant v of type t as the tree the back end lays out. A field of
   a struct keeps its place in the tree, so an item and a field share an
   index. The back end reads the offset of one from the other. */
static void const_tree(struct lowerer *l, const struct const_value *v,
                       const struct type *t, struct ir_const *out)
{
    struct ir_const *agg;
    uint64_t i;

    if (v->kind == CONST_TEXT) {
        agg = ir_const_agg(l->m, vtype_of(l, t), 2);
        agg->items[0].kind = IR_CONST_ADDR;
        agg->items[0].scalar = IR_PTR;
        agg->items[0].global = literal_global(l, &v->as.text)->index;
        agg->items[1].kind = IR_CONST_INT;
        agg->items[1].scalar = IR_I64;
        agg->items[1].integer = v->as.text.length;
        *out = *agg;
    } else if (t->kind == TYPE_STRUCT) {
        agg = ir_const_agg(l->m, vtype_of(l, t), t->field_count);
        for (i = 0; i < t->field_count; i++) {
            if (type_field_is_unit_break(&t->fields[i])) {
                continue;
            }
            const_tree(l, &v->as.aggregate.items[i], t->fields[i].type,
                       &agg->items[i]);
        }
        *out = *agg;
    } else if (t->kind == TYPE_ARRAY) {
        agg = ir_const_agg(l->m, vtype_of(l, t), v->as.aggregate.count);
        for (i = 0; i < v->as.aggregate.count; i++) {
            const_tree(l, &v->as.aggregate.items[i], t->element,
                       &agg->items[i]);
        }
        *out = *agg;
    } else {
        const_scalar(l, v, ir_type_of(t), out);
    }
}

/* DESIGN: an aggregate constant is read-only data of the module, named by
   its index there as a literal is. Every use reads the same bytes, the
   copy is one memcopy, and a constant that no use reaches costs nothing.
   Two constants of equal value share the data. */
static struct ir_operand const_address(struct lowerer *l,
                                       const struct const_value *v,
                                       const struct type *t)
{
    struct ir_module *m = l->m;
    struct ir_const *value = arena_alloc(m->arena, sizeof *value);
    char name[16];
    size_t i;

    const_tree(l, v, t, value);
    for (i = 0; i < m->global_count; i++) {
        const struct ir_global *g = m->globals[i];
        if (strcmp(g->module, l->module_name) == 0 &&
            ir_const_equal(g->value, value)) {
            return temp(l, ir_addr(l->f, l->b, ir_global_op(g)));
        }
    }
    snprintf(name, sizeof name, "%u", globals_of_module(l));
    return temp(l, ir_addr(l->f, l->b, ir_global_op(
                    ir_global_add_value(m, l->module_name, name, value))));
}

static void build_into(struct lowerer *l, const struct expr *e,
                       struct ir_operand dest);

/* Put the value of e, of type t, at address. */
static void store_value(struct lowerer *l, const struct type *t,
                        const struct expr *e, struct ir_operand address)
{
    struct ir_operand v;

    if (is_aggregate(t)) {
        build_into(l, e, address);
        return;
    }
    v = lower_expr(l, e);
    if (!l->failed) {
        ir_store(l->f, l->b, ir_type_of(t), v, address);
    }
}

/* [v; n] computes v once and fills the n elements in a loop. */
static void fill_array(struct lowerer *l, const struct expr *e,
                       struct ir_operand dest)
{
    const struct type *element = e->type->element;
    struct ir_operand size = size_operand(l, element);
    struct ir_operand length = e->type->length_of != NULL
                                   ? ir_sym_operand(l->m,
                                                    sym_of(l, e->type->length_of))
                                   : ir_int_op(IR_I64, e->type->length);
    struct ir_operand v = none();
    struct ir_block *test;
    struct ir_block *body;
    struct ir_block *done;
    uint32_t index;
    uint32_t more;
    struct ir_operand at;

    if (is_aggregate(element)) {
        build_into(l, e->as.array_repeat.value, dest);
    } else {
        v = lower_expr(l, e->as.array_repeat.value);
    }
    if (l->failed) {
        return;
    }
    index = ir_unary(l->f, l->b, IR_COPY, IR_I64,
                     ir_int_op(IR_I64, is_aggregate(element) ? 1 : 0));
    test = new_block(l);
    body = new_block(l);
    done = new_block(l);
    ir_jump(l->f, l->b, test);
    l->b = test;
    more = ir_binary(l->f, l->b, IR_SLT, IR_I8, temp(l, index), length);
    ir_branch(l->f, l->b, temp(l, more), body, done);
    l->b = body;
    at = temp(l, ir_ptradd(l->f, l->b, dest,
                           temp(l, ir_binary(l->f, l->b, IR_MUL, IR_I64,
                                             temp(l, index), size))));
    if (is_aggregate(element)) {
        ir_memcopy(l->f, l->b, at, dest, vtype_of(l, element));
    } else {
        ir_store(l->f, l->b, ir_type_of(element), v, at);
    }
    ir_assign(l->f, l->b, index,
              temp(l, ir_binary(l->f, l->b, IR_ADD, IR_I64, temp(l, index),
                                ir_int_op(IR_I64, 1))));
    ir_jump(l->f, l->b, test);
    l->b = done;
}

/* a[lo..hi] points lo elements after element 0 of a and holds hi - lo
   elements. */
static void build_slice(struct lowerer *l, const struct expr *e,
                        struct ir_operand dest)
{
    struct ir_operand base = first_element(l, e->as.slice.base);
    struct ir_operand low = lower_expr(l, e->as.slice.low);
    struct ir_operand high = lower_expr(l, e->as.slice.high);
    uint32_t offset;

    if (l->failed) {
        return;
    }
    offset = ir_binary(l->f, l->b, IR_MUL, IR_I64, low,
                       size_operand(l, e->type->element));
    ir_store(l->f, l->b, IR_PTR,
             temp(l, ir_ptradd(l->f, l->b, base, temp(l, offset))), dest);
    ir_store(l->f, l->b, IR_I64,
             temp(l, ir_binary(l->f, l->b, IR_SUB, IR_I64, high, low)),
             offset_address(l, dest, field_offset(l, e->type, &len_name)));
}

/* Construct the aggregate value of e at dest. A literal fills its fields
   or elements in place, and any other value is copied. */
static void build_into(struct lowerer *l, const struct expr *e,
                       struct ir_operand dest)
{
    const struct type *t = e->type;
    struct ir_operand src;
    size_t i;

    switch (e->kind) {
    case EXPR_STRUCT_LIT:
        for (i = 0; i < e->as.struct_lit.field_count && !l->failed; i++) {
            const struct field_init *init = &e->as.struct_lit.fields[i];
            const struct struct_field *field = field_of(t, &init->name);
            if (field->bits != 0) {
                struct ir_operand v = lower_expr(l, init->value);
                if (!l->failed) {
                    ir_bitstore(l->f, l->b, ir_type_of(field->type), v, dest,
                                agg_of(l, t),
                                (uint32_t)(field - t->fields));
                }
                continue;
            }
            store_value(l, field->type, init->value,
                        offset_address(l, dest,
                                       field_offset(l, t, &field->name)));
        }
        /* DESIGN: a field the literal leaves out has a default, which the
           checker required, and its expression is written here. The value
           is therefore complete however the literal was written. */
        for (i = 0; i < t->field_count && !l->failed; i++) {
            const struct struct_field *field = &t->fields[i];
            size_t k;
            bool given = false;
            if (field->value == NULL) {
                continue;
            }
            for (k = 0; k < e->as.struct_lit.field_count; k++) {
                given = given ||
                        (e->as.struct_lit.fields[k].name.length ==
                             field->name.length &&
                         memcmp(e->as.struct_lit.fields[k].name.text,
                                field->name.text, field->name.length) == 0);
            }
            if (given) {
                continue;
            }
            if (field->bits != 0) {
                struct ir_operand v =
                    lower_expr(l, (struct expr *)field->value);
                if (!l->failed) {
                    ir_bitstore(l->f, l->b, ir_type_of(field->type), v, dest,
                                agg_of(l, t), (uint32_t)i);
                }
                continue;
            }
            store_value(l, field->type, (struct expr *)field->value,
                        offset_address(l, dest,
                                       field_offset(l, t, &field->name)));
        }
        break;
    case EXPR_ARRAY_LIT:
        for (i = 0; i < e->as.array_lit.count && !l->failed; i++) {
            store_value(l, t->element, e->as.array_lit.elements[i],
                        offset_address(l, dest,
                                       element_offset(l, t->element, i)));
        }
        break;
    case EXPR_ARRAY_REPEAT:
        fill_array(l, e, dest);
        break;
    case EXPR_STRING:
    case EXPR_BYTES:
        ir_store(l->f, l->b, IR_PTR, literal_address(l, &e->as.text), dest);
        ir_store(l->f, l->b, IR_I64, ir_int_op(IR_I64, e->as.text.length),
                 offset_address(l, dest, field_offset(l, t, &len_name)));
        break;
    case EXPR_SLICE_LIT:
        for (i = 0; i < e->as.slice_lit.field_count && !l->failed; i++) {
            const struct field_init *init = &e->as.slice_lit.fields[i];
            bool len = name_is(&init->name, "len");
            struct ir_operand v = lower_expr(l, init->value);
            if (!l->failed) {
                ir_store(l->f, l->b, len ? IR_I64 : IR_PTR, v,
                         offset_address(l, dest,
                                        field_offset(l, t, &init->name)));
            }
        }
        break;
    case EXPR_SLICE:
        build_slice(l, e, dest);
        break;
    default:
        src = lower_address(l, e);
        if (!l->failed) {
            ir_memcopy(l->f, l->b, dest, src, vtype_of(l, t));
        }
        break;
    }
}

static struct ir_operand lower_call(struct lowerer *l, const struct expr *e);
static struct ir_operand lower_parallel(struct lowerer *l,
                                       const struct expr *e);

/* The address of the memory that holds the aggregate value of e. A
   literal gets a slot of its own, and a constant is read-only data. */
static struct ir_operand lower_address(struct lowerer *l,
                                       const struct expr *e)
{
    const struct symbol *sym = e->symbol;
    uint32_t slot;

    if (l->failed) {
        return none();
    }
    if (sym != NULL && sym->kind == SYMBOL_CONST &&
        (e->kind == EXPR_NAME || e->kind == EXPR_FIELD)) {
        return const_address(l, sym->value, e->type);
    }
    switch (e->kind) {
    case EXPR_NAME:
        return temp(l, sym->ir);
    case EXPR_FIELD:
        return field_address(l, e);
    case EXPR_INDEX:
        return element_address(l, e);
    case EXPR_UNARY:
        return lower_expr(l, e->as.unary.operand);
    case EXPR_CALL:
        return lower_call(l, e);
    case EXPR_PARALLEL:
        return lower_parallel(l, e);
    default:
        slot = ir_entry_slot(l->f, vtype_of(l, e->type));
        build_into(l, e, temp(l, slot));
        return temp(l, slot);
    }
}

static struct ir_operand read_place(struct lowerer *l, const struct place *p)
{
    if (p->in_temp) {
        return temp(l, p->temp);
    }
    if (p->bitfield) {
        return temp(l, ir_bitload(l->f, l->b, p->type, p->address, p->agg,
                                  p->field));
    }
    return temp(l, ir_load(l->f, l->b, p->type, p->address));
}

static struct ir_operand lower_name(struct lowerer *l, const struct expr *e)
{
    const struct symbol *sym = e->symbol;
    struct place p;

    if (sym->kind == SYMBOL_CONST) {
        return constant(l, sym->value, ir_type_of(e->type));
    }
    if (sym->kind == SYMBOL_FN || sym->kind == SYMBOL_EXTERN_FN) {
        return temp(l, ir_addr(l->f, l->b,
                               ir_func_op(callee_function(l, sym))));
    }
    lower_place(l, e, &p);
    return read_place(l, &p);
}

static struct ir_operand lower_unary(struct lowerer *l, const struct expr *e)
{
    const struct expr *operand = e->as.unary.operand;
    enum ir_type type = ir_type_of(e->type);
    struct ir_operand v;
    struct place p;

    switch (e->as.unary.op) {
    case TOKEN_MINUS:
        /* A '-' directly before a literal forms one constant. */
        if (operand->kind == EXPR_INT) {
            return ir_int_op(type, 0 - operand->as.integer);
        }
        if (operand->kind == EXPR_FLOAT) {
            return ir_float_op(type, -float_literal(operand, type));
        }
        v = lower_expr(l, operand);
        return l->failed ? none()
                         : temp(l, ir_unary(l->f, l->b,
                                            type == IR_F32 || type == IR_F64
                                                ? IR_FNEG
                                                : IR_NEG,
                                            type, v));
    case TOKEN_BANG:
        v = lower_expr(l, operand);
        return l->failed ? none()
                         : temp(l, ir_binary(l->f, l->b, IR_XOR, IR_I8, v,
                                             ir_int_op(IR_I8, 1)));
    case TOKEN_TILDE:
        v = lower_expr(l, operand);
        return l->failed ? none()
                         : temp(l, ir_unary(l->f, l->b, IR_NOT, type, v));
    case TOKEN_STAR:
        v = lower_expr(l, operand);
        return l->failed ? none()
                         : temp(l, ir_load(l->f, l->b, type, v));
    default: /* TOKEN_AMP: semantic analysis marked the operand */
        return lower_place(l, operand, &p) ? p.address : none();
    }
}

static enum ir_op binary_op(enum token_kind op, const struct type *t)
{
    bool is_float = type_is_float(t);
    bool is_signed = type_is_signed(t);

    switch (op) {
    case TOKEN_PLUS: return is_float ? IR_FADD : IR_ADD;
    case TOKEN_MINUS: return is_float ? IR_FSUB : IR_SUB;
    case TOKEN_STAR: return is_float ? IR_FMUL : IR_MUL;
    case TOKEN_SLASH: return is_float ? IR_FDIV : is_signed ? IR_SDIV : IR_UDIV;
    case TOKEN_PERCENT: return is_signed ? IR_SREM : IR_UREM;
    case TOKEN_AMP: return IR_AND;
    case TOKEN_PIPE: return IR_OR;
    case TOKEN_CARET: return IR_XOR;
    case TOKEN_SHL: return IR_SHL;
    case TOKEN_SHR: return is_signed ? IR_SHR_S : IR_SHR_U;
    case TOKEN_EQ: return is_float ? IR_FEQ : IR_EQ;
    case TOKEN_NE: return is_float ? IR_FNE : IR_NE;
    case TOKEN_LT: return is_float ? IR_FLT : is_signed ? IR_SLT : IR_ULT;
    case TOKEN_LE: return is_float ? IR_FLE : is_signed ? IR_SLE : IR_ULE;
    case TOKEN_GT: return is_float ? IR_FGT : is_signed ? IR_SGT : IR_UGT;
    default: return is_float ? IR_FGE : is_signed ? IR_SGE : IR_UGE;
    }
}

static bool is_comparison(enum token_kind op)
{
    return op == TOKEN_EQ || op == TOKEN_NE || op == TOKEN_LT ||
           op == TOKEN_LE || op == TOKEN_GT || op == TOKEN_GE;
}

/* a && b or a || b as a value. The result is a when a decides it, else b. */
static struct ir_operand short_circuit(struct lowerer *l, const struct expr *e)
{
    bool is_and = e->as.binary.op == TOKEN_AND_AND;
    struct ir_operand left = lower_expr(l, e->as.binary.left);
    struct ir_operand right;
    struct ir_block *rest;
    struct ir_block *join;
    uint32_t result;

    if (l->failed) {
        return none();
    }
    result = ir_unary(l->f, l->b, IR_COPY, IR_I8, left);
    rest = new_block(l);
    join = new_block(l);
    ir_branch(l->f, l->b, left, is_and ? rest : join, is_and ? join : rest);
    l->b = rest;
    right = lower_expr(l, e->as.binary.right);
    if (l->failed) {
        return none();
    }
    ir_assign(l->f, l->b, result, right);
    ir_jump(l->f, l->b, join);
    l->b = join;
    return temp(l, result);
}

static struct ir_operand lower_binary(struct lowerer *l, const struct expr *e)
{
    enum token_kind op = e->as.binary.op;
    const struct type *operands = e->as.binary.left->type;
    struct ir_operand left;
    struct ir_operand right;

    if (op == TOKEN_AND_AND || op == TOKEN_OR_OR) {
        return short_circuit(l, e);
    }
    left = lower_expr(l, e->as.binary.left);
    right = lower_expr(l, e->as.binary.right);
    if (l->failed) {
        return none();
    }
    return temp(l, ir_binary(l->f, l->b, binary_op(op, operands),
                             is_comparison(op) ? IR_I8 : ir_type_of(operands),
                             left, right));
}

/* The conversions of chapter 2. Two types with one IR type, such as u32
   and char, convert without an instruction. */
static struct ir_operand lower_cast(struct lowerer *l, const struct expr *e)
{
    const struct type *from = e->as.cast.operand->type;
    const struct type *to = e->type;
    enum ir_type source = ir_type_of(from);
    enum ir_type target = ir_type_of(to);
    struct ir_operand v = lower_expr(l, e->as.cast.operand);
    enum ir_op op;

    if (l->failed) {
        return none();
    }
    if (type_is_float(from) && type_is_float(to)) {
        if (source == target) {
            return v;
        }
        op = target == IR_F64 ? IR_FEXT : IR_FTRUNC;
    } else if (type_is_float(from)) {
        op = type_is_signed(to) ? IR_FTOSI : IR_FTOUI;
    } else if (type_is_float(to)) {
        op = type_is_signed(from) ? IR_SITOF : IR_UITOF;
    } else if (source == target) {
        return v;
    } else if (narrows(source, target)) {
        op = IR_TRUNC;
    } else {
        op = type_is_signed(from) ? IR_SEXT : IR_ZEXT;
    }
    return temp(l, ir_unary(l->f, l->b, op, target, v));
}

/* The IR form of a symbolic value: the operations of lower_binary and
   lower_cast on symbolic operands. */
static uint32_t sym_of(struct lowerer *l, const struct symbolic *s)
{
    enum ir_type type = ir_type_of(s->type);
    enum ir_type source;
    uint32_t a = 0;

    switch (s->kind) {
    case SYMBOLIC_INT:
        return ir_sym_int(l->m, type, s->value);
    case SYMBOLIC_SIZE_OF:
        return ir_sym_size_of(l->m, vtype_of(l, s->of));
    case SYMBOLIC_UNARY:
        a = sym_of(l, s->a);
        if (s->op == TOKEN_BANG) {
            return ir_sym_op(l->m, IR_XOR, IR_I8, a, ir_sym_int(l->m, IR_I8, 1));
        }
        return ir_sym_op(l->m, s->op == TOKEN_MINUS ? IR_NEG : IR_NOT, type, a,
                         IR_NO_AGG);
    case SYMBOLIC_BINARY:
        a = sym_of(l, s->a);
        if (s->op == TOKEN_AND_AND || s->op == TOKEN_OR_OR) {
            return ir_sym_op(l->m, s->op == TOKEN_AND_AND ? IR_AND : IR_OR,
                             IR_I8, a, sym_of(l, s->b));
        }
        return ir_sym_op(l->m, binary_op(s->op, s->a->type),
                         is_comparison(s->op) ? IR_I8 : type, a,
                         sym_of(l, s->b));
    case SYMBOLIC_CAST:
        a = sym_of(l, s->a);
        source = ir_type_of(s->a->type);
        if (source == type) {
            return a;
        }
        return ir_sym_op(l->m,
                         narrows(source, type)        ? IR_TRUNC
                         : type_is_signed(s->a->type) ? IR_SEXT
                                                      : IR_ZEXT,
                         type, a, IR_NO_AGG);
    }
    return a;
}

static struct ir_operand lower_call(struct lowerer *l, const struct expr *e)
{
    const struct expr *callee = e->as.call.callee;
    const struct symbol *sym = callee->kind == EXPR_NAME ? callee->symbol
                                                         : NULL;
    bool direct = sym != NULL && (sym->kind == SYMBOL_FN ||
                                  sym->kind == SYMBOL_EXTERN_FN);
    size_t n = e->as.call.arg_count;
    struct ir_operand target = none();
    struct ir_operand *args;
    uint32_t result;
    size_t i;

    /* The callee comes before the arguments, from left to right. */
    if (!direct) {
        target = lower_expr(l, callee);
    }
    args = malloc((n + 1) * sizeof *args);
    if (args == NULL) {
        fputs("antic: out of memory\n", stderr);
        exit(70);
    }
    for (i = 0; i < n; i++) {
        args[i] = lower_expr(l, e->as.call.args[i]);
    }
    if (l->failed) {
        free(args);
        return none();
    }
    if (direct) {
        result = ir_call(l->f, l->b, ir_type_of(e->type),
                         ir_func_op(callee_function(l, sym)), args, n);
    } else {
        result = ir_call_indirect(l->f, l->b, ir_type_of(e->type), target,
                                  signature(l, callee->type), args, n);
    }
    free(args);
    return result == IR_NO_RESULT ? none() : temp(l, result);
}

/* Threads */

/* The count of `parallel` thunks already written here, which names the
   next one. */
static uint32_t next_thunk(const struct lowerer *l)
{
    uint32_t count = 0;
    size_t i;

    for (i = 0; i < l->m->function_count; i++) {
        const struct ir_function *g = l->m->functions[i];
        if (g->module != NULL && strcmp(g->module, l->module_name) == 0 &&
            strncmp(g->name, "parallel.", 9) == 0) {
            count++;
        }
    }
    return count;
}

/* An extern declaration of a runtime function whose parameters are all
   scalars. A module that calls it twice shares the declaration. */
static struct ir_function *rt_function(struct lowerer *l, const char *name,
                                       const enum ir_type *params,
                                       size_t count)
{
    struct ir_function *f = find_function(l->m, NULL, name);
    size_t i;

    if (f == NULL) {
        f = ir_extern_add(l->m, name, IR_VOID, false);
        for (i = 0; i < count; i++) {
            ir_param_add(f, params[i], IR_NO_AGG);
        }
    }
    return f;
}

/* The aggregate that carries the arguments every chunk receives, or
   IR_NO_AGG when the worker takes the chunk alone. */
static uint32_t context_aggregate(struct lowerer *l, const struct expr *call,
                                  const char *name)
{
    struct ir_field *fields;
    uint32_t agg;
    size_t n = call->as.call.arg_count;
    size_t i;

    fields = malloc(n * sizeof *fields);
    if (fields == NULL) {
        fputs("antic: out of memory\n", stderr);
        exit(70);
    }
    for (i = 0; i < n; i++) {
        char *field = malloc(24);
        if (field == NULL) {
            fputs("antic: out of memory\n", stderr);
            exit(70);
        }
        snprintf(field, 24, "a%zu", i);
        fields[i].name = field;
        fields[i].type = vtype_of(l, call->as.call.args[i]->type);
        fields[i].bits = 0;
        fields[i].ext = IR_EXT_NONE;
    }
    agg = ir_struct_add(l->m, IR_AGG_STRUCT, name, fields, n, false, 0);
    for (i = 0; i < n; i++) {
        free((char *)fields[i].name);
    }
    free(fields);
    return agg;
}

/* DESIGN: the worker pool calls one C signature, and a worker has the
   signature its own declaration gives. antic writes a thunk for each
   `parallel` that joins the two. The thunk rebuilds the chunk from the
   pointer and the length. It then reads the arguments that every chunk
   shares out of the context, calls the worker and stores its result. */
static struct ir_function *parallel_thunk(struct lowerer *l,
                                          const struct expr *e,
                                          const char *name, uint32_t context)
{
    const struct expr *call = e->as.parallel.call;
    const struct expr *callee = call->kind == EXPR_CALL
                                    ? call->as.call.callee : call;
    const struct type *slice = e->as.parallel.array->type;
    const struct type *result = callee->symbol->type->result;
    size_t extra = call->kind == EXPR_CALL ? call->as.call.arg_count : 0;
    struct ir_function *outer_f = l->f;
    struct ir_block *outer_b = l->b;
    struct ir_function *f;
    struct ir_operand *args;
    struct ir_block *entry;
    struct ir_operand chunk;
    uint32_t slot;
    uint32_t value;
    size_t i;

    f = ir_function_add(l->m, l->module_name, name, IR_VOID, IR_NO_AGG);
    ir_param_add(f, IR_PTR, IR_NO_AGG);     /* the context */
    ir_param_add(f, IR_PTR, IR_NO_AGG);     /* the first element */
    ir_param_add(f, IR_I64, IR_NO_AGG);     /* the element count */
    ir_param_add(f, IR_PTR, IR_NO_AGG);     /* where the result goes */
    entry = ir_block_add(f);
    l->f = f;
    l->b = entry;

    slot = ir_slot(f, entry, vtype_of(l, slice));
    ir_store(f, entry, IR_PTR, temp(l, f->params[1].temp), temp(l, slot));
    ir_store(f, entry, IR_I64, temp(l, f->params[2].temp),
             offset_address(l, temp(l, slot),
                            field_offset(l, slice, &len_name)));

    args = malloc((extra + 1) * sizeof *args);
    if (args == NULL) {
        fputs("antic: out of memory\n", stderr);
        exit(70);
    }
    args[0] = temp(l, slot);
    for (i = 0; i < extra; i++) {
        const struct type *t = call->as.call.args[i]->type;
        struct ir_operand at =
            offset_address(l, temp(l, f->params[0].temp),
                           ir_sym_operand(l->m,
                                          ir_sym_offset_of(l->m, context,
                                                           (uint32_t)i)));
        args[i + 1] = is_aggregate(t)
                          ? at
                          : temp(l, ir_load(f, entry, ir_type_of(t), at));
    }
    value = ir_call(f, entry, ir_type_of(result),
                    ir_func_op(callee_function(l, callee->symbol)), args,
                    extra + 1);
    free(args);
    chunk = temp(l, f->params[3].temp);
    if (is_aggregate(result)) {
        ir_memcopy(f, entry, chunk, temp(l, value), vtype_of(l, result));
    } else {
        ir_store(f, entry, ir_type_of(result), temp(l, value), chunk);
    }
    ir_ret(f, entry, IR_VOID, none());
    l->f = outer_f;
    l->b = outer_b;
    return f;
}

/* DESIGN: `parallel a by n -> f(x)` becomes one call of the runtime. The
   runtime decides the chunk count when n is absent. It therefore
   allocates the array of results and writes back the pointer and the
   count, and the expression is the slice of those results. */
static struct ir_operand lower_parallel(struct lowerer *l,
                                        const struct expr *e)
{
    static const enum ir_type signature[] = {IR_PTR, IR_I64, IR_I64, IR_I64,
                                             IR_I64, IR_PTR, IR_PTR, IR_PTR,
                                             IR_PTR};
    const struct expr *call = e->as.parallel.call;
    const struct expr *callee = call->kind == EXPR_CALL
                                    ? call->as.call.callee : call;
    const struct type *slice = e->as.parallel.array->type;
    const struct type *result = callee->symbol->type->result;
    size_t extra = call->kind == EXPR_CALL ? call->as.call.arg_count : 0;
    struct ir_operand context = ir_int_op(IR_PTR, 0);
    struct ir_operand args[9];
    struct ir_operand array;
    uint32_t agg = IR_NO_AGG;
    uint32_t results;
    uint32_t count;
    uint32_t out;
    char name[24];
    size_t i;

    snprintf(name, sizeof name, "parallel.%u", next_thunk(l));
    array = lower_address(l, e->as.parallel.array);
    if (l->failed) {
        return none();
    }
    if (extra > 0) {
        char context_name[32];
        uint32_t slot;
        snprintf(context_name, sizeof context_name, "%s.context", name);
        agg = context_aggregate(l, call, context_name);
        slot = ir_slot(l->f, l->b, ir_aggregate(agg));
        for (i = 0; i < extra; i++) {
            const struct expr *arg = call->as.call.args[i];
            struct ir_operand at =
                offset_address(l, temp(l, slot),
                               ir_sym_operand(l->m,
                                              ir_sym_offset_of(l->m, agg,
                                                               (uint32_t)i)));
            store_value(l, arg->type, arg, at);
        }
        context = temp(l, slot);
    }
    results = ir_slot(l->f, l->b, ir_scalar(IR_PTR));
    count = ir_slot(l->f, l->b, ir_scalar(IR_I64));
    args[0] = temp(l, ir_load(l->f, l->b, IR_PTR, array));
    args[1] = temp(l, ir_load(l->f, l->b, IR_I64,
                              offset_address(l, array,
                                             field_offset(l, slice,
                                                          &len_name))));
    args[2] = size_operand(l, slice->element);
    args[3] = e->as.parallel.chunks != NULL
                  ? lower_expr(l, e->as.parallel.chunks)
                  : ir_int_op(IR_I64, 0);
    args[4] = size_operand(l, result);
    args[5] = temp(l, ir_addr(l->f, l->b,
                              ir_func_op(parallel_thunk(l, e, name,
                                                        agg))));
    args[6] = context;
    args[7] = temp(l, results);
    args[8] = temp(l, count);
    if (l->failed) {
        return none();
    }
    ir_call(l->f, l->b, IR_VOID,
            ir_func_op(rt_function(l, "anti_rt_parallel", signature, 9)),
            args, 9);
    out = ir_slot(l->f, l->b, vtype_of(l, e->type));
    ir_store(l->f, l->b, IR_PTR, temp(l, ir_load(l->f, l->b, IR_PTR,
                                                 temp(l, results))),
             temp(l, out));
    ir_store(l->f, l->b, IR_I64,
             temp(l, ir_load(l->f, l->b, IR_I64, temp(l, count))),
             offset_address(l, temp(l, out),
                            field_offset(l, e->type, &len_name)));
    return temp(l, out);
}

static struct ir_operand lower_expr(struct lowerer *l, const struct expr *e)
{
    enum ir_type type;
    struct ir_operand v;
    struct place p;
    uint32_t size;

    if (l->failed) {
        return none();
    }
    if (is_aggregate(e->type)) {
        return lower_address(l, e);
    }
    type = ir_type_of(e->type);
    switch (e->kind) {
    case EXPR_INT:
        return ir_int_op(type, e->as.integer);
    case EXPR_FLOAT:
        return ir_float_op(type, float_literal(e, type));
    case EXPR_CHAR:
        return ir_int_op(type, e->as.character);
    case EXPR_BOOL:
        return ir_int_op(type, e->as.boolean);
    case EXPR_NULL:
        return ir_int_op(type, 0);
    case EXPR_NAME:
        return lower_name(l, e);
    case EXPR_UNARY:
        return lower_unary(l, e);
    case EXPR_BINARY:
        return lower_binary(l, e);
    case EXPR_CAST:
        return lower_cast(l, e);
    case EXPR_CALL:
        return lower_call(l, e);
    case EXPR_PARALLEL:
        return lower_parallel(l, e);
    case EXPR_FIELD:
        if (e->symbol != NULL && e->symbol->kind == SYMBOL_CONST) {
            return constant(l, e->symbol->value, type);
        }
        if (e->symbol != NULL && (e->symbol->kind == SYMBOL_FN ||
                                  e->symbol->kind == SYMBOL_EXTERN_FN)) {
            return temp(l, ir_addr(l->f, l->b,
                                   ir_func_op(callee_function(l, e->symbol))));
        }
        /* A value of an enum is the number the checker folded, in the
           underlying type of the enum. */
        if (e->as.field.enum_value != 0 && e->type->kind == TYPE_ENUM) {
            return ir_int_op(ir_type_of(e->type->base),
                             e->type->fields[e->as.field.enum_value - 1]
                                 .number);
        }
        if (e->as.field.base->type->kind == TYPE_ARRAY) {
            /* .len is the only field of an array. */
            const struct type *array = e->as.field.base->type;
            lower_expr(l, e->as.field.base);
            return array->length_of != NULL
                       ? ir_sym_operand(l->m, sym_of(l, array->length_of))
                       : ir_int_op(IR_I64, array->length);
        }
        return lower_place(l, e, &p) ? read_place(l, &p) : none();
    case EXPR_INDEX:
        return lower_place(l, e, &p) ? read_place(l, &p) : none();
    case EXPR_ALLOC:
        v = lower_expr(l, e->as.alloc.count);
        if (l->failed) {
            return none();
        }
        size = ir_binary(l->f, l->b, IR_MUL, IR_I64, v,
                         size_operand(l, e->type->element));
        v = temp(l, size);
        return temp(l, ir_call(l->f, l->b, IR_PTR,
                               ir_func_op(c_function(l, "malloc", IR_PTR,
                                                     IR_I64)),
                               &v, 1));
    case EXPR_FREE:
        v = lower_expr(l, e->as.free_pointer);
        if (!l->failed) {
            ir_call(l->f, l->b, IR_VOID,
                    ir_func_op(c_function(l, "free", IR_VOID, IR_PTR)), &v, 1);
        }
        return none();
    case EXPR_SIZE_OF:
        return size_operand(l, e->as.size_of->type);
    default:
        /* Literals of aggregates returned their address above. */
        return none();
    }
}

/* Conditions */

/* Branch to then_block when e is true and to else_block when it is false.
   && and || branch after each operand, so a condition computes no bool
   value. The current block ends with the branch. */
static void lower_branch(struct lowerer *l, const struct expr *e,
                         struct ir_block *then_block,
                         struct ir_block *else_block)
{
    struct ir_block *rest;
    struct ir_operand v;

    if (e->kind == EXPR_BINARY && (e->as.binary.op == TOKEN_AND_AND ||
                                   e->as.binary.op == TOKEN_OR_OR)) {
        rest = new_block(l);
        if (e->as.binary.op == TOKEN_AND_AND) {
            lower_branch(l, e->as.binary.left, rest, else_block);
        } else {
            lower_branch(l, e->as.binary.left, then_block, rest);
        }
        l->b = rest;
        lower_branch(l, e->as.binary.right, then_block, else_block);
        return;
    }
    if (e->kind == EXPR_UNARY && e->as.unary.op == TOKEN_BANG) {
        lower_branch(l, e->as.unary.operand, else_block, then_block);
        return;
    }
    v = lower_expr(l, e);
    if (!l->failed) {
        ir_branch(l->f, l->b, v, then_block, else_block);
    }
}

/* Statements */

static void lower_block(struct lowerer *l, const struct block *b);

static void jump_to_join(struct lowerer *l, struct ir_block **join)
{
    if (l->b == NULL) {
        return;
    }
    if (*join == NULL) {
        *join = new_block(l);
    }
    ir_jump(l->f, l->b, *join);
}

/* Each condition of the chain branches to its body or to the next
   condition. The join block after the chain exists only when some path
   reaches the end of the chain. */
static void lower_if(struct lowerer *l, const struct stmt *s)
{
    size_t count = s->as.if_chain.count;
    struct ir_block *join = NULL;
    size_t i;

    for (i = 0; i < count && !l->failed; i++) {
        const struct if_branch *branch = &s->as.if_chain.branches[i];
        struct ir_block *then_block = new_block(l);
        struct ir_block *next;

        if (i + 1 < count || s->as.if_chain.else_body != NULL) {
            next = new_block(l);
        } else {
            if (join == NULL) {
                join = new_block(l);
            }
            next = join;
        }
        lower_branch(l, branch->cond, then_block, next);
        l->b = then_block;
        lower_block(l, branch->body);
        jump_to_join(l, &join);
        l->b = next;
    }
    if (s->as.if_chain.else_body != NULL && !l->failed) {
        lower_block(l, s->as.if_chain.else_body);
        jump_to_join(l, &join);
    }
    l->b = join;
}

static void lower_loop(struct lowerer *l, const struct stmt *s)
{
    bool is_while = s->kind == STMT_WHILE;
    struct ir_block *first = new_block(l);
    struct ir_block *second = new_block(l);
    struct ir_block *exit = new_block(l);
    struct ir_block *test = is_while ? first : second;
    struct ir_block *body = is_while ? second : first;
    struct loop loop;

    loop.continue_to = test;
    loop.break_to = exit;
    loop.outer = l->loop;
    ir_jump(l->f, l->b, first);
    if (is_while) {
        l->b = test;
        lower_branch(l, s->as.loop.cond, body, exit);
    }
    l->loop = &loop;
    l->b = body;
    lower_block(l, s->as.loop.body);
    if (l->b != NULL) {
        ir_jump(l->f, l->b, test);
    }
    l->loop = loop.outer;
    if (!is_while) {
        l->b = test;
        lower_branch(l, s->as.loop.cond, body, exit);
    }
    l->b = exit;
}

static enum token_kind compound_op(enum token_kind op)
{
    switch (op) {
    case TOKEN_PLUS_ASSIGN: return TOKEN_PLUS;
    case TOKEN_MINUS_ASSIGN: return TOKEN_MINUS;
    case TOKEN_STAR_ASSIGN: return TOKEN_STAR;
    case TOKEN_SLASH_ASSIGN: return TOKEN_SLASH;
    case TOKEN_PERCENT_ASSIGN: return TOKEN_PERCENT;
    case TOKEN_AMP_ASSIGN: return TOKEN_AMP;
    case TOKEN_PIPE_ASSIGN: return TOKEN_PIPE;
    case TOKEN_CARET_ASSIGN: return TOKEN_CARET;
    case TOKEN_SHL_ASSIGN: return TOKEN_SHL;
    default: return TOKEN_SHR;
    }
}

/* Chapter 2 evaluates the place first and the value second. A compound
   assignment reads the old value before it evaluates the new operand, as
   x = x + e reads x first. */
static void lower_assign(struct lowerer *l, const struct stmt *s)
{
    const struct expr *target = s->as.assign.target;
    struct place p;
    struct ir_operand old = none();
    struct ir_operand v;

    if (!lower_place(l, target, &p)) {
        return;
    }
    if (is_aggregate(target->type)) {
        v = lower_address(l, s->as.assign.value);
        if (!l->failed) {
            ir_memcopy(l->f, l->b, p.address, v, vtype_of(l, target->type));
        }
        return;
    }
    if (s->as.assign.op != TOKEN_ASSIGN) {
        old = read_place(l, &p);
    }
    v = lower_expr(l, s->as.assign.value);
    if (l->failed) {
        return;
    }
    if (s->as.assign.op != TOKEN_ASSIGN) {
        v = temp(l, ir_binary(l->f, l->b,
                              binary_op(compound_op(s->as.assign.op),
                                        target->type),
                              p.type, old, v));
    }
    if (p.in_temp) {
        ir_assign(l->f, l->b, p.temp, v);
    } else if (p.bitfield) {
        ir_bitstore(l->f, l->b, p.type, v, p.address, p.agg, p.field);
    } else {
        ir_store(l->f, l->b, p.type, v, p.address);
    }
}

/* A local that is not address-taken gets a temporary of its own. An
   assignment to the variable it was copied from leaves it unchanged. */
static void lower_let(struct lowerer *l, const struct stmt *s)
{
    struct symbol *sym = s->as.let.symbol;
    struct ir_operand v;

    if (is_aggregate(sym->type)) {
        build_into(l, s->as.let.value, temp(l, sym->ir));
        return;
    }
    v = lower_expr(l, s->as.let.value);
    if (l->failed) {
        return;
    }
    if (sym->address_taken) {
        ir_store(l->f, l->b, ir_type_of(sym->type), v, temp(l, sym->ir));
    } else {
        sym->ir = ir_unary(l->f, l->b, IR_COPY, ir_type_of(sym->type), v);
    }
}

static void lower_stmt(struct lowerer *l, const struct stmt *s)
{
    struct ir_operand v;

    switch (s->kind) {
    case STMT_LET:
        lower_let(l, s);
        return;
    case STMT_CONST:
        /* Semantic analysis computed the value, and every use is a
           constant operand. */
        return;
    case STMT_EXPR:
        lower_expr(l, s->as.expr);
        return;
    case STMT_ASSIGN:
        lower_assign(l, s);
        return;
    case STMT_IF:
        lower_if(l, s);
        return;
    case STMT_WHILE:
    case STMT_DO_WHILE:
        lower_loop(l, s);
        return;
    case STMT_BREAK:
        ir_jump(l->f, l->b, l->loop->break_to);
        l->b = NULL;
        return;
    case STMT_CONTINUE:
        ir_jump(l->f, l->b, l->loop->continue_to);
        l->b = NULL;
        return;
    case STMT_RETURN:
        if (s->as.return_value == NULL) {
            ir_ret(l->f, l->b, IR_VOID, none());
        } else {
            v = lower_expr(l, s->as.return_value);
            if (l->failed) {
                return;
            }
            ir_ret(l->f, l->b, l->f->result == IR_AGG ? IR_PTR : l->f->result,
                   v);
        }
        l->b = NULL;
        return;
    case STMT_BLOCK:
        lower_block(l, s->as.block);
        return;
    }
}

/* Anti has no labels, so a statement after return, break or continue is
   unreachable. Lowering skips it. */
static void lower_block(struct lowerer *l, const struct block *b)
{
    size_t i;

    for (i = 0; i < b->count && l->b != NULL && !l->failed; i++) {
        lower_stmt(l, b->stmts[i]);
    }
}

/* Functions */

/* DESIGN: every address-taken local gets its stack slot in the entry
   block, before the first statement. A slot in a loop body would suggest
   a new slot per iteration, and the back end reserves each one once. */
static void reserve_slots(struct lowerer *l, struct ir_block *entry,
                          const struct block *b)
{
    size_t i;
    size_t j;

    for (i = 0; i < b->count; i++) {
        const struct stmt *s = b->stmts[i];
        struct symbol *sym;

        switch (s->kind) {
        case STMT_LET:
            sym = s->as.let.symbol;
            if (sym->address_taken || is_aggregate(sym->type)) {
                sym->ir = ir_slot(l->f, entry, vtype_of(l, sym->type));
            }
            break;
        case STMT_IF:
            for (j = 0; j < s->as.if_chain.count; j++) {
                reserve_slots(l, entry, s->as.if_chain.branches[j].body);
            }
            if (s->as.if_chain.else_body != NULL) {
                reserve_slots(l, entry, s->as.if_chain.else_body);
            }
            break;
        case STMT_WHILE:
        case STMT_DO_WHILE:
            reserve_slots(l, entry, s->as.loop.body);
            break;
        case STMT_BLOCK:
            reserve_slots(l, entry, s->as.block);
            break;
        default:
            break;
        }
    }
}

static void declare_function(struct lowerer *l, struct item *it)
{
    const struct type *t = it->symbol->type;
    /* A function of a struct body carries the name `T.f`, which its
       symbol holds, so its symbol becomes `module.T.f`. */
    char *name = cstr(it->owner != NULL ? &it->symbol->name : &it->name);
    struct ir_function *f;
    size_t i;

    f = it->kind == ITEM_EXTERN_FN ? find_function(l->m, NULL, name) : NULL;
    if (f == NULL) {
        f = it->kind == ITEM_FN
                ? ir_function_add(l->m, l->module_name, name,
                                  ir_type_of(t->result),
                                  result_agg(l, t->result))
                : ir_extern_add(l->m, name, ir_type_of(t->result),
                                it->variadic);
        f->result_agg = result_agg(l, t->result);
        f->exported = it->exported;
        for (i = 0; i < t->param_count; i++) {
            add_param(l, f, t->params[i]);
        }
    }
    free(name);
    it->symbol->ir = f->index;
}

static void lower_function(struct lowerer *l, struct item *it)
{
    struct ir_block *entry;
    size_t i;

    l->f = l->m->functions[it->symbol->ir];
    l->loop = NULL;
    entry = new_block(l);
    for (i = 0; i < it->param_count; i++) {
        struct symbol *sym = it->params[i].symbol;
        sym->ir = l->f->params[i].temp;
        if (sym->address_taken && !is_aggregate(sym->type)) {
            sym->ir = ir_slot(l->f, entry, vtype_of(l, sym->type));
        }
    }
    reserve_slots(l, entry, it->body);
    for (i = 0; i < it->param_count; i++) {
        const struct symbol *sym = it->params[i].symbol;
        if (sym->address_taken && !is_aggregate(sym->type)) {
            ir_store(l->f, entry, l->f->params[i].type,
                     temp(l, l->f->params[i].temp), temp(l, sym->ir));
        }
    }
    l->b = entry;
    lower_block(l, it->body);
    /* Semantic analysis rejects a function with a result that can reach
       its end, so only a function without one gets here. */
    if (l->b != NULL && !l->failed) {
        ir_ret(l->f, l->b, IR_VOID, none());
    }
}

bool lower_module(struct module *module, const char *module_name,
                  struct ir_module *out, struct diagnostics *diags)
{
    struct lowerer l;
    bool ok = true;
    size_t i;

    memset(&l, 0, sizeof l);
    l.m = out;
    l.diags = diags;
    l.module_name = module_name;
    /* A function of a struct body is a function of the module with one
       more segment in its name. It is declared and lowered like a free
       function. */
    for (i = 0; i < module->item_count; i++) {
        struct item *it = module->items[i];
        size_t j;
        if (it->kind == ITEM_FN || it->kind == ITEM_EXTERN_FN) {
            declare_function(&l, it);
        }
        for (j = 0; j < it->member_count; j++) {
            if (it->members[j]->kind == ITEM_FN &&
                it->members[j]->body != NULL) {
                declare_function(&l, it->members[j]);
            }
        }
    }
    for (i = 0; i < module->item_count; i++) {
        struct item *it = module->items[i];
        size_t j;
        l.failed = false;
        if (it->kind == ITEM_FN) {
            lower_function(&l, it);
        }
        for (j = 0; j < it->member_count; j++) {
            if (it->members[j]->kind == ITEM_FN &&
                it->members[j]->body != NULL) {
                lower_function(&l, it->members[j]);
            }
        }
        ok = ok && !l.failed;
    }
    return ok;
}
