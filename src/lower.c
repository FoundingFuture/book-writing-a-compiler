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

struct defers;

struct loop {
    struct ir_block *continue_to;
    struct ir_block *break_to;
    struct loop *outer;
    struct defers *defers_at;   /* the block scope the loop started in */
};

/* DESIGN: the statements of `defer` are recorded per block and run at
   every exit of it, in reverse order of declaration. A `return` runs the
   scopes of the whole function, and `break` or `continue` those the loop
   encloses. Nothing unwinds. */
/* One exit action of a block: a deferred statement, or the end of a
   local whose class has to be torn down. */
struct exit_action {
    const struct stmt *stmt;
    const struct symbol *local;
};

struct defers {
    struct exit_action *items;
    size_t count;
    size_t capacity;
    struct defers *outer;
};

/* Where a handler sends control, and where `yield` puts its value. */
struct handling {
    struct ir_block *join;      /* the block after the call */
    struct ir_operand out;      /* where the result is written */
    uint32_t error;             /* the error the handler binds */
    bool has_out;
    bool passes;                /* a `return e` hands the error on */
    struct handling *outer;
};

/* The one handler of a `try` block, which every failing call inside it
   reaches. */
struct try_scope {
    struct ir_block *handler;
    uint32_t error;             /* the temporary the error lands in */
    struct try_scope *outer;
};

struct lowerer {
    struct ir_module *m;
    const char *file;           /* the source path, for an assertion */
    struct diagnostics *diags;
    const char *module_name;
    struct ir_function *f;
    struct ir_block *b;         /* NULL after a terminator */
    struct loop *loop;
    struct defers *defers;
    uint32_t loop_depth;        /* the loops the next block sits inside */
    struct handling *handling;  /* the handler being lowered */
    struct try_scope *try_scope;
    struct ir_operand out_address;  /* the out parameter of a failing call */
    const struct symbol *moved;     /* the local a `return` hands over */
    bool no_reflect;            /* --no-reflect: no field list */
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
    case TYPE_CLASS:
    case TYPE_ARRAY:
    case TYPE_STR:
    case TYPE_SLICE:
        return IR_AGG;
    default:
        return IR_VOID;
    }
}

/* A str, a slice and a bound function are aggregates of two words. */
static bool is_aggregate(const struct type *t)
{
    return type_has_fields(t) || t->kind == TYPE_ARRAY ||
           t->kind == TYPE_STR || t->kind == TYPE_SLICE ||
           (t->kind == TYPE_FN && t->bound);
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
    size_t count = type_has_fields(t) ? t->field_count : 2;
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
    } else if (type_has_fields(t)) {
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
    } else if (t->kind == TYPE_FN) {
        /* A bound function is the object and the entry of its table. */
        fields[0].name = "object";
        fields[0].type = ir_scalar(IR_PTR);
        fields[1].name = "entry";
        fields[1].type = ir_scalar(IR_PTR);
        agg = ir_struct_add(l->m, IR_AGG_STRUCT, name, fields, 2, false, 0);
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

/* Every block records the loops it sits inside. The allocator weighs a
   spill by it, because a value in a loop is read again on every pass. */
static struct ir_block *new_block(struct lowerer *l)
{
    struct ir_block *b = ir_block_add(l->f);

    b->loop_depth = l->loop_depth;
    return b;
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

    /* DESIGN: a function of the root has no source. Its body is a symbol
       of the runtime under the `anti_rt_Object_` prefix, declared with
       the signature the checker gave the function. */
    if (sym->item != NULL && sym->item->runtime != NULL) {
        char symbol[64];
        snprintf(symbol, sizeof symbol, "anti_rt_Object_%s",
                 sym->item->runtime);
        f = find_function(l->m, NULL, symbol);
        if (f == NULL) {
            f = ir_extern_add(l->m, symbol, ir_type_of(t->result), false);
            f->result_agg = result_agg(l, t->result);
            for (i = 0; i < t->param_count; i++) {
                add_param(l, f, t->params[i]);
            }
        }
        return f;
    }
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

/* The signature of `fatal`, which takes the error and returns nothing.
   `catch fatal` calls it through the table of the error's class. */
static const struct ir_function *fatal_signature(struct lowerer *l)
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
        if (g->param_count == 1 && g->params[0].type == IR_PTR &&
            g->result == IR_VOID) {
            return g;
        }
        count++;
    }
    snprintf(name, sizeof name, "fn.%u", count);
    f = ir_declare_add(l->m, l->module_name, name, IR_VOID, IR_NO_AGG);
    ir_param_add(f, IR_PTR, IR_NO_AGG);
    return f;
}

/* The signature of a call through a bound function, which takes the
   object as its first parameter and then the declared ones. */
static const struct ir_function *bound_signature(struct lowerer *l,
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
        if (g->param_count == t->param_count + 1 &&
            g->params[0].type == IR_PTR &&
            g->result == ir_type_of(t->result)) {
            size_t k;
            for (k = 0; k < t->param_count; k++) {
                if (g->params[k + 1].type != ir_type_of(t->params[k])) {
                    break;
                }
            }
            if (k == t->param_count) {
                return g;
            }
        }
        count++;
    }
    snprintf(name, sizeof name, "fn.%u", count);
    f = ir_declare_add(l->m, l->module_name, name, ir_type_of(t->result),
                       result_agg(l, t->result));
    ir_param_add(f, IR_PTR, IR_NO_AGG);
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

/* DESIGN: the class of the chain that declares the field `name`. A class
   carries its base as field 0, so every class of the chain starts at the
   same address as the object. The address of an inherited field is
   therefore the object plus the field's offset in the class that
   declares it, with no step per level. */
static const struct type *field_owner(const struct type *t,
                                      const struct name *name)
{
    const struct type *s;

    for (s = t; s != NULL; s = s->kind == TYPE_CLASS ? s->base : NULL) {
        if (field_of(s, name) != NULL) {
            return s;
        }
    }
    return t;
}

static bool name_is(const struct name *name, const char *text)
{
    return name->length == strlen(text) &&
           memcmp(name->text, text, name->length) == 0;
}

static const struct name len_name = {"len", 3};
static const struct name entry_name = {"entry", 5};

/* The offset of a field as an operand. C places the first field and every
   field of a union at offset 0 on every target. Only a later field of a
   struct needs a symbolic offset. The ptr of a str or slice is its first
   field and len its second. A bound function holds its object first and
   its entry second. */
static struct ir_operand field_offset(struct lowerer *l, const struct type *s,
                                      const struct name *name)
{
    uint32_t index = type_has_fields(s) ? (uint32_t)(field_of(s, name) -
                                                     s->fields)
                     : name_is(name, "len") || name_is(name, "entry") ? 1
                                                                      : 0;

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
        if (l->m->globals[i]->module != NULL &&
            strcmp(l->m->globals[i]->module, l->module_name) == 0) {
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
        if (g->module == NULL ||
            strcmp(g->module, l->module_name) != 0 || g->bytes == NULL) {
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

static void const_tree(struct lowerer *l, const struct const_value *v,
                       const struct type *t, struct ir_const *out);

/* DESIGN: a static field is a global of the module that declares its
   class, named `Class.field`, with the declared value written into it.
   Every mention of the field reaches the same global. */
static struct ir_global *static_global(struct lowerer *l,
                                       const struct symbol *sym)
{
    struct ir_module *m = l->m;
    struct ir_const *value;
    char *name = cstr(&sym->name);
    struct ir_global *g = NULL;
    size_t i;

    for (i = 0; i < m->global_count; i++) {
        if (m->globals[i]->module != NULL &&
            strcmp(m->globals[i]->module, l->module_name) == 0 &&
            strcmp(m->globals[i]->name, name) == 0) {
            free(name);
            return m->globals[i];
        }
    }
    value = arena_alloc(m->arena, sizeof *value);
    const_tree(l, sym->value, sym->type, value);
    g = ir_global_add_value(m, l->module_name, name, value);
    g->mutable = true;
    free(name);
    return g;
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
    case EXPR_FIELD:
        /* A static field of a class is a global of the module. */
        if (sym != NULL && sym->kind == SYMBOL_GLOBAL) {
            p->address = temp(l, ir_addr(l->f, l->b,
                                         ir_global_op(static_global(l, sym))));
            return !l->failed;
        }
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
    default:
        /* DESIGN: a value that is no place still has an address once it
           is written somewhere. A literal receiver of an operator is the
           case, and lower_address gives it a slot of its own. */
        p->address = lower_address(l, e);
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
    } else if (type_has_fields(t)) {
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
        if (g->module != NULL && strcmp(g->module, l->module_name) == 0 &&
            ir_const_equal(g->value, value)) {
            return temp(l, ir_addr(l->f, l->b, ir_global_op(g)));
        }
    }
    snprintf(name, sizeof name, "%u", globals_of_module(l));
    return temp(l, ir_addr(l->f, l->b, ir_global_op(
                    ir_global_add_value(m, l->module_name, name, value))));
}

/* The aggregate of a table of n entries: an array of n pointers. Every
   class of one length shares it, as any two equal array types do. */
static uint32_t table_agg(struct lowerer *l, size_t n)
{
    char name[32];

    snprintf(name, sizeof name, "[%zu]ptr", n);
    if (ir_agg_find(l->m, name) != IR_NO_AGG) {
        return ir_agg_find(l->m, name);
    }
    return ir_array_add(l->m, name, ir_scalar(IR_PTR),
                        ir_sym_int(l->m, IR_I64, (uint64_t)n), NULL);
}

/* The class behind a value of type T or *T, or NULL. */
static const struct type *struct_of_expr(const struct expr *e)
{
    const struct type *t = e->type;

    if (t == NULL) {
        return NULL;
    }
    if (t->kind == TYPE_POINTER) {
        t = t->element;
    }
    return t->kind == TYPE_CLASS ? t : NULL;
}

/* Whether a bound function names one body. A `final` function and a
   `final` class have no class below them to replace it. */
static bool bound_is_direct(const struct expr *e, const struct type *s)
{
    const struct item *m = e->symbol != NULL ? e->symbol->item : NULL;

    return s == NULL || s->is_final || (m != NULL && m->is_final);
}

/* DESIGN: the table of a class holds the class descriptor at entry 0 and
   one entry per public function of its chain. The entries of the base
   come first, in declaration order. The class's own entries follow. One
   name therefore keeps one index in every class of a chain. A call
   through a base pointer reads the entry the derived class filled. A
   `concrete fn` takes the entry of the function it replaces. An
   `abstract fn` leaves its entry null until a class fills it. */

/* One entry of a table. It holds the name a call names and the function
   of the class that fills it. A function of the root has no source, so
   it holds the runtime symbol instead. */
struct entry {
    struct name name;
    const struct item *fn;
    const char *runtime;
};

struct table {
    struct entry *entries;
    size_t count;
    size_t capacity;
};

/* DESIGN: anti.rt.Object declares seven public functions whose bodies
   live in the runtime. Every class inherits them, so they take the first
   entries of every table. A class replaces one with a concrete function
   of the same name. */
static const char *const root_names[] = {
    "type_name", "to_text", "equals", "hash", "serialize", "destruct", "copy"
};

static bool same_name(const struct name *a, const struct name *b)
{
    return a->length == b->length &&
           memcmp(a->text, b->text, a->length) == 0;
}

static void table_add(struct table *t, struct name name,
                      const struct item *m, const char *runtime)
{
    size_t i;

    for (i = 0; i < t->count; i++) {
        if (same_name(&t->entries[i].name, &name)) {
            t->entries[i].fn = m;
            return;
        }
    }
    if (t->count == t->capacity) {
        size_t capacity = t->capacity == 0 ? 8 : t->capacity * 2;
        struct entry *entries =
            realloc(t->entries, capacity * sizeof *entries);
        if (entries == NULL) {
            fputs("antic: out of memory\n", stderr);
            exit(70);
        }
        t->entries = entries;
        t->capacity = capacity;
    }
    t->entries[t->count].name = name;
    t->entries[t->count].fn = m;
    t->entries[t->count].runtime = runtime;
    t->count++;
}

static void table_of(const struct type *t, struct table *out)
{
    size_t i;

    if (t == NULL) {
        return;
    }
    if (t->kind == TYPE_CLASS) {
        table_of(t->base, out);
    }
    if (t->kind == TYPE_CLASS && t->base == NULL) {
        for (i = 0; i < sizeof root_names / sizeof root_names[0]; i++) {
            struct name name;
            name.text = root_names[i];
            name.length = strlen(root_names[i]);
            table_add(out, name, NULL, root_names[i]);
        }
    }
    for (i = 0; i < t->member_count; i++) {
        const struct item *m = t->members[i];
        /* DESIGN: `concrete fn I::f` fills the table of I alone. The
           primary table of the class keeps whatever it had, which is the
           inherited entry or an unqualified body. The class's own name
           as a qualifier means the primary table. */
        if (m->kind == ITEM_FN && m->pub &&
            (m->qualifier.length == 0 || same_name(&m->qualifier, &t->name))) {
            table_add(out, m->name, m, NULL);
        }
    }
}

/* The member of t that fills the entry `name` of the table of iface: a
   body qualified by that interface first, then an unqualified one. */
static const struct item *qualified_member(const struct type *t,
                                           const struct type *iface,
                                           const struct name *name)
{
    const struct item *plain = NULL;
    const struct type *up;
    size_t i;

    for (up = t; up != NULL; up = up->kind == TYPE_CLASS ? up->base : NULL) {
        for (i = 0; i < up->member_count; i++) {
            const struct item *m = up->members[i];
            const struct type *chain;
            if (m->kind != ITEM_FN || !m->pub || !same_name(&m->name, name)) {
                continue;
            }
            if (m->qualifier.length == 0 && plain == NULL) {
                plain = m;
                continue;
            }
            for (chain = iface; chain != NULL;
                 chain = chain->kind == TYPE_CLASS ? chain->base : NULL) {
                if (same_name(&m->qualifier, &chain->name)) {
                    return m;
                }
            }
        }
    }
    return plain;
}

/* The index of the entry that holds the function `name`, or 0 when the
   class has no such entry. Entry 0 is the descriptor, so a real entry is
   never 0. */
static int table_index(const struct type *t, const struct name *name)
{
    struct table table = {0};
    size_t i;
    int found = 0;

    table_of(t, &table);
    for (i = 0; i < table.count; i++) {
        if (same_name(&table.entries[i].name, name)) {
            found = (int)i + 1;
            break;
        }
    }
    free(table.entries);
    return found;
}

/* The offset of table entry index. The IR holds no sizes, so the width
   of a pointer stays symbolic. */
static struct ir_operand entry_offset(struct lowerer *l, int index)
{
    return temp(l, ir_binary(l->f, l->b, IR_MUL, IR_I64,
                             ir_int_op(IR_I64, (uint64_t)index),
                             ir_sym_operand(l->m,
                                 ir_sym_size_of(l->m, ir_scalar(IR_PTR)))));
}

/* The aggregate of one field record of a descriptor. */
/* DESIGN: the function record of a descriptor names one public function
   of the class and where its entry sits. A program reads the list and
   calls through the table, so reflection needs no name of its own. */
static uint32_t function_agg(struct lowerer *l)
{
    static const char name[] = "anti.rt.Function";
    struct ir_field fields[4];
    uint32_t agg = ir_agg_find(l->m, name);

    if (agg != IR_NO_AGG) {
        return agg;
    }
    memset(fields, 0, sizeof fields);
    fields[0].name = "name";
    fields[0].type = ir_scalar(IR_PTR);
    fields[1].name = "name_length";
    fields[1].type = ir_scalar(IR_I64);
    fields[2].name = "slot";
    fields[2].type = ir_scalar(IR_I64);
    fields[3].name = "param_count";
    fields[3].type = ir_scalar(IR_I64);
    return ir_struct_add(l->m, IR_AGG_STRUCT, name, fields, 4, false, 0);
}

/* The aggregate of a function list of n records. */
static uint32_t functions_agg(struct lowerer *l, size_t n)
{
    char name[48];

    snprintf(name, sizeof name, "[%zu]anti.rt.Function", n);
    if (ir_agg_find(l->m, name) != IR_NO_AGG) {
        return ir_agg_find(l->m, name);
    }
    return ir_array_add(l->m, name, ir_aggregate(function_agg(l)),
                        ir_sym_int(l->m, IR_I64, (uint64_t)n), NULL);
}

static uint32_t field_agg(struct lowerer *l)
{
    static const char name[] = "anti.rt.Field";
    struct ir_field fields[6];
    uint32_t agg = ir_agg_find(l->m, name);

    if (agg != IR_NO_AGG) {
        return agg;
    }
    memset(fields, 0, sizeof fields);
    fields[0].name = "name";
    fields[0].type = ir_scalar(IR_PTR);
    fields[1].name = "name_length";
    fields[1].type = ir_scalar(IR_I64);
    fields[2].name = "offset";
    fields[2].type = ir_scalar(IR_I64);
    fields[3].name = "kind";
    fields[3].type = ir_scalar(IR_I64);
    fields[4].name = "owned";
    fields[4].type = ir_scalar(IR_I64);
    fields[5].name = "descriptor";
    fields[5].type = ir_scalar(IR_PTR);
    return ir_struct_add(l->m, IR_AGG_STRUCT, name, fields, 6, false, 0);
}

/* The aggregate of an array of n field records. */
static uint32_t fields_agg(struct lowerer *l, size_t n)
{
    char name[40];

    snprintf(name, sizeof name, "[%zu]anti.rt.Field", n);
    if (ir_agg_find(l->m, name) != IR_NO_AGG) {
        return ir_agg_find(l->m, name);
    }
    return ir_array_add(l->m, name, ir_aggregate(field_agg(l)),
                        ir_sym_int(l->m, IR_I64, (uint64_t)n), NULL);
}

/* The aggregate of a class descriptor. Every class shares it. */
static uint32_t descriptor_agg(struct lowerer *l)
{
    static const char name[] = "anti.rt.Descriptor";
    struct ir_field fields[12];
    uint32_t agg = ir_agg_find(l->m, name);

    if (agg != IR_NO_AGG) {
        return agg;
    }
    memset(fields, 0, sizeof fields);
    fields[0].name = "name";
    fields[0].type = ir_scalar(IR_PTR);
    fields[1].name = "name_length";
    fields[1].type = ir_scalar(IR_I64);
    fields[2].name = "parent";
    fields[2].type = ir_scalar(IR_PTR);
    fields[3].name = "size";
    fields[3].type = ir_scalar(IR_I64);
    fields[4].name = "depth";
    fields[4].type = ir_scalar(IR_I64);
    fields[5].name = "ancestors";
    fields[5].type = ir_scalar(IR_PTR);
    fields[6].name = "field_count";
    fields[6].type = ir_scalar(IR_I64);
    fields[7].name = "fields";
    fields[7].type = ir_scalar(IR_PTR);
    fields[8].name = "destruct";
    fields[8].type = ir_scalar(IR_PTR);
    /* DESIGN: the descriptor of an interface table records how far the
       sub-object sits from the start of the object. A pointer to the
       sub-object therefore leads back to the object itself, which is
       what `is`, `as`, `delete` and identity need. The descriptor of a
       class has zero there. */
    fields[9].name = "offset";
    fields[9].type = ir_scalar(IR_I64);
    fields[10].name = "function_count";
    fields[10].type = ir_scalar(IR_I64);
    fields[11].name = "functions";
    fields[11].type = ir_scalar(IR_PTR);
    return ir_struct_add(l->m, IR_AGG_STRUCT, name, fields, 12, false, 0);
}

/* The depth of a class in its chain. The root anti.rt.Object is 0. */
static uint32_t class_depth(const struct type *t)
{
    uint32_t depth = 0;

    for (; t != NULL && t->kind == TYPE_CLASS && t->base != NULL;
         t = t->base) {
        depth++;
    }
    return depth;
}

/* A global of the module named `<class>.<suffix>`, or NULL when the
   module has none. Every class builds its data once. */
static struct ir_global *class_global(struct lowerer *l, const struct type *t,
                                      const char *suffix, char **module_out,
                                      char **name_out)
{
    struct ir_module *m = l->m;
    char *module = cstr(&t->module);
    char *name;
    size_t size;
    size_t i;

    /* DESIGN: an export class gives its table and its descriptor the C
       names the generated header declares, so C code reads them. Every
       other class keeps the name `Class.part` of its module. */
    size = t->name.length + strlen(suffix) + 8;
    name = malloc(size);
    if (name == NULL) {
        fputs("antic: out of memory\n", stderr);
        exit(70);
    }
    if (t->item_exported && (strcmp(suffix, "table") == 0 ||
                             strcmp(suffix, "descriptor") == 0)) {
        snprintf(name, size, "anti_%.*s_%s", (int)t->name.length,
                 t->name.text,
                 strcmp(suffix, "table") == 0 ? "vtable" : "descriptor");
    } else {
        memcpy(name, t->name.text, t->name.length);
        name[t->name.length] = '.';
        memcpy(name + t->name.length + 1, suffix, strlen(suffix) + 1);
    }
    for (i = 0; i < m->global_count; i++) {
        if (m->globals[i]->module != NULL &&
            strcmp(m->globals[i]->module, module) == 0 &&
            strcmp(m->globals[i]->name, name) == 0) {
            free(module);
            free(name);
            return m->globals[i];
        }
    }
    /* DESIGN: a class's table and descriptor belong to the module that
       declares it, which writes them whether it builds one or not. A
       module that names a class of another refers to them, so a program
       holds one descriptor per class and `is` compares one address. */
    if (l->module_name != NULL && strcmp(module, l->module_name) != 0) {
        struct ir_global *g = ir_global_add(m, module, name, NULL, 0, 1);
        g->is_extern = true;
        g->exported = t->item_exported;
        free(module);
        free(name);
        return g;
    }
    *module_out = module;
    *name_out = name;
    return NULL;
}

static struct ir_global *class_descriptor(struct lowerer *l,
                                          const struct type *t);

/* The count of fields a descriptor lists: the class's own fields, with
   the base and the table pointer left out. Each class lists its own, and
   the parent descriptor holds the rest of the chain. */
static size_t own_fields(const struct type *t)
{
    size_t count = 0;
    size_t i;

    for (i = 0; i < t->field_count; i++) {
        if (t->fields[i].form != FIELD_BASE &&
            t->fields[i].form != FIELD_TABLE) {
            count++;
        }
    }
    return count;
}

/* DESIGN: the field list of a class names every field it declares, with
   the offset, the kind and the `own` bit of each. A field of class or
   struct type carries its descriptor as well, so a walk of the list
   reaches the whole object. `--no-reflect` drops the list and keeps the
   rest of the descriptor. */
static struct ir_global *class_fields(struct lowerer *l,
                                      const struct type *t)
{
    size_t count = own_fields(t);
    struct ir_const *value;
    struct ir_global *g;
    struct token_text text;
    char *module;
    char *name;
    size_t i;
    size_t n = 0;

    g = class_global(l, t, "fields", &module, &name);
    if (g != NULL) {
        return g;
    }
    value = ir_const_agg(l->m, ir_aggregate(fields_agg(l, count)), count);
    for (i = 0; i < t->field_count; i++) {
        const struct struct_field *f = &t->fields[i];
        struct ir_const *item;
        if (f->form == FIELD_BASE || f->form == FIELD_TABLE) {
            continue;
        }
        item = ir_const_agg(l->m, ir_aggregate(field_agg(l)), 6);
        text.bytes = f->name.text;
        text.length = f->name.length;
        item->items[0].kind = IR_CONST_ADDR;
        item->items[0].scalar = IR_PTR;
        item->items[0].global = literal_global(l, &text)->index;
        item->items[1].kind = IR_CONST_INT;
        item->items[1].scalar = IR_I64;
        item->items[1].integer = f->name.length;
        item->items[2].kind = IR_CONST_SYM;
        item->items[2].scalar = IR_I64;
        item->items[2].sym = ir_sym_offset_of(l->m, agg_of(l, t), (uint32_t)i);
        item->items[3].kind = IR_CONST_INT;
        item->items[3].scalar = IR_I64;
        item->items[3].integer = is_aggregate(f->type) ? IR_AGG
                                                       : ir_type_of(f->type);
        item->items[4].kind = IR_CONST_INT;
        item->items[4].scalar = IR_I64;
        item->items[4].integer = f->owned ? 1 : 0;
        /* A field of class type, and a pointer to one, both carry the
           descriptor of that class, so a deep copy knows its size. */
        item->items[5].scalar = IR_PTR;
        if (f->type->kind == TYPE_CLASS) {
            item->items[5].kind = IR_CONST_ADDR;
            item->items[5].global = class_descriptor(l, f->type)->index;
        } else if (f->type->kind == TYPE_POINTER &&
                   f->type->element->kind == TYPE_CLASS) {
            item->items[5].kind = IR_CONST_ADDR;
            item->items[5].global =
                class_descriptor(l, f->type->element)->index;
        } else {
            item->items[5].kind = IR_CONST_INT;
            item->items[5].integer = 0;
        }
        value->items[n++] = *item;
    }
    g = ir_global_add_value(l->m, module, name, value);
    free(module);
    free(name);
    return g;
}

/* DESIGN: the ancestors of a class are its descriptors from the root
   down to the class itself, one entry per level. The entry at a level is
   the same address in every class below it, so `p is *T` compares the
   entry at T's depth with T's descriptor. */
/* A global the runtime defines. Every module refers to it and none
   writes it out. */
static struct ir_global *runtime_global(struct lowerer *l, const char *name)
{
    struct ir_module *m = l->m;
    struct ir_global *g;
    size_t i;

    for (i = 0; i < m->global_count; i++) {
        if (m->globals[i]->module == NULL &&
            strcmp(m->globals[i]->name, name) == 0) {
            return m->globals[i];
        }
    }
    g = ir_global_add(m, NULL, name, NULL, 0, 1);
    g->is_extern = true;
    g->exported = true;
    return g;
}

static struct ir_global *class_ancestors(struct lowerer *l,
                                         const struct type *t)
{
    uint32_t depth = class_depth(t);
    struct ir_const *value;
    struct ir_global *g;
    const struct type *up;
    char *module;
    char *name;
    uint32_t i;

    if (t->base == NULL) {
        return runtime_global(l, "anti_rt_Object_ancestors");
    }
    g = class_global(l, t, "ancestors", &module, &name);
    if (g != NULL) {
        return g;
    }
    value = ir_const_agg(l->m, ir_aggregate(table_agg(l, depth + 1)),
                         depth + 1);
    up = t;
    for (i = depth + 1; i-- > 0; up = up->base) {
        value->items[i].kind = IR_CONST_ADDR;
        value->items[i].scalar = IR_PTR;
        value->items[i].global = class_descriptor(l, up)->index;
    }
    g = ir_global_add_value(l->m, module, name, value);
    free(module);
    free(name);
    return g;
}

/* The list of public functions of the chain, in table order. */
static struct ir_global *class_functions(struct lowerer *l,
                                         const struct type *t,
                                         size_t *count_out)
{
    struct table table = {0};
    struct ir_const *value;
    struct ir_global *g;
    struct token_text text;
    char *module;
    char *name;
    size_t i;

    table_of(t, &table);
    *count_out = table.count;
    if (table.count == 0) {
        free(table.entries);
        return NULL;
    }
    g = class_global(l, t, "functions", &module, &name);
    if (g != NULL) {
        free(table.entries);
        return g;
    }
    value = ir_const_agg(l->m, ir_aggregate(functions_agg(l, table.count)),
                         table.count);
    for (i = 0; i < table.count; i++) {
        struct ir_const *item =
            ir_const_agg(l->m, ir_aggregate(function_agg(l)), 4);
        const struct item *fn = table.entries[i].fn;
        text.bytes = table.entries[i].name.text;
        text.length = table.entries[i].name.length;
        item->items[0].kind = IR_CONST_ADDR;
        item->items[0].scalar = IR_PTR;
        item->items[0].global = literal_global(l, &text)->index;
        item->items[1].kind = IR_CONST_INT;
        item->items[1].scalar = IR_I64;
        item->items[1].integer = table.entries[i].name.length;
        item->items[2].kind = IR_CONST_INT;
        item->items[2].scalar = IR_I64;
        item->items[2].integer = i + 1;
        item->items[3].kind = IR_CONST_INT;
        item->items[3].scalar = IR_I64;
        item->items[3].integer =
            fn != NULL && fn->symbol != NULL &&
                    fn->symbol->type->kind == TYPE_FN
                ? fn->symbol->type->param_count
                : 1;
        value->items[i] = *item;
    }
    free(table.entries);
    g = ir_global_add_value(l->m, module, name, value);
    free(module);
    free(name);
    return g;
}

/* DESIGN: the descriptor of a class is read-only data at entry 0 of its
   table. It names the class and points at the descriptor of its base. It
   holds the size of the class and its depth in the chain, and points at
   its ancestors. */
static struct ir_global *class_descriptor(struct lowerer *l,
                                          const struct type *t)
{
    struct ir_const *value;
    struct ir_global *g;
    struct token_text text;
    char *module;
    char *name;

    /* DESIGN: the root's descriptor belongs to the runtime, so that two
       modules of one program share one and `p is *Object` compares the
       same address everywhere. */
    if (t->base == NULL) {
        return runtime_global(l, "anti_rt_Object_descriptor");
    }
    g = class_global(l, t, "descriptor", &module, &name);
    if (g != NULL) {
        return g;
    }
    /* The global is added before its ancestors, so a chain that comes
       back around finds it and does not build it twice. */
    value = ir_const_agg(l->m, ir_aggregate(descriptor_agg(l)), 12);
    g = ir_global_add_value(l->m, module, name, value);
    g->exported = t->item_exported;
    free(module);
    free(name);
    text.bytes = t->name.text;
    text.length = t->name.length;
    value->items[0].kind = IR_CONST_ADDR;
    value->items[0].scalar = IR_PTR;
    value->items[0].global = literal_global(l, &text)->index;
    value->items[1].kind = IR_CONST_INT;
    value->items[1].scalar = IR_I64;
    value->items[1].integer = t->name.length;
    value->items[2].scalar = IR_PTR;
    if (t->base != NULL) {
        value->items[2].kind = IR_CONST_ADDR;
        value->items[2].global = class_descriptor(l, t->base)->index;
    } else {
        value->items[2].kind = IR_CONST_INT;
        value->items[2].integer = 0;
    }
    value->items[3].kind = IR_CONST_SYM;
    value->items[3].scalar = IR_I64;
    value->items[3].sym = ir_sym_size_of(l->m, vtype_of(l, t));
    value->items[4].kind = IR_CONST_INT;
    value->items[4].scalar = IR_I64;
    value->items[4].integer = class_depth(t);
    value->items[5].kind = IR_CONST_ADDR;
    value->items[5].scalar = IR_PTR;
    value->items[5].global = class_ancestors(l, t)->index;
    value->items[6].kind = IR_CONST_INT;
    value->items[6].scalar = IR_I64;
    value->items[6].integer = l->no_reflect ? 0 : own_fields(t);
    value->items[7].scalar = IR_PTR;
    if (l->no_reflect || own_fields(t) == 0) {
        value->items[7].kind = IR_CONST_INT;
        value->items[7].integer = 0;
    } else {
        value->items[7].kind = IR_CONST_ADDR;
        value->items[7].global = class_fields(l, t)->index;
    }
    /* DESIGN: the destruct body the class declares, and not the one it
       inherits, because `delete` runs one body per level of the chain.
       The table holds the last one, which is a different question. */
    value->items[8].scalar = IR_PTR;
    value->items[8].kind = IR_CONST_INT;
    value->items[8].integer = 0;
    {
        static const struct name destruct_name = {"destruct", 8};
        size_t i;
        for (i = 0; i < t->member_count; i++) {
            const struct item *m = t->members[i];
            if (m->kind == ITEM_FN && same_name(&m->name, &destruct_name) &&
                m->body != NULL && m->symbol != NULL) {
                value->items[8].kind = IR_CONST_FUNC;
                value->items[8].global = m->symbol->ir;
            }
        }
    }
    value->items[9].kind = IR_CONST_INT;
    value->items[9].scalar = IR_I64;
    value->items[9].integer = 0;
    /* The function list names every public function of the chain and
       the entry of each. `--no-reflect` drops it with the field list. */
    {
        size_t function_count = 0;
        const struct ir_global *list =
            l->no_reflect ? NULL : class_functions(l, t, &function_count);
        value->items[10].kind = IR_CONST_INT;
        value->items[10].scalar = IR_I64;
        value->items[10].integer = list != NULL ? function_count : 0;
        value->items[11].scalar = IR_PTR;
        if (list != NULL) {
            value->items[11].kind = IR_CONST_ADDR;
            value->items[11].global = list->index;
        } else {
            value->items[11].kind = IR_CONST_INT;
            value->items[11].integer = 0;
        }
    }
    return g;
}

/* The descriptor that entry 0 of an interface table points at. It is
   the class's own descriptor with the offset of the sub-object. A
   pointer into the middle of an object therefore still names the class
   and still leads back to its start. */
static struct ir_global *interface_descriptor(struct lowerer *l,
                                              const struct type *t,
                                              const struct struct_field *sub)
{
    const struct ir_global *of = class_descriptor(l, t);
    struct ir_const *value;
    struct ir_global *g;
    char label[160];
    char *module;
    char *name;

    snprintf(label, sizeof label, "%.*s.descriptor", (int)sub->name.length,
             sub->name.text);
    g = class_global(l, t, label, &module, &name);
    if (g != NULL) {
        return g;
    }
    value = ir_const_agg(l->m, ir_aggregate(descriptor_agg(l)), 12);
    memcpy(value->items, l->m->globals[of->index]->value->items,
           12 * sizeof *value->items);
    value->items[9].kind = IR_CONST_SYM;
    value->items[9].scalar = IR_I64;
    value->items[9].sym =
        ir_sym_offset_of(l->m, agg_of(l, sub->home),
                         (uint32_t)(sub - sub->home->fields));
    g = ir_global_add_value(l->m, module, name, value);
    g->exported = t->item_exported;
    free(module);
    free(name);
    return g;
}

static struct ir_function *rt_function(struct lowerer *l, const char *name,
                                       const enum ir_type *params,
                                       size_t count);

/* The global that holds the table of t, built once per class. */
static struct ir_global *class_table(struct lowerer *l, const struct type *t)
{
    struct ir_module *m = l->m;
    struct table table = {0};
    struct ir_const *value;
    struct ir_global *g;
    char *module;
    char *name;
    size_t i;

    g = class_global(l, t, "table", &module, &name);
    if (g != NULL) {
        return g;
    }
    table_of(t, &table);
    value = ir_const_agg(m, ir_aggregate(table_agg(l, table.count + 1)),
                         table.count + 1);
    g = ir_global_add_value(m, module, name, value);
    g->exported = t->item_exported;
    value->items[0].kind = IR_CONST_ADDR;
    value->items[0].scalar = IR_PTR;
    value->items[0].global = class_descriptor(l, t)->index;
    for (i = 0; i < table.count; i++) {
        const struct item *fn = table.entries[i].fn;
        value->items[i + 1].scalar = IR_PTR;
        if (fn != NULL && fn->symbol != NULL &&
            (fn->body != NULL || fn->runtime != NULL)) {
            value->items[i + 1].kind = IR_CONST_FUNC;
            value->items[i + 1].global =
                callee_function(l, fn->symbol)->index;
        } else {
            value->items[i + 1].kind = IR_CONST_INT;
            value->items[i + 1].integer = 0;
        }
    }
    free(table.entries);
    free(module);
    free(name);
    return g;
}

/* The public function `name` of t or of a class above it. */
static const struct item *find_member_fn(const struct type *t,
                                         const struct name *name)
{
    size_t i;

    for (; t != NULL; t = t->kind == TYPE_CLASS ? t->base : NULL) {
        for (i = 0; i < t->member_count; i++) {
            const struct item *m = t->members[i];
            if (m->kind == ITEM_FN && m->pub && same_name(&m->name, name)) {
                return m;
            }
        }
    }
    return NULL;
}

/* DESIGN: an interface table holds thunks. A thunk takes `self` as a
   pointer to the sub-object. It subtracts the offset of that sub-object
   to reach the object, then calls the class's own function. Every
   function is written once. It serves the direct call and the call
   through the interface alike. */
static struct ir_function *interface_thunk(struct lowerer *l,
                                           const struct type *t,
                                           const struct struct_field *sub,
                                           const struct item *fn)
{
    const struct type *sig = fn->symbol->type;
    struct ir_function *outer_f = l->f;
    struct ir_block *outer_b = l->b;
    struct ir_function *target = callee_function(l, fn->symbol);
    struct ir_function *f;
    struct ir_operand *args;
    struct ir_block *entry;
    struct ir_operand back;
    uint32_t value;
    char name[192];
    size_t i;

    snprintf(name, sizeof name, "%.*s.%.*s.%.*s.thunk", (int)t->name.length,
             t->name.text, (int)sub->name.length, sub->name.text,
             (int)fn->name.length, fn->name.text);
    f = find_function(l->m, l->module_name, name);
    if (f != NULL) {
        return f;
    }
    f = ir_function_add(l->m, l->module_name, name, ir_type_of(sig->result),
                        result_agg(l, sig->result));
    f->result_agg = result_agg(l, sig->result);
    for (i = 0; i < sig->param_count; i++) {
        add_param(l, f, sig->params[i]);
    }
    entry = ir_block_add(f);
    l->f = f;
    l->b = entry;
    args = malloc((sig->param_count + 1) * sizeof *args);
    if (args == NULL) {
        fputs("antic: out of memory\n", stderr);
        exit(70);
    }
    back = temp(l, ir_binary(f, entry, IR_SUB, IR_I64, ir_int_op(IR_I64, 0),
                             field_offset(l, sub->home, &sub->name)));
    args[0] = temp(l, ir_ptradd(f, entry, temp(l, f->params[0].temp), back));
    for (i = 1; i < sig->param_count; i++) {
        args[i] = temp(l, f->params[i].temp);
    }
    value = ir_call(f, entry, ir_type_of(sig->result),
                    ir_func_op(target), args, sig->param_count);
    free(args);
    if (sig->result->kind == TYPE_VOID) {
        ir_ret(f, entry, IR_VOID, none());
    } else {
        /* An aggregate result travels as the address of its storage,
           which is what the called function already returned. */
        ir_ret(f, entry, f->result == IR_AGG ? IR_PTR : f->result,
               temp(l, value));
    }
    l->f = outer_f;
    l->b = outer_b;
    return f;
}

/* The table of one interface sub-object of t, whose entries are thunks
   into t's own functions. */
static struct ir_global *interface_table(struct lowerer *l,
                                         const struct type *t,
                                         const struct struct_field *sub)
{
    struct ir_module *m = l->m;
    struct table table = {0};
    struct ir_const *value;
    struct ir_global *g;
    char *module;
    char *name;
    char label[160];
    size_t i;

    snprintf(label, sizeof label, "%.*s.table", (int)sub->name.length,
             sub->name.text);
    g = class_global(l, t, label, &module, &name);
    if (g != NULL) {
        return g;
    }
    table_of(sub->type, &table);
    value = ir_const_agg(m, ir_aggregate(table_agg(l, table.count + 1)),
                         table.count + 1);
    g = ir_global_add_value(m, module, name, value);
    g->exported = t->item_exported;
    value->items[0].kind = IR_CONST_ADDR;
    value->items[0].scalar = IR_PTR;
    value->items[0].global = interface_descriptor(l, t, sub)->index;
    for (i = 0; i < table.count; i++) {
        /* The class fills the entry where it declares or inherits the
           name, and the thunk moves the receiver back to the object.
           Where it does not, the interface's own body serves, and that
           body already takes a pointer to the sub-object. */
        const struct item *fn =
            qualified_member(t, sub->type, &table.entries[i].name);
        bool own = fn != NULL && fn->symbol != NULL &&
                   (fn->body != NULL || fn->runtime != NULL);
        if (!own) {
            fn = find_member_fn(sub->type, &table.entries[i].name);
            own = false;
        }
        value->items[i + 1].scalar = IR_PTR;
        if (fn != NULL && fn->symbol != NULL &&
            (fn->body != NULL || fn->runtime != NULL)) {
            value->items[i + 1].kind = IR_CONST_FUNC;
            value->items[i + 1].global =
                own ? interface_thunk(l, t, sub, fn)->index
                    : callee_function(l, fn->symbol)->index;
        } else {
            value->items[i + 1].kind = IR_CONST_INT;
            value->items[i + 1].integer = 0;
        }
    }
    free(table.entries);
    free(module);
    free(name);
    return g;
}

/* DESIGN: `construct` runs after a literal has written every field,
   base first down the chain. A base therefore sees its own fields
   before the class below it adds to them. A class without one adds
   nothing. */
static void run_construct(struct lowerer *l, const struct type *t,
                          struct ir_operand dest)
{
    static const struct name construct_name = {"construct", 9};
    size_t i;

    if (t == NULL || t->kind != TYPE_CLASS) {
        return;
    }
    run_construct(l, t->base, dest);
    for (i = 0; i < t->member_count && !l->failed; i++) {
        const struct item *m = t->members[i];
        if (m->kind != ITEM_FN || !same_name(&m->name, &construct_name) ||
            m->body == NULL || m->symbol == NULL || m->param_count != 0) {
            continue;
        }
        ir_call(l->f, l->b, IR_VOID,
                ir_func_op(callee_function(l, m->symbol)), &dest, 1);
    }
}

/* Store the table pointer of every interface sub-object of t into the
   object at dest. */
static void store_interface_tables(struct lowerer *l, const struct type *t,
                                   struct ir_operand dest)
{
    const struct type *up;
    size_t i;

    for (up = t; up != NULL; up = up->kind == TYPE_CLASS ? up->base : NULL) {
        for (i = 0; i < up->field_count; i++) {
            const struct struct_field *field = &up->fields[i];
            if (field->form != FIELD_IMPL) {
                continue;
            }
            ir_store(l->f, l->b, IR_PTR,
                     temp(l, ir_addr(l->f, l->b,
                                     ir_global_op(interface_table(l, t,
                                                                  field)))),
                     offset_address(l, dest,
                                    field_offset(l, up, &field->name)));
        }
    }
}

static void build_into(struct lowerer *l, const struct expr *e,
                       struct ir_operand dest);
static struct ir_operand lower_construct(struct lowerer *l,
                                         const struct expr *e,
                                         struct ir_operand dest);
static void handle_error(struct lowerer *l, const struct expr *call,
                         struct ir_operand err, struct ir_operand out,
                         bool has_out, struct ir_operand release);

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

/* The repeat form of an array literal computes its value once, then
   fills every element in a loop. */
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
    const struct type *owner;
    struct ir_operand src;
    size_t i;

    switch (e->kind) {
    /* `T(args)` and `alloc T(args)` build the object in place and run
       its `construct` with the arguments. */
    case EXPR_CALL:
        if (e->as.call.builds != NULL) {
            struct ir_operand err = lower_construct(l, e, dest);
            if (!l->failed && err.kind != IR_NONE) {
                handle_error(l, e, err, dest, !e->as.call.on_heap,
                             e->as.call.on_heap ? dest : none());
            }
            return;
        }
        /* Any other call gives an aggregate, which is copied. */
        src = lower_address(l, e);
        if (!l->failed) {
            ir_memcopy(l->f, l->b, dest, src, vtype_of(l, t));
        }
        break;
    case EXPR_STRUCT_LIT:
        /* The table pointer is the first word of every object, and the
           base of a class sits at offset 0, so it goes at dest. */
        if (t->kind == TYPE_CLASS) {
            ir_store(l->f, l->b, IR_PTR,
                     temp(l, ir_addr(l->f, l->b,
                                     ir_global_op(class_table(l, t)))),
                     dest);
            store_interface_tables(l, t, dest);
        }
        for (i = 0; i < e->as.struct_lit.field_count && !l->failed; i++) {
            const struct field_init *init = &e->as.struct_lit.fields[i];
            const struct type *at = field_owner(t, &init->name);
            const struct struct_field *field = field_of(at, &init->name);
            if (field->bits != 0) {
                struct ir_operand v = lower_expr(l, init->value);
                if (!l->failed) {
                    ir_bitstore(l->f, l->b, ir_type_of(field->type), v, dest,
                                agg_of(l, at),
                                (uint32_t)(field - at->fields));
                }
                continue;
            }
            store_value(l, field->type, init->value,
                        offset_address(l, dest,
                                       field_offset(l, at, &field->name)));
        }
        /* DESIGN: a field the literal leaves out has a default, which the
           checker required, and its expression is written here. The value
           is therefore complete however the literal was written. */
        for (owner = t; owner != NULL && !l->failed;
             owner = owner->kind == TYPE_CLASS ? owner->base : NULL) {
        for (i = 0; i < owner->field_count && !l->failed; i++) {
            const struct struct_field *field = &owner->fields[i];
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
                                agg_of(l, owner), (uint32_t)i);
                }
                continue;
            }
            store_value(l, field->type, (struct expr *)field->value,
                        offset_address(l, dest,
                                       field_offset(l, owner, &field->name)));
        }
        }
        run_construct(l, t, dest);
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
    case EXPR_FIELD:
        /* DESIGN: a bound function holds the object and the entry of its
           table, so a call needs no receiver and the object may move.
           A `final` function and a `final` class take the address of the
           body, because no class below replaces it. */
        if (t->kind == TYPE_FN && t->bound) {
            const struct type *s = struct_of_expr(e->as.field.base);
            int index = s != NULL ? table_index(s, &e->as.field.name) : 0;
            struct ir_operand object =
                e->as.field.base->type->kind == TYPE_POINTER
                    ? lower_expr(l, e->as.field.base)
                    : lower_address(l, e->as.field.base);
            struct ir_operand entry;
            if (l->failed) {
                break;
            }
            if (index > 0 && !bound_is_direct(e, s)) {
                struct ir_operand table =
                    temp(l, ir_load(l->f, l->b, IR_PTR, object));
                entry = temp(l, ir_load(l->f, l->b, IR_PTR,
                                        offset_address(l, table,
                                            entry_offset(l, index))));
            } else {
                entry = temp(l, ir_addr(l->f, l->b,
                                 ir_func_op(callee_function(l, e->symbol))));
            }
            ir_store(l->f, l->b, IR_PTR, object, dest);
            ir_store(l->f, l->b, IR_PTR, entry,
                     offset_address(l, dest,
                                    field_offset(l, t, &entry_name)));
            break;
        }
        src = lower_address(l, e);
        if (!l->failed) {
            ir_memcopy(l->f, l->b, dest, src, vtype_of(l, t));
        }
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
static struct ir_operand lower_dispatch(struct lowerer *l,
                                        const struct expr *e);
static struct ir_operand lower_join(struct lowerer *l, const struct expr *e);

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
    /* A static field is a global, and its name is its whole address. */
    if (sym != NULL && sym->kind == SYMBOL_GLOBAL) {
        return temp(l, ir_addr(l->f, l->b,
                               ir_global_op(static_global(l, sym))));
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
    case EXPR_DISPATCH:
        return lower_dispatch(l, e);
    case EXPR_JOIN:
        return lower_join(l, e);
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

static struct ir_function *rt_function(struct lowerer *l, const char *name,
                                       const enum ir_type *params,
                                       size_t count);

/* DESIGN: `==` on class pointers compares object identity. A pointer to
   an interface sub-object points into the middle of an object. Each
   side therefore moves back to the start of its object before the
   addresses are compared. A pointer to a concrete class is already there. Only a
   pointer whose static type is abstract can be a sub-object. The runtime
   does the move, because it also has to answer for a null pointer. */
static bool may_be_sub(const struct type *t)
{
    return t->kind == TYPE_POINTER && t->element->kind == TYPE_CLASS &&
           t->element->has_abstract;
}

static struct ir_operand object_of(struct lowerer *l, struct ir_operand p)
{
    static const enum ir_type params[] = {IR_PTR};

    return temp(l, ir_call(l->f, l->b, IR_PTR,
                           ir_func_op(rt_function(l, "anti_rt_object_of",
                                                  params, 1)),
                           &p, 1));
}

static struct ir_operand lower_binary(struct lowerer *l, const struct expr *e)
{
    enum token_kind op = e->as.binary.op;
    const struct type *operands = e->as.binary.left->type;
    struct ir_operand left;
    struct ir_operand right;
    bool identity = (op == TOKEN_EQ || op == TOKEN_NE) &&
                    may_be_sub(e->as.binary.left->type) &&
                    may_be_sub(e->as.binary.right->type);

    if (op == TOKEN_AND_AND || op == TOKEN_OR_OR) {
        return short_circuit(l, e);
    }
    left = lower_expr(l, e->as.binary.left);
    right = lower_expr(l, e->as.binary.right);
    if (l->failed) {
        return none();
    }
    if (identity) {
        left = object_of(l, left);
        right = object_of(l, right);
    }
    return temp(l, ir_binary(l->f, l->b, binary_op(op, operands),
                             is_comparison(op) ? IR_I8 : ir_type_of(operands),
                             left, right));
}

/* The field of a descriptor at index, read through the pointer d. */
static struct ir_operand descriptor_field(struct lowerer *l,
                                          struct ir_operand d, uint32_t index,
                                          enum ir_type type)
{
    struct ir_operand at =
        index == 0 ? d
                   : temp(l, ir_ptradd(l->f, l->b, d,
                                       ir_sym_operand(l->m,
                                           ir_sym_offset_of(l->m,
                                               descriptor_agg(l), index))));
    return temp(l, ir_load(l->f, l->b, type, at));
}

/* DESIGN: `p is *T` holds when the object is T or a class below it. The
   ancestors of a class are its descriptors from the root down. The entry
   at T's depth is therefore T's descriptor for every class below T. An
   object shallower than T has no entry there, so the depth is compared
   first. The two comparisons are one branch each. */
static struct ir_operand class_test(struct lowerer *l, struct ir_operand p,
                                    const struct type *to)
{
    uint32_t depth = class_depth(to);
    struct ir_operand table = temp(l, ir_load(l->f, l->b, IR_PTR, p));
    struct ir_operand descriptor =
        temp(l, ir_load(l->f, l->b, IR_PTR, table));
    struct ir_operand object_depth =
        descriptor_field(l, descriptor, 4, IR_I64);
    struct ir_block *deep;
    struct ir_block *join;
    uint32_t result;
    struct ir_operand ancestors;
    struct ir_operand at;
    struct ir_operand found;

    result = ir_unary(l->f, l->b, IR_COPY, IR_I8, ir_int_op(IR_I8, 0));
    deep = new_block(l);
    join = new_block(l);
    ir_branch(l->f, l->b,
              temp(l, ir_binary(l->f, l->b, IR_SGE, IR_I8, object_depth,
                                ir_int_op(IR_I64, depth))),
              deep, join);
    l->b = deep;
    ancestors = descriptor_field(l, descriptor, 5, IR_PTR);
    at = temp(l, ir_load(l->f, l->b, IR_PTR,
                         offset_address(l, ancestors,
                                        entry_offset(l, (int)depth))));
    found = temp(l, ir_binary(l->f, l->b, IR_EQ, IR_I8, at,
                              temp(l, ir_addr(l->f, l->b,
                                  ir_global_op(class_descriptor(l, to))))));
    ir_assign(l->f, l->b, result, found);
    ir_jump(l->f, l->b, join);
    l->b = join;
    return temp(l, result);
}

/* `p as *T` traps on a mismatch and `p as? *T` gives null. */
static struct ir_operand checked_cast(struct lowerer *l, struct ir_operand p,
                                      const struct type *to, bool gives_null,
                                      bool from_sub)
{
    static const enum ir_type params[] = {IR_PTR, IR_I64};
    struct ir_operand ok = class_test(l, p, to);
    struct ir_block *bad = new_block(l);
    struct ir_block *join = new_block(l);
    struct token_text text;
    const struct ir_global *name;
    struct ir_operand args[2];
    uint32_t result = ir_unary(l->f, l->b, IR_COPY, IR_PTR, p);

    /* A pointer to an interface sub-object leads back to the object by
       the offset its descriptor holds. */
    if (from_sub) {
        struct ir_operand table = temp(l, ir_load(l->f, l->b, IR_PTR, p));
        struct ir_operand descriptor =
            temp(l, ir_load(l->f, l->b, IR_PTR, table));
        struct ir_operand offset =
            descriptor_field(l, descriptor, 9, IR_I64);
        ir_assign(l->f, l->b, result,
                  temp(l, ir_ptradd(l->f, l->b, p,
                                    temp(l, ir_binary(l->f, l->b, IR_SUB,
                                                      IR_I64,
                                                      ir_int_op(IR_I64, 0),
                                                      offset)))));
    }
    ir_branch(l->f, l->b, ok, join, bad);
    l->b = bad;
    if (gives_null) {
        ir_assign(l->f, l->b, result, ir_int_op(IR_PTR, 0));
    } else {
        text.bytes = to->name.text;
        text.length = to->name.length;
        name = literal_global(l, &text);
        args[0] = temp(l, ir_addr(l->f, l->b, ir_global_op(name)));
        args[1] = ir_int_op(IR_I64, to->name.length);
        ir_call(l->f, l->b, IR_VOID,
                ir_func_op(rt_function(l, "anti_rt_cast_failed", params, 2)),
                args, 2);
    }
    ir_jump(l->f, l->b, join);
    l->b = join;
    return temp(l, result);
}

/* The value of e as an i64, which every atomic operation of the runtime
   takes. A narrower value extends, and a pointer is copied. */
static struct ir_operand widen_to_i64(struct lowerer *l, const struct expr *e)
{
    struct ir_operand v = lower_expr(l, e);
    enum ir_type from = ir_type_of(e->type);

    if (l->failed || from == IR_I64 || from == IR_PTR) {
        return v;
    }
    return temp(l, ir_unary(l->f, l->b,
                            type_is_signed(e->type) ? IR_SEXT : IR_ZEXT,
                            IR_I64, v));
}

/* The i64 a runtime operation gave back, as a value of type t. */
static struct ir_operand narrow_from_i64(struct lowerer *l,
                                         struct ir_operand v,
                                         const struct type *t)
{
    enum ir_type to = ir_type_of(t);

    if (to == IR_I64 || to == IR_PTR) {
        return v;
    }
    return temp(l, ir_unary(l->f, l->b, IR_TRUNC, to, v));
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
    /* A class test, and a conversion down a chain, which needs a check.
       A conversion up a chain is the same address and needs none. */
    if (e->as.cast.test) {
        return class_test(l, v, e->as.cast.target);
    }
    if (from->kind == TYPE_POINTER && from->element->kind == TYPE_CLASS &&
        to->kind == TYPE_POINTER && to->element->kind == TYPE_CLASS) {
        /* A conversion up a chain is the same address. One that leaves
           an interface sub-object moves back to the object, whatever the
           depths say, because the two chains are unrelated. */
        if (!e->as.cast.from_sub &&
            class_depth(to->element) <= class_depth(from->element)) {
            return v;
        }
        return checked_cast(l, v, to->element, e->as.cast.checked,
                            e->as.cast.from_sub);
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
    struct ir_operand bound = none();
    struct ir_operand *args;
    uint32_t result;
    enum ir_type declared;
    size_t i;

    /* The callee comes before the arguments, from left to right. A
       bound function gives its object as the first argument and its
       entry as the target. */
    if (callee->type != NULL && callee->type->kind == TYPE_FN &&
        callee->type->bound) {
        struct ir_operand value = lower_address(l, callee);
        if (l->failed) {
            return none();
        }
        bound = temp(l, ir_load(l->f, l->b, IR_PTR, value));
        target = temp(l, ir_load(l->f, l->b, IR_PTR,
                                 offset_address(l, value,
                                     field_offset(l, callee->type,
                                                  &entry_name))));
        direct = false;
    } else if (!direct) {
        target = lower_expr(l, callee);
    }
    args = malloc((n + 2) * sizeof *args);
    if (args == NULL) {
        fputs("antic: out of memory\n", stderr);
        exit(70);
    }
    if (bound.kind != IR_NONE) {
        args[0] = bound;
    }
    for (i = 0; i < n; i++) {
        args[i + (bound.kind != IR_NONE ? 1 : 0)] =
            lower_expr(l, e->as.call.args[i]);
    }
    if (bound.kind != IR_NONE) {
        n++;
    }
    if (l->failed) {
        free(args);
        return none();
    }
    if (e->as.call.out != NULL) {
        args[n++] = l->out_address;
    }
    /* DESIGN: a call through the table loads the table pointer from the
       object, which is its first word, then the entry of the function.
       The index is the same in every class of a chain, so the entry the
       concrete class filled is the one this call reads. */
    if (e->as.call.dispatch != NULL && n > 0) {
        int index = table_index(e->as.call.dispatch, &e->as.call.entry);
        if (index > 0) {
            struct ir_operand table =
                temp(l, ir_load(l->f, l->b, IR_PTR, args[0]));
            target = temp(l, ir_load(l->f, l->b, IR_PTR,
                                     offset_address(l, table,
                                                    entry_offset(l, index))));
            direct = false;
        }
    }
    /* A call whose error a handler takes gives the error pointer here.
       The value of the expression is what the out parameter received, so
       the IR type comes from the function and not from the node. */
    declared = callee->type != NULL && callee->type->kind == TYPE_FN
                   ? ir_type_of(callee->type->result)
                   : ir_type_of(e->type);
    if (e->as.call.handler.kind == HANDLE_NONE) {
        declared = ir_type_of(e->type);
    }
    if (direct) {
        result = ir_call(l->f, l->b, declared,
                         ir_func_op(callee_function(l, sym)), args, n);
    } else {
        result = ir_call_indirect(l->f, l->b, declared, target,
                                  bound.kind != IR_NONE
                                      ? bound_signature(l, callee->type)
                                      : signature(l, callee->type),
                                  args, n);
    }
    free(args);
    return result == IR_NO_RESULT ? none() : temp(l, result);
}

/* Threads */

/* The count of `parallel` thunks already written here, which names the
   next one. */
static uint32_t next_thunk(const struct lowerer *l, const char *prefix)
{
    size_t length = strlen(prefix);
    uint32_t count = 0;
    size_t i;

    for (i = 0; i < l->m->function_count; i++) {
        const struct ir_function *g = l->m->functions[i];
        if (g->module != NULL && strcmp(g->module, l->module_name) == 0 &&
            strncmp(g->name, prefix, length) == 0) {
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
/* DESIGN: the thunk of a dispatch has the shape the runtime calls: the
   context, the object and the address of the result. It unpacks the
   context and calls the worker with the object first. */
static struct ir_function *dispatch_thunk(struct lowerer *l,
                                          const struct expr *e,
                                          const char *name, uint32_t context)
{
    const struct expr *call = e->as.dispatch.call;
    const struct expr *callee = call->kind == EXPR_CALL
                                    ? call->as.call.callee : call;
    const struct type *result = callee->symbol->type->result;
    size_t extra = call->kind == EXPR_CALL ? call->as.call.arg_count : 0;
    struct ir_function *outer_f = l->f;
    struct ir_block *outer_b = l->b;
    struct ir_function *f;
    struct ir_operand *args;
    struct ir_block *entry;
    struct ir_operand out;
    uint32_t value;
    size_t i;

    f = ir_function_add(l->m, l->module_name, name, IR_VOID, IR_NO_AGG);
    ir_param_add(f, IR_PTR, IR_NO_AGG);     /* the context */
    ir_param_add(f, IR_PTR, IR_NO_AGG);     /* the object */
    ir_param_add(f, IR_PTR, IR_NO_AGG);     /* where the result goes */
    entry = ir_block_add(f);
    l->f = f;
    l->b = entry;

    args = malloc((extra + 1) * sizeof *args);
    if (args == NULL) {
        fputs("antic: out of memory\n", stderr);
        exit(70);
    }
    args[0] = temp(l, f->params[1].temp);
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
    out = temp(l, f->params[2].temp);
    if (result->kind == TYPE_VOID) {
        /* A worker without a result writes nothing. */
    } else if (is_aggregate(result)) {
        ir_memcopy(f, entry, out, temp(l, value), vtype_of(l, result));
    } else {
        ir_store(f, entry, ir_type_of(result), temp(l, value), out);
    }
    ir_ret(f, entry, IR_VOID, none());
    l->f = outer_f;
    l->b = outer_b;
    return f;
}

/* DESIGN: `dispatch` is one call of the runtime with the object, the size
   of the result, the thunk and the context. The runtime gives a handle
   back, which the Job holds in its one field. */
static struct ir_operand lower_dispatch(struct lowerer *l,
                                        const struct expr *e)
{
    static const enum ir_type signature[] = {IR_PTR, IR_I64, IR_PTR, IR_PTR};
    const struct expr *call = e->as.dispatch.call;
    const struct expr *callee = call->kind == EXPR_CALL
                                    ? call->as.call.callee : call;
    const struct type *result = callee->symbol->type->result;
    size_t extra = call->kind == EXPR_CALL ? call->as.call.arg_count : 0;
    struct ir_operand context = ir_int_op(IR_PTR, 0);
    struct ir_operand args[4];
    struct ir_function *f;
    uint32_t agg = IR_NO_AGG;
    uint32_t handle;
    uint32_t out;
    char name[24];
    size_t i;

    snprintf(name, sizeof name, "dispatch.%u", next_thunk(l, "dispatch."));
    args[0] = lower_expr(l, e->as.dispatch.object);
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
    args[1] = result->kind == TYPE_VOID ? ir_int_op(IR_I64, 0)
                                        : size_operand(l, result);
    f = dispatch_thunk(l, e, name, agg);
    args[2] = temp(l, ir_addr(l->f, l->b, ir_func_op(f)));
    args[3] = context;
    if (l->failed) {
        return none();
    }
    handle = ir_call(l->f, l->b, IR_PTR,
                     ir_func_op(rt_function(l, "anti_rt_dispatch", signature,
                                            4)),
                     args, 4);
    out = ir_slot(l->f, l->b, vtype_of(l, e->type));
    ir_store(l->f, l->b, IR_PTR, temp(l, handle), temp(l, out));
    return temp(l, out);
}

/* `join` waits for one job and writes its result into a slot, and
   `join_all` walks the slice of jobs. */
static struct ir_operand lower_join(struct lowerer *l, const struct expr *e)
{
    static const enum ir_type one[] = {IR_PTR, IR_I64, IR_PTR};
    static const enum ir_type all[] = {IR_PTR, IR_I64};
    const struct type *job = e->as.join.job->type;
    struct ir_operand value = lower_address(l, e->as.join.job);
    struct ir_operand args[3];
    uint32_t out;

    if (l->failed) {
        return none();
    }
    if (e->as.join.all) {
        args[0] = temp(l, ir_load(l->f, l->b, IR_PTR, value));
        args[1] = temp(l, ir_load(l->f, l->b, IR_I64,
                                  offset_address(l, value,
                                      field_offset(l, job, &len_name))));
        ir_call(l->f, l->b, IR_VOID,
                ir_func_op(rt_function(l, "anti_rt_join_all", all, 2)),
                args, 2);
        return none();
    }
    args[0] = temp(l, ir_load(l->f, l->b, IR_PTR, value));
    if (e->type->kind == TYPE_VOID) {
        args[1] = ir_int_op(IR_I64, 0);
        args[2] = ir_int_op(IR_PTR, 0);
        ir_call(l->f, l->b, IR_VOID,
                ir_func_op(rt_function(l, "anti_rt_join", one, 3)), args, 3);
        return none();
    }
    out = ir_slot(l->f, l->b, vtype_of(l, e->type));
    args[1] = size_operand(l, e->type);
    args[2] = temp(l, out);
    ir_call(l->f, l->b, IR_VOID,
            ir_func_op(rt_function(l, "anti_rt_join", one, 3)), args, 3);
    if (is_aggregate(e->type)) {
        return temp(l, out);
    }
    return temp(l, ir_load(l->f, l->b, ir_type_of(e->type), temp(l, out)));
}

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

    snprintf(name, sizeof name, "parallel.%u", next_thunk(l, "parallel."));
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

static struct ir_operand lower_expr_value(struct lowerer *l,
                                         const struct expr *e);

/* DESIGN: a pointer to a class becomes a pointer to one of its
   interfaces by adding the offset of the sub-object. The checker marked
   the expression, and every place a value flows into an interface slot
   passes through here. */
static struct ir_operand lower_expr(struct lowerer *l, const struct expr *e)
{
    struct ir_operand v = lower_expr_value(l, e);

    if (e->to_iface == NULL || l->failed) {
        return v;
    }
    return temp(l, ir_ptradd(l->f, l->b, v,
                             field_offset(l, e->to_iface->home,
                                          &e->to_iface->name)));
}

static struct ir_operand lower_expr_value(struct lowerer *l,
                                          const struct expr *e)
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
    case EXPR_DISPATCH:
        return lower_dispatch(l, e);
    case EXPR_JOIN:
        return lower_join(l, e);
    case EXPR_FIELD:
        if (e->symbol != NULL && e->symbol->kind == SYMBOL_CONST) {
            return constant(l, e->symbol->value, type);
        }
        /* A bound function is built into a slot, like any aggregate. */
        if (e->type != NULL && e->type->kind == TYPE_FN && e->type->bound) {
            return lower_address(l, e);
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
        /* One object of the literal's type, with the literal written
           into it. The count form multiplies by the element size. */
        if (e->as.alloc.value != NULL) {
            v = size_operand(l, e->type->element);
            v = temp(l, ir_call(l->f, l->b, IR_PTR,
                                ir_func_op(c_function(l, "malloc", IR_PTR,
                                                      IR_I64)),
                                &v, 1));
            if (!l->failed) {
                build_into(l, e->as.alloc.value, v);
            }
            return v;
        }
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
    /* DESIGN: an atomic operation is a call of the runtime, which holds
       one body per operation and switches on the width. The runtime is
       compiled per target with optimisation, so each body is the
       instruction the target gives: `lock xadd` and `lock cmpxchg` on
       x86_64, `ldaddal` and `casal` on an ARM64 with LSE, and a
       load-store-exclusive loop on one without it. */
    case EXPR_ATOMIC: {
        static const char *const names[] = {
            "anti_rt_atomic_load", "anti_rt_atomic_store",
            "anti_rt_atomic_swap", "anti_rt_atomic_add",
            "anti_rt_atomic_sub", "anti_rt_atomic_and",
            "anti_rt_atomic_or", "anti_rt_atomic_compare_swap"
        };
        static const enum ir_type four[] = {IR_PTR, IR_I64, IR_I64, IR_I64};
        const struct type *of = e->as.atomic.place->type;
        /* The runtime gives an i64 back. A field of pointer width is
           the same bits, so the call takes that type and no conversion
           stands between them. */
        enum ir_type result = e->type->kind == TYPE_VOID ? IR_VOID
                              : e->as.atomic.op == ATOMIC_CAS ? IR_I8
                              : ir_type_of(e->type) == IR_PTR ? IR_PTR
                                                              : IR_I64;
        struct ir_function *f;
        struct ir_operand args[4];
        size_t n = 2;
        uint32_t call;
        args[0] = lower_address(l, e->as.atomic.place);
        args[1] = size_operand(l, of);
        if (e->as.atomic.a != NULL) {
            args[n++] = widen_to_i64(l, e->as.atomic.a);
        }
        if (e->as.atomic.b != NULL) {
            args[n++] = widen_to_i64(l, e->as.atomic.b);
        }
        if (l->failed) {
            return none();
        }
        f = rt_function(l, names[e->as.atomic.op], four, n);
        f->result = result;
        call = ir_call(l->f, l->b, result, ir_func_op(f), args, n);
        if (result == IR_VOID) {
            return none();
        }
        if (result == IR_I8 || result == IR_PTR) {
            return temp(l, call);
        }
        return narrow_from_i64(l, temp(l, call), e->type);
    }
    /* The three read the table of the object, so the runtime does the
       walk and the compiler passes the pointer. */
    case EXPR_OBJECT: {
        const char *name = e->as.object.op == TOKEN_DUP    ? "anti_rt_dup"
                           : e->as.object.op == TOKEN_DELETE
                               ? "anti_rt_delete"
                               : "anti_rt_destroy";
        bool gives = e->as.object.op == TOKEN_DUP;
        uint32_t call;
        v = lower_expr(l, e->as.object.operand);
        if (l->failed) {
            return none();
        }
        call = ir_call(l->f, l->b, gives ? IR_PTR : IR_VOID,
                       ir_func_op(c_function(l, name,
                                             gives ? IR_PTR : IR_VOID,
                                             IR_PTR)),
                       &v, 1);
        return gives ? temp(l, call) : none();
    }
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
static void run_defers_to(struct lowerer *l, const struct defers *stop);

/* Whether any block the function is inside has a statement to run. */
static bool has_defers(const struct lowerer *l)
{
    const struct defers *scope;

    for (scope = l->defers; scope != NULL; scope = scope->outer) {
        if (scope->count > 0) {
            return true;
        }
    }
    return false;
}

/* Room for one more exit action in scope. */
static struct exit_action *grow_defers(struct defers *scope)
{
    struct exit_action *items;

    if (scope->count < scope->capacity) {
        return scope->items;
    }
    scope->capacity = scope->capacity == 0 ? 4 : scope->capacity * 2;
    items = realloc(scope->items, scope->capacity * sizeof *items);
    if (items == NULL) {
        fputs("antic: out of memory\n", stderr);
        exit(70);
    }
    scope->items = items;
    return items;
}

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
    loop.defers_at = l->defers;
    l->loop_depth++;
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
    l->loop_depth--;
    if (!is_while) {
        l->b = test;
        lower_branch(l, s->as.loop.cond, body, exit);
    }
    l->b = exit;
}

/* DESIGN: `for` is a loop of its own rather than a rewrite into `while`,
   because the step is the target of `continue`. A textual rewrite would
   put the step after the body, where `continue` jumps over it and the
   loop would never end. */
static void lower_for(struct lowerer *l, const struct stmt *s)
{
    struct symbol *sym = s->as.for_loop.symbol;
    const struct expr *over = s->as.for_loop.over;
    struct ir_block *test = new_block(l);
    struct ir_block *body = new_block(l);
    struct ir_block *step = new_block(l);
    struct ir_block *exit = new_block(l);
    struct loop loop;
    struct ir_operand limit;
    struct ir_operand base = none();
    const struct type *seq = NULL;
    enum ir_type counter_type = IR_I64;
    uint32_t counter;
    int64_t stride = s->as.for_loop.step_value;
    bool down = stride < 0;

    /* The bound is read once, before the loop. */
    if (over != NULL) {
        seq = over->type;
        base = lower_address(l, over);
        if (l->failed) {
            return;
        }
        if (seq->kind == TYPE_SLICE) {
            limit = temp(l, ir_load(l->f, l->b, IR_I64,
                                    offset_address(l, base,
                                                   field_offset(l, seq,
                                                                &len_name))));
            base = temp(l, ir_load(l->f, l->b, IR_PTR, base));
        } else {
            limit = seq->length_of != NULL
                        ? ir_sym_operand(l->m, sym_of(l, seq->length_of))
                        : ir_int_op(IR_I64, seq->length);
        }
        counter = ir_unary(l->f, l->b, IR_COPY, IR_I64, ir_int_op(IR_I64, 0));
    } else {
        struct ir_operand low = lower_expr(l, s->as.for_loop.low);
        struct ir_operand high;
        counter_type = ir_type_of(s->as.for_loop.low->type);
        high = lower_expr(l, s->as.for_loop.high);
        if (l->failed) {
            return;
        }
        /* DESIGN: `by -k` walks the values of `by k` in reverse, so it
           starts at the largest of them and not at the high bound. That
           value is `low + ((high - low - 1) / k) * k`, and the division
           is by a constant and runs once.

           The counter holds the next value plus k, and the body
           subtracts before it reads. The test is then `counter >= low +
           k`, which never wraps below zero the way `counter - k` would
           on an unsigned type. An empty range leaves the counter at
           `low`, where the first test already fails. */
        if (down) {
            uint64_t k = (uint64_t)-stride;
            struct ir_block *first = new_block(l);
            struct ir_operand span =
                temp(l, ir_binary(l->f, l->b, IR_SUB, counter_type, high, low));
            limit = temp(l, ir_binary(l->f, l->b, IR_ADD, counter_type, low,
                                      ir_int_op(counter_type, k)));
            counter = ir_unary(l->f, l->b, IR_COPY, counter_type, low);
            ir_branch(l->f, l->b,
                      temp(l, ir_binary(l->f, l->b, IR_SGT, IR_I8, span,
                                        ir_int_op(counter_type, 0))),
                      first, test);
            l->b = first;
            ir_assign(
                l->f, l->b, counter,
                temp(l, ir_binary(
                            l->f, l->b, IR_ADD, counter_type, limit,
                            temp(l, ir_binary(
                                        l->f, l->b, IR_MUL, counter_type,
                                        temp(l, ir_binary(
                                                    l->f, l->b, IR_SDIV,
                                                    counter_type,
                                                    temp(l, ir_binary(
                                                                l->f, l->b,
                                                                IR_SUB,
                                                                counter_type,
                                                                span,
                                                                ir_int_op(counter_type, 1))),
                                                    ir_int_op(counter_type, k))),
                                        ir_int_op(counter_type, k))))));
        } else {
            limit = high;
            counter = ir_unary(l->f, l->b, IR_COPY, counter_type, low);
        }
    }
    loop.continue_to = step;
    loop.break_to = exit;
    loop.outer = l->loop;
    loop.defers_at = l->defers;
    l->loop_depth++;
    ir_jump(l->f, l->b, test);
    l->b = test;
    ir_branch(l->f, l->b,
              temp(l, ir_binary(l->f, l->b, down ? IR_SGE : IR_SLT, IR_I8,
                                temp(l, counter), limit)),
              body, exit);
    l->loop = &loop;
    l->b = body;
    /* The variable of the body: the counter of a range, or the element
       that the index reaches. A range without a name counts and reads
       nothing. */
    if (over == NULL) {
        if (down) {
            ir_assign(l->f, l->b, counter,
                      temp(l, ir_binary(l->f, l->b, IR_SUB, counter_type,
                                        temp(l, counter),
                                        ir_int_op(counter_type, (uint64_t)-stride))));
        }
        if (sym != NULL) {
            sym->ir = ir_unary(l->f, l->b, IR_COPY, counter_type,
                               temp(l, counter));
        }
    } else {
        struct ir_operand at =
            temp(l, ir_ptradd(l->f, l->b, base,
                              temp(l, ir_binary(l->f, l->b, IR_MUL, IR_I64,
                                                temp(l, counter),
                                                size_operand(l,
                                                             seq->element)))));
        if (s->as.for_loop.by_pointer) {
            sym->ir = ir_unary(l->f, l->b, IR_COPY, IR_PTR, at);
        } else if (is_aggregate(sym->type)) {
            ir_memcopy(l->f, l->b, temp(l, sym->ir), at,
                       vtype_of(l, sym->type));
        } else {
            sym->ir = ir_load(l->f, l->b, ir_type_of(sym->type), at);
        }
    }
    lower_block(l, s->as.for_loop.body);
    if (l->b != NULL) {
        ir_jump(l->f, l->b, step);
    }
    l->loop = loop.outer;
    l->loop_depth--;
    l->b = step;
    if (!down) {
        ir_assign(l->f, l->b, counter,
                  temp(l, ir_binary(l->f, l->b, IR_ADD, counter_type,
                                    temp(l, counter),
                                    ir_int_op(counter_type, (uint64_t)stride))));
    }
    ir_jump(l->f, l->b, test);
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
/* DESIGN: a local of a class whose chain declares `destruct` or owns
   memory is torn down at the end of its block, as if the program had
   written `defer destroy(&c)` after the `let`. A heap object is never
   torn down by itself, and `delete` is the only way to free one. */
static bool type_needs_destruct(const struct type *t)
{
    size_t i;

    if (t == NULL || t->kind != TYPE_CLASS) {
        return false;
    }
    for (; t != NULL; t = t->kind == TYPE_CLASS ? t->base : NULL) {
        for (i = 0; i < t->member_count; i++) {
            const struct item *m = t->members[i];
            static const struct name destruct_name = {"destruct", 8};
            if (m->kind == ITEM_FN && same_name(&m->name, &destruct_name) &&
                m->body != NULL) {
                return true;
            }
        }
        for (i = 0; i < t->field_count; i++) {
            if (t->fields[i].owned) {
                return true;
            }
        }
    }
    return false;
}

static void destroy_local(struct lowerer *l, const struct symbol *sym)
{
    static const enum ir_type params[] = {IR_PTR};
    struct ir_operand p = temp(l, sym->ir);

    ir_call(l->f, l->b, IR_VOID,
            ir_func_op(rt_function(l, "anti_rt_destroy", params, 1)), &p, 1);
}

/* DESIGN: a call that can fail gives a pointer. A null pointer is
   success, so the branch after the call is the whole of the error
   machinery. It is one compare and one branch, and nothing unwinds. */
static void handle_error(struct lowerer *l, const struct expr *call,
                         struct ir_operand err, struct ir_operand out,
                         bool has_out, struct ir_operand release)
{
    static const enum ir_type one[] = {IR_PTR};
    const struct handler *h = &call->as.call.handler;
    struct ir_block *bad = new_block(l);
    struct ir_block *join = new_block(l);
    struct handling scope;
    uint32_t error;

    ir_branch(l->f, l->b,
              temp(l, ir_binary(l->f, l->b, IR_EQ, IR_I8, err,
                                ir_int_op(IR_PTR, 0))),
              join, bad);
    l->b = bad;
    /* A `construct` that fails leaves no object behind, so the memory
       it was given goes back before the handler runs. */
    if (release.kind != IR_NONE) {
        ir_call(l->f, l->b, IR_VOID,
                ir_func_op(c_function(l, "free", IR_VOID, IR_PTR)),
                &release, 1);
    }
    switch (h->kind) {
    case HANDLE_NONE:
    case HANDLE_ENCLOSING:
        /* The `try` block around the call holds the one handler. */
        if (l->try_scope != NULL) {
            ir_assign(l->f, l->b, l->try_scope->error, err);
            ir_jump(l->f, l->b, l->try_scope->handler);
        } else {
            ir_jump(l->f, l->b, join);
        }
        break;
    case HANDLE_TRY:
        run_defers_to(l, NULL);
        if (l->b != NULL) {
            ir_ret(l->f, l->b, IR_PTR, err);
        }
        break;
    case HANDLE_FATAL: {
        static const struct name fatal_name = {"fatal", 5};
        const struct type *error_type = call->as.call.callee->type->result;
        int index = table_index(error_type->element, &fatal_name);
        struct ir_operand table = temp(l, ir_load(l->f, l->b, IR_PTR, err));
        struct ir_operand entry =
            temp(l, ir_load(l->f, l->b, IR_PTR,
                            offset_address(l, table,
                                           entry_offset(l, index))));
        struct ir_operand self = err;
        ir_call_indirect(l->f, l->b, IR_VOID, entry,
                         fatal_signature(l), &self, 1);
        ir_jump(l->f, l->b, join);
        break;
    }
    case HANDLE_BLOCK:
        error = ir_unary(l->f, l->b, IR_COPY, IR_PTR, err);
        if (h->symbol != NULL) {
            ((struct symbol *)h->symbol)->ir = error;
        }
        scope.join = join;
        scope.out = out;
        scope.has_out = has_out;
        scope.error = error;
        scope.passes = h->passes;
        scope.outer = l->handling;
        l->handling = &scope;
        lower_block(l, h->body);
        l->handling = scope.outer;
        /* The error belongs to the handler, which ends it, unless the
           handler hands it to the caller with `return e`. */
        if (l->b != NULL && !h->passes) {
            struct ir_operand p = temp(l, error);
            ir_call(l->f, l->b, IR_VOID,
                    ir_func_op(rt_function(l, "anti_rt_delete", one, 1)),
                    &p, 1);
        }
        if (l->b != NULL) {
            ir_jump(l->f, l->b, join);
        }
        break;
    }
    l->b = join;
}

/* DESIGN: `T(args)` writes the table pointers and the defaults of the
   whole chain. Then it runs every `construct` without arguments, base
   first, and last the one the class declares with the arguments. The
   object is complete before its own body sees it. */
static struct ir_operand lower_construct(struct lowerer *l,
                                         const struct expr *e,
                                         struct ir_operand dest)
{
    static const struct name construct_name = {"construct", 9};
    const struct type *t = e->as.call.builds;
    const struct type *up;
    struct ir_operand *args;
    const struct item *m = NULL;
    uint32_t result;
    size_t i;

    ir_store(l->f, l->b, IR_PTR,
             temp(l, ir_addr(l->f, l->b, ir_global_op(class_table(l, t)))),
             dest);
    store_interface_tables(l, t, dest);
    for (up = t; up != NULL; up = up->kind == TYPE_CLASS ? up->base : NULL) {
        for (i = 0; i < up->field_count && !l->failed; i++) {
            const struct struct_field *field = &up->fields[i];
            if (field->value == NULL) {
                continue;
            }
            store_value(l, field->type, (struct expr *)field->value,
                        offset_address(l, dest,
                                       field_offset(l, up, &field->name)));
        }
    }
    if (t->base != NULL) {
        run_construct(l, t->base, dest);
    }
    for (i = 0; i < t->member_count; i++) {
        if (t->members[i]->kind == ITEM_FN &&
            same_name(&t->members[i]->name, &construct_name)) {
            m = t->members[i];
        }
    }
    if (m == NULL || m->symbol == NULL || l->failed) {
        return none();
    }
    args = malloc((e->as.call.arg_count + 1) * sizeof *args);
    if (args == NULL) {
        fputs("antic: out of memory\n", stderr);
        exit(70);
    }
    args[0] = dest;
    for (i = 0; i < e->as.call.arg_count; i++) {
        args[i + 1] = lower_expr(l, e->as.call.args[i]);
    }
    if (l->failed) {
        free(args);
        return none();
    }
    result = ir_call(l->f, l->b,
                     m->result != NULL ? IR_PTR : IR_VOID,
                     ir_func_op(callee_function(l, m->symbol)), args,
                     e->as.call.arg_count + 1);
    free(args);
    return m->result != NULL ? temp(l, result) : none();
}

/* Whether the expression is a call whose error a handler takes. */
static bool is_handled_call(const struct expr *e)
{
    return e->kind == EXPR_CALL && e->as.call.builds == NULL &&
           e->as.call.handler.kind != HANDLE_NONE;
}

static void lower_let(struct lowerer *l, const struct stmt *s)
{
    struct symbol *sym = s->as.let.symbol;
    struct ir_operand v;

    /* `let c = alloc T(args) catch e { }`: the object goes on the heap,
       its `construct` runs, and the handler may put another pointer in
       c's place with `yield`. */
    if (s->as.let.value->kind == EXPR_ALLOC &&
        s->as.let.value->as.alloc.value != NULL &&
        s->as.let.value->as.alloc.value->kind == EXPR_CALL &&
        s->as.let.value->as.alloc.value->as.call.builds != NULL) {
        const struct expr *call = s->as.let.value->as.alloc.value;
        struct ir_operand place = temp(l, sym->ir);
        struct ir_operand size = size_operand(l, sym->type->element);
        struct ir_operand object =
            temp(l, ir_call(l->f, l->b, IR_PTR,
                            ir_func_op(c_function(l, "malloc", IR_PTR,
                                                  IR_I64)),
                            &size, 1));
        struct ir_operand err;
        if (l->failed) {
            return;
        }
        ir_store(l->f, l->b, IR_PTR, object, place);
        err = lower_construct(l, call, object);
        if (l->failed) {
            return;
        }
        if (err.kind != IR_NONE) {
            handle_error(l, call, err, place, true, object);
        }
        return;
    }
    /* `let n = f(args) catch e { }`: the call writes n through the out
       parameter the compiler supplies, and the handler runs on an
       error. */
    if (is_handled_call(s->as.let.value)) {
        struct ir_operand out = temp(l, sym->ir);
        struct ir_operand err;
        bool has_out = s->as.let.value->as.call.out != NULL;
        l->out_address = out;
        err = lower_call(l, s->as.let.value);
        if (l->failed) {
            return;
        }
        handle_error(l, s->as.let.value, err, out, has_out, none());
        return;
    }
    if (is_aggregate(sym->type)) {
        build_into(l, s->as.let.value, temp(l, sym->ir));
        if (type_needs_destruct(sym->type)) {
            l->defers->items = grow_defers(l->defers);
            l->defers->items[l->defers->count].stmt = NULL;
            l->defers->items[l->defers->count++].local = sym;
        }
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

/* DESIGN: an assertion is a branch to a block that calls the runtime and
   falls through to the rest. The failure block carries a flag. The build
   that compiles the program cuts the branch, and the ordinary passes
   remove the block, the call and the text. */
static void assert_branch(struct lowerer *l, struct ir_operand cond,
                          const struct ir_global *text)
{
    static const enum ir_type params[] = {IR_PTR, IR_I64};
    struct ir_block *fail = new_block(l);
    struct ir_block *rest = new_block(l);
    struct ir_operand args[2];

    fail->assert_fail = true;
    ir_branch(l->f, l->b, cond, rest, fail);
    l->b = fail;
    args[0] = temp(l, ir_addr(l->f, l->b, ir_global_op(text)));
    args[1] = ir_int_op(IR_I64, text->size - 1);
    ir_call(l->f, l->b, IR_VOID,
            ir_func_op(rt_function(l, "anti_rt_assert_failed", params, 2)),
            args, 2);
    ir_jump(l->f, l->b, rest);
    l->b = rest;
}

static void lower_stmt(struct lowerer *l, const struct stmt *s)
{
    struct ir_operand v;

    switch (s->kind) {
    case STMT_LET:
        lower_let(l, s);
        return;
    /* `yield v` writes the value the failing call would have written and
       leaves the handler. */
    case STMT_YIELD: {
        const struct handling *h = l->handling;
        if (h == NULL) {
            return;
        }
        if (s->as.yielded != NULL && h->has_out) {
            store_value(l, s->as.yielded->type, s->as.yielded, h->out);
        } else if (s->as.yielded != NULL) {
            lower_expr(l, s->as.yielded);
        }
        /* `yield` is an exit of the handler, so the error ends here. */
        if (l->b != NULL && !h->passes) {
            static const enum ir_type one[] = {IR_PTR};
            struct ir_operand p = temp(l, h->error);
            ir_call(l->f, l->b, IR_VOID,
                    ir_func_op(rt_function(l, "anti_rt_delete", one, 1)),
                    &p, 1);
        }
        if (l->b != NULL) {
            ir_jump(l->f, l->b, h->join);
            l->b = NULL;
        }
        return;
    }
    /* DESIGN: `try { } catch e { }` gives every failing call of the body
       one handler. The first error abandons the rest of the block and
       runs its deferred statements on the way out. */
    case STMT_TRY: {
        struct try_scope scope;
        struct ir_block *handler = new_block(l);
        struct ir_block *join = new_block(l);
        static const enum ir_type one[] = {IR_PTR};
        const struct handler *h = &s->as.try_block.handler;
        scope.handler = handler;
        scope.error = ir_unary(l->f, l->b, IR_COPY, IR_PTR,
                               ir_int_op(IR_PTR, 0));
        scope.outer = l->try_scope;
        l->try_scope = &scope;
        lower_block(l, s->as.try_block.body);
        l->try_scope = scope.outer;
        if (l->b != NULL) {
            ir_jump(l->f, l->b, join);
        }
        l->b = handler;
        if (h->symbol != NULL) {
            ((struct symbol *)h->symbol)->ir = scope.error;
        }
        if (h->kind == HANDLE_BLOCK) {
            lower_block(l, h->body);
        }
        if (l->b != NULL && !h->passes) {
            struct ir_operand p = temp(l, scope.error);
            ir_call(l->f, l->b, IR_VOID,
                    ir_func_op(rt_function(l, "anti_rt_delete", one, 1)),
                    &p, 1);
        }
        if (l->b != NULL) {
            ir_jump(l->f, l->b, join);
        }
        l->b = join;
        return;
    }
    case STMT_CONST:
        /* Semantic analysis computed the value, and every use is a
           constant operand. */
        return;
    case STMT_EXPR:
        if (is_handled_call(s->as.expr)) {
            struct ir_operand err = lower_call(l, s->as.expr);
            if (!l->failed) {
                handle_error(l, s->as.expr, err, none(), false, none());
            }
            return;
        }
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
    case STMT_FOR:
        lower_for(l, s);
        return;
    /* DESIGN: a switch lowers to a chain of comparisons, one block per
       arm and one join. The back end turns a dense chain into a jump
       table where it pays. */
    case STMT_SWITCH: {
        struct ir_block *join = NULL;
        struct ir_operand over;
        enum ir_type type;
        size_t i;
        over = lower_expr(l, s->as.switch_stmt.value);
        if (l->failed) {
            return;
        }
        type = ir_type_of(s->as.switch_stmt.value->type);
        over = temp(l, ir_unary(l->f, l->b, IR_COPY, type, over));
        for (i = 0; i < s->as.switch_stmt.count && l->b != NULL; i++) {
            struct ir_block *arm = new_block(l);
            struct ir_block *next_test = new_block(l);
            struct ir_operand value =
                lower_expr(l, s->as.switch_stmt.arms[i].value);
            if (l->failed) {
                return;
            }
            ir_branch(l->f, l->b,
                      temp(l, ir_binary(l->f, l->b, IR_EQ, IR_I8, over,
                                        value)),
                      arm, next_test);
            l->b = arm;
            lower_stmt(l, s->as.switch_stmt.arms[i].body);
            jump_to_join(l, &join);
            l->b = next_test;
        }
        if (l->b != NULL && s->as.switch_stmt.otherwise != NULL) {
            lower_stmt(l, s->as.switch_stmt.otherwise);
        }
        jump_to_join(l, &join);
        l->b = join;
        return;
    }
    /* DESIGN: the text of a failure is built here and lives in the
       read-only data of the module. The back end needs no formatting,
       and a build without assertions drops the whole string. */
    case STMT_ASSERT: {
        struct token_text text;
        struct text message = {0};
        struct ir_operand cond = lower_expr(l, s->as.assertion.cond);
        if (l->failed) {
            return;
        }
        if (s->as.assertion.message.length > 0) {
            text_appendf(&message, "%s:%d: assertion failed: %.*s", l->file,
                         s->as.assertion.cond->pos.line,
                         (int)s->as.assertion.message.length,
                         s->as.assertion.message.bytes);
        } else {
            text_appendf(&message, "%s:%d: assertion failed: %.*s", l->file,
                         s->as.assertion.cond->pos.line,
                         (int)s->as.assertion.text.length,
                         s->as.assertion.text.bytes);
        }
        text.bytes = text_cstr(&message);
        text.length = message.length;
        assert_branch(l, cond, literal_global(l, &text));
        text_free(&message);
        return;
    }
    case STMT_DEFER:
        l->defers->items = grow_defers(l->defers);
        l->defers->items[l->defers->count].stmt = s->as.deferred;
        l->defers->items[l->defers->count++].local = NULL;
        return;
    case STMT_BREAK:
        run_defers_to(l, l->loop->defers_at);
        if (l->b != NULL) {
            ir_jump(l->f, l->b, l->loop->break_to);
        }
        l->b = NULL;
        return;
    case STMT_CONTINUE:
        run_defers_to(l, l->loop->defers_at);
        if (l->b != NULL) {
            ir_jump(l->f, l->b, l->loop->continue_to);
        }
        l->b = NULL;
        return;
    case STMT_RETURN:
        if (s->as.return_value == NULL) {
            run_defers_to(l, NULL);
            if (l->b == NULL) {
                return;
            }
            ir_ret(l->f, l->b, IR_VOID, none());
        } else {
            v = lower_expr(l, s->as.return_value);
            if (l->failed) {
                return;
            }
            /* The value is computed before the deferred statements run,
               so a `defer` cannot change what the function returns. A
               function without one keeps the value where it is. */
            if (has_defers(l) && !is_aggregate(s->as.return_value->type)) {
                v = temp(l, ir_unary(l->f, l->b, IR_COPY,
                                     ir_type_of(s->as.return_value->type), v));
            }
            /* DESIGN: `return local` hands the value to the caller, so
               the local is not destroyed on the way out. Its `own`
               fields belong to the returned value now. Every other local
               of the scope is destroyed as usual. */
            if (s->as.return_value->kind == EXPR_NAME) {
                l->moved = s->as.return_value->symbol;
            }
            run_defers_to(l, NULL);
            l->moved = NULL;
            if (l->b == NULL) {
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
/* Run the statements of one block scope, last declared first. */
static void destroy_local(struct lowerer *l, const struct symbol *sym);

static void run_defers(struct lowerer *l, const struct defers *scope)
{
    size_t i;

    for (i = scope->count; i > 0 && l->b != NULL && !l->failed; i--) {
        const struct exit_action *action = &scope->items[i - 1];
        if (action->stmt != NULL) {
            lower_stmt(l, action->stmt);
        } else if (action->local != l->moved) {
            destroy_local(l, action->local);
        }
    }
}

/* Run every scope from the innermost out to stop, which is not run. */
static void run_defers_to(struct lowerer *l, const struct defers *stop)
{
    const struct defers *scope;

    for (scope = l->defers; scope != stop; scope = scope->outer) {
        run_defers(l, scope);
    }
}

static void lower_block(struct lowerer *l, const struct block *b)
{
    struct defers scope;
    size_t i;

    memset(&scope, 0, sizeof scope);
    scope.outer = l->defers;
    l->defers = &scope;
    for (i = 0; i < b->count && l->b != NULL && !l->failed; i++) {
        lower_stmt(l, b->stmts[i]);
    }
    /* The closing brace is an exit of the block. */
    run_defers(l, &scope);
    l->defers = scope.outer;
    free(scope.items);
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
        case STMT_FOR:
            reserve_slots(l, entry, s->as.for_loop.body);
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
    size_t first;
    size_t i;

    l->f = l->m->functions[it->symbol->ir];
    l->loop = NULL;
    entry = new_block(l);
    /* DESIGN: `self` is the first IR parameter of a member function
       and has no entry in the declared list. Every declared parameter
       therefore sits one place further along. */
    first = it->has_self ? 1 : 0;
    if (it->self != NULL) {
        it->self->ir = l->f->params[0].temp;
    }
    for (i = 0; i < it->param_count; i++) {
        struct symbol *sym = it->params[i].symbol;
        sym->ir = l->f->params[i + first].temp;
        if (sym->address_taken && !is_aggregate(sym->type)) {
            sym->ir = ir_slot(l->f, entry, vtype_of(l, sym->type));
        }
    }
    reserve_slots(l, entry, it->body);
    for (i = 0; i < it->param_count; i++) {
        const struct symbol *sym = it->params[i].symbol;
        if (sym->address_taken && !is_aggregate(sym->type)) {
            ir_store(l->f, entry, l->f->params[i + first].type,
                     temp(l, l->f->params[i + first].temp), temp(l, sym->ir));
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

/* DESIGN: a singleton keeps its one instance in an atomic global of its
   own module. `get` reads it, and on the first call it builds an object
   and puts it there with a compare and swap. A second thread that lost
   the race frees what it made and reads the winner. */
static struct ir_global *singleton_instance(struct lowerer *l,
                                            const struct type *t)
{
    struct ir_const *value;
    struct ir_global *g;
    char *module;
    char *name;

    g = class_global(l, t, "instance", &module, &name);
    if (g != NULL) {
        return g;
    }
    value = arena_alloc(l->m->arena, sizeof *value);
    memset(value, 0, sizeof *value);
    value->kind = IR_CONST_INT;
    value->scalar = IR_PTR;
    value->integer = 0;
    g = ir_global_add_value(l->m, module, name, value);
    g->mutable = true;
    g->exported = t->item_exported;
    free(module);
    free(name);
    return g;
}

static void lower_singleton_get(struct lowerer *l, const struct item *it)
{
    static const enum ir_type load_params[] = {IR_PTR, IR_I64};
    static const enum ir_type swap_params[] = {IR_PTR, IR_I64, IR_I64,
                                               IR_I64};
    const struct type *t = it->owner->symbol->type;
    struct ir_operand address;
    struct ir_operand args[4];
    struct ir_operand made;
    struct ir_operand first;
    struct ir_block *build;
    struct ir_block *lost;
    struct ir_block *done;
    uint32_t result;
    uint32_t width;

    l->f = l->m->functions[it->symbol->ir];
    l->b = new_block(l);
    address = temp(l, ir_addr(l->f, l->b,
                              ir_global_op(singleton_instance(l, t))));
    width = ir_sym_size_of(l->m, ir_scalar(IR_PTR));
    args[0] = address;
    args[1] = ir_sym_operand(l->m, width);
    first = temp(l, ir_call(l->f, l->b, IR_PTR,
                            ir_func_op(rt_function(l, "anti_rt_atomic_load",
                                                   load_params, 2)),
                            args, 2));
    result = ir_unary(l->f, l->b, IR_COPY, IR_PTR, first);
    build = new_block(l);
    lost = new_block(l);
    done = new_block(l);
    ir_branch(l->f, l->b,
              temp(l, ir_binary(l->f, l->b, IR_EQ, IR_I8, first,
                                ir_int_op(IR_PTR, 0))),
              build, done);
    l->b = build;
    args[0] = size_operand(l, t);
    made = temp(l, ir_call(l->f, l->b, IR_PTR,
                           ir_func_op(c_function(l, "malloc", IR_PTR,
                                                 IR_I64)),
                           args, 1));
    ir_store(l->f, l->b, IR_PTR,
             temp(l, ir_addr(l->f, l->b, ir_global_op(class_table(l, t)))),
             made);
    store_interface_tables(l, t, made);
    {
        const struct type *up;
        size_t i;
        for (up = t; up != NULL; up = up->kind == TYPE_CLASS ? up->base
                                                             : NULL) {
            for (i = 0; i < up->field_count; i++) {
                const struct struct_field *field = &up->fields[i];
                if (field->value == NULL) {
                    continue;
                }
                store_value(l, field->type, (struct expr *)field->value,
                            offset_address(l, made,
                                           field_offset(l, up,
                                                        &field->name)));
            }
        }
    }
    run_construct(l, t, made);
    ir_assign(l->f, l->b, result, made);
    args[0] = address;
    args[1] = ir_sym_operand(l->m, width);
    args[2] = ir_int_op(IR_PTR, 0);
    args[3] = made;
    ir_branch(l->f, l->b,
              temp(l, ir_call(l->f, l->b, IR_I8,
                              ir_func_op(rt_function(l,
                                  "anti_rt_atomic_compare_swap",
                                  swap_params, 4)),
                              args, 4)),
              done, lost);
    l->b = lost;
    ir_call(l->f, l->b, IR_VOID,
            ir_func_op(c_function(l, "free", IR_VOID, IR_PTR)), &made, 1);
    args[0] = address;
    args[1] = ir_sym_operand(l->m, width);
    ir_assign(l->f, l->b, result,
              temp(l, ir_call(l->f, l->b, IR_PTR,
                              ir_func_op(rt_function(l, "anti_rt_atomic_load",
                                                     load_params, 2)),
                              args, 2)));
    ir_jump(l->f, l->b, done);
    l->b = done;
    ir_ret(l->f, l->b, IR_PTR, temp(l, result));
}

/* DESIGN: an export class gives C a function that prepares an object it
   allocated itself. It stores the table pointer and writes the default of
   every field of the chain, which is what a literal of the class does.
   An abstract class has no complete value and therefore none. */
static void class_init(struct lowerer *l, const struct item *it)
{
    const struct type *t = it->symbol->type;
    const struct type *up;
    struct ir_function *f;
    struct ir_block *entry;
    struct ir_operand self;
    char name[128];
    size_t i;

    snprintf(name, sizeof name, "anti_%.*s_init", (int)t->name.length,
             t->name.text);
    f = ir_function_add(l->m, l->module_name, name, IR_VOID, IR_NO_AGG);
    f->exported = true;
    ir_param_add(f, IR_PTR, IR_NO_AGG);
    entry = ir_block_add(f);
    l->f = f;
    l->b = entry;
    self = temp(l, f->params[0].temp);
    ir_store(l->f, l->b, IR_PTR,
             temp(l, ir_addr(l->f, l->b, ir_global_op(class_table(l, t)))),
             self);
    store_interface_tables(l, t, self);
    for (up = t; up != NULL; up = up->kind == TYPE_CLASS ? up->base : NULL) {
        for (i = 0; i < up->field_count; i++) {
            const struct struct_field *field = &up->fields[i];
            if (field->value == NULL) {
                continue;
            }
            store_value(l, field->type, (struct expr *)field->value,
                        offset_address(l, self,
                                       field_offset(l, up, &field->name)));
        }
    }
    run_construct(l, t, self);
    ir_ret(l->f, l->b, IR_VOID, none());
}

bool lower_module(struct module *module, const char *module_name,
                  struct ir_module *out, struct diagnostics *diags,
                  bool no_reflect)
{
    struct lowerer l;
    bool ok = true;
    size_t i;

    memset(&l, 0, sizeof l);
    l.m = out;
    l.diags = diags;
    l.module_name = module_name;
    l.file = module->file != NULL ? module->file : module_name;
    l.no_reflect = no_reflect;
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
                (it->members[j]->body != NULL ||
                 it->members[j]->singleton_get)) {
                declare_function(&l, it->members[j]);
            }
        }
    }
    /* DESIGN: the table and the descriptor of a class are data of the
       module that declares it. Every class builds them, whether or not a
       literal here constructs one. An abstract class has no complete
       value and therefore no table. */
    for (i = 0; i < module->item_count; i++) {
        const struct item *it = module->items[i];
        if (it->kind == ITEM_CLASS && !it->is_abstract &&
            it->symbol != NULL && it->symbol->type != NULL) {
            const struct type *t = it->symbol->type;
            const struct type *up;
            size_t k;
            class_table(&l, t);
            for (up = t; up != NULL;
                 up = up->kind == TYPE_CLASS ? up->base : NULL) {
                for (k = 0; k < up->field_count; k++) {
                    if (up->fields[k].form == FIELD_IMPL) {
                        interface_table(&l, t, &up->fields[k]);
                    }
                }
            }
            if (it->exported) {
                class_init(&l, it);
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
            if (it->members[j]->singleton_get) {
                lower_singleton_get(&l, it->members[j]);
            } else if (it->members[j]->kind == ITEM_FN &&
                it->members[j]->body != NULL) {
                lower_function(&l, it->members[j]);
            }
        }
        ok = ok && !l.failed;
    }
    return ok;
}
