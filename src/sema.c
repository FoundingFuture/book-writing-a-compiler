#include "sema.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* DESIGN: one pass over the syntax tree per module, after every
   module-level name is declared, so an item can be used before its
   declaration. Each expression is checked with the type its context
   expects, which is how a literal gets its type. The checker writes the
   type into every expression and a symbol into every name. */

struct scope_entry {
    struct name name;
    struct symbol *symbol;
};

struct scope {
    struct scope *parent;
    struct scope_entry *entries;
    size_t count;
    size_t capacity;
};

struct checker {
    struct types *types;
    struct arena *arena;
    struct diagnostics *diags;
    struct module *module;
    struct name module_name;
    const char *package;        /* the package this compilation builds */
    const struct interface *const *libraries;
    size_t library_count;
    struct scope module_scope;
    struct scope *scope;
    struct item *function;      /* the function whose body is checked */
    int loop_depth;
    bool target_sized;          /* a symbolic array length is allowed */
    bool atomic_place;          /* the place of an atomic operation */
    bool whole_program;         /* release mode: every class is here */
    struct type *yields;        /* the type `yield` gives in a handler */
    int handler_depth;          /* above 0, a `yield` has a place to go */
    struct block *try_block;    /* the body of the enclosing `try` block */
    struct type *error_type;    /* `*Error` of the first failing call */
    int quiet;                  /* above 0, errors are not reported */
    bool ok;
};

/* Helpers */

static void error_at(struct checker *c, struct pos pos, const char *format,
                     ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

static void error_at(struct checker *c, struct pos pos, const char *format,
                     ...)
{
    char message[160];
    va_list args;

    if (c->quiet > 0) {
        return;
    }
    va_start(args, format);
    vsnprintf(message, sizeof message, format, args);
    va_end(args);
    diagnostics_add(c->diags, pos.line, pos.column, "%s", message);
    c->ok = false;
}

/* A type name for a message, kept in one of four rotating buffers so
   that one message can name up to four types. */
static const char *tn(const struct type *t)
{
    static char buffers[4][96];
    static int next;
    struct text text = {0};
    char *buffer = buffers[next++ % 4];

    type_name(&text, t);
    snprintf(buffer, sizeof buffers[0], "%s", text_cstr(&text));
    text_free(&text);
    return buffer;
}

static struct type *builtin(struct checker *c, enum type_kind kind)
{
    return types_builtin(c->types, kind);
}

static bool is_error(const struct type *t)
{
    return t == NULL || t->kind == TYPE_ERROR;
}

static bool same_name(const struct name *a, const struct name *b)
{
    return a->length == b->length && memcmp(a->text, b->text, a->length) == 0;
}

static bool name_is(const struct name *a, const char *text)
{
    return a->length == strlen(text) && memcmp(a->text, text, a->length) == 0;
}

/* Scopes */

static struct symbol *scope_find_local(const struct scope *s,
                                       const struct name *name)
{
    size_t i;

    for (i = 0; i < s->count; i++) {
        if (same_name(&s->entries[i].name, name)) {
            return s->entries[i].symbol;
        }
    }
    return NULL;
}

static struct symbol *lookup(const struct checker *c, const struct name *name)
{
    const struct scope *s;

    for (s = c->scope; s != NULL; s = s->parent) {
        struct symbol *found = scope_find_local(s, name);
        if (found != NULL) {
            return found;
        }
    }
    return NULL;
}

/* Modules */

static const struct interface *find_library(const struct checker *c,
                                            const struct name *module)
{
    size_t i;

    for (i = 0; i < c->library_count; i++) {
        if (name_is(module, c->libraries[i]->module)) {
            return c->libraries[i];
        }
    }
    return NULL;
}

/* Whether lib imports module, directly or through its own imports. The
   depth limit ends the search when damaged libraries import each other. */
static bool depends_on(const struct checker *c, const struct interface *lib,
                       const struct name *module, size_t depth)
{
    size_t i;

    if (depth > c->library_count) {
        return false;
    }
    for (i = 0; i < lib->import_count; i++) {
        struct name imported;
        const struct interface *next;
        imported.text = lib->imports[i];
        imported.length = strlen(lib->imports[i]);
        if (same_name(&imported, module)) {
            return true;
        }
        next = find_library(c, &imported);
        if (next != NULL && depends_on(c, next, module, depth + 1)) {
            return true;
        }
    }
    return false;
}

/* DESIGN: an `internal` item reaches the modules of its own package and
   no others. The compilation names its package, and a library carries
   the name of the package that built it, so the two are compared here.
   From outside the package the item is not there at all, and the caller
   reports the name as missing. */
static bool package_shared(const struct checker *c,
                           const struct interface *lib)
{
    return c->package != NULL && lib->package.name != NULL &&
           strcmp(c->package, lib->package.name) == 0;
}

static struct symbol *library_item(const struct checker *c,
                                   const struct interface *lib,
                                   const struct name *name)
{
    size_t i;

    for (i = 0; i < lib->item_count; i++) {
        if (!same_name(&lib->items[i]->name, name)) {
            continue;
        }
        if (lib->items[i]->internal && !package_shared(c, lib)) {
            return NULL;
        }
        return lib->items[i];
    }
    return NULL;
}

/* Declare a symbol in the current scope. A second name in the same scope
   is an error, a name that an outer scope holds is shadowed. */
static struct symbol *declare(struct checker *c, enum symbol_kind kind,
                              const struct name *name, struct pos pos,
                              const char *duplicate_message)
{
    struct scope *s = c->scope;
    struct symbol *sym;

    if (scope_find_local(s, name) != NULL) {
        error_at(c, pos, duplicate_message, (int)name->length, name->text);
        return NULL;
    }
    if (s->count == s->capacity) {
        size_t capacity = s->capacity == 0 ? 16 : s->capacity * 2;
        struct scope_entry *entries =
            realloc(s->entries, capacity * sizeof *entries);
        if (entries == NULL) {
            fputs("antic: out of memory\n", stderr);
            exit(70);
        }
        s->entries = entries;
        s->capacity = capacity;
    }
    sym = arena_alloc(c->arena, sizeof *sym);
    sym->kind = kind;
    sym->name = *name;
    sym->pos = pos;
    s->entries[s->count].name = *name;
    s->entries[s->count].symbol = sym;
    s->count++;
    return sym;
}

static void enter_scope(struct checker *c, struct scope *s)
{
    memset(s, 0, sizeof *s);
    s->parent = c->scope;
    c->scope = s;
}

static void leave_scope(struct checker *c, struct scope *s)
{
    c->scope = s->parent;
    free(s->entries);
}

/* Types */

static struct type *builtin_of_token(struct checker *c, enum token_kind k)
{
    switch (k) {
    case TOKEN_BOOL_TYPE: return builtin(c, TYPE_BOOL);
    case TOKEN_CHAR_TYPE: return builtin(c, TYPE_CHAR);
    case TOKEN_I8:
    case TOKEN_C_CHAR: return builtin(c, TYPE_I8);
    case TOKEN_I16:
    case TOKEN_C_SHORT: return builtin(c, TYPE_I16);
    case TOKEN_I32:
    case TOKEN_C_INT: return builtin(c, TYPE_I32);
    case TOKEN_I64:
    case TOKEN_INT_TYPE:
    case TOKEN_C_LONGLONG: return builtin(c, TYPE_I64);
    case TOKEN_U8:
    case TOKEN_BYTE_TYPE:
    case TOKEN_C_UCHAR: return builtin(c, TYPE_U8);
    case TOKEN_U16:
    case TOKEN_C_USHORT: return builtin(c, TYPE_U16);
    case TOKEN_U32:
    case TOKEN_C_UINT: return builtin(c, TYPE_U32);
    case TOKEN_U64:
    case TOKEN_UINT_TYPE:
    case TOKEN_C_ULONGLONG:
    case TOKEN_C_SIZE_T: return builtin(c, TYPE_U64);
    case TOKEN_C_LONG: return builtin(c, TYPE_CLONG);
    case TOKEN_C_ULONG: return builtin(c, TYPE_CULONG);
    case TOKEN_C_WCHAR: return builtin(c, TYPE_CWCHAR);
    case TOKEN_F32:
    case TOKEN_C_FLOAT: return builtin(c, TYPE_F32);
    case TOKEN_F64:
    case TOKEN_FLOAT_TYPE:
    case TOKEN_C_DOUBLE: return builtin(c, TYPE_F64);
    case TOKEN_STR_TYPE: return builtin(c, TYPE_STR);
    default: return builtin(c, TYPE_ERROR);
    }
}

static struct type *check_expr(struct checker *c, struct expr *e,
                               struct type *expected);
static bool eval_const(struct checker *c, struct expr *e,
                       struct const_value *out);
static bool undefined_on_constants(struct checker *c, struct expr *e,
                                   const struct type *result);

/* A constant expression of type int with a value of at least 1, or a
   symbolic value. DESIGN: a length computed from size_of stays symbolic,
   and the back end checks that it is at least 1 on its target. Chapter 2
   allows it in struct fields and local variables. */
static struct type *array_of(struct checker *c, struct expr *e,
                             struct type *element)
{
    struct const_value v;

    if (is_error(check_expr(c, e, builtin(c, TYPE_I64))) || is_error(element)) {
        return builtin(c, TYPE_ERROR);
    }
    if (e->type->kind != TYPE_I64) {
        error_at(c, e->pos, "an array length has type `int`, found `%s`",
                 tn(e->type));
        return builtin(c, TYPE_ERROR);
    }
    if (!eval_const(c, e, &v)) {
        return builtin(c, TYPE_ERROR);
    }
    if (v.kind == CONST_SYMBOLIC) {
        if (!c->target_sized) {
            error_at(c, e->pos, "a length computed from `size_of` is allowed "
                     "only in a struct field or a local variable");
            return builtin(c, TYPE_ERROR);
        }
        return types_array_symbolic(c->types, element, v.as.symbolic);
    }
    if ((int64_t)v.as.integer < 1) {
        error_at(c, e->pos, "an array length is at least 1");
        return builtin(c, TYPE_ERROR);
    }
    return types_array(c->types, element, v.as.integer);
}

static struct type *resolve_type(struct checker *c, struct type_expr *t);
static const struct type *inherited(const struct type *t);

static bool require(struct checker *c, struct expr *e, struct type *got,
                    struct type *expected);
static bool descends_from(const struct type *a, const struct type *b);
static bool implemented_in(const struct type *t, const struct type *iface);
static void check_block(struct checker *c, struct block *b);
static struct item *find_member(const struct type *t,
                                const struct name *name);
struct worker_walk;
static void walk_function(struct worker_walk *w, const struct item *it);
static bool singleton_type(const struct type *t);
static struct symbol *method_symbol(const struct checker *c,
                                    const struct type *s,
                                    const struct name *name);
static struct expr *new_node(struct checker *c, enum expr_kind kind,
                             struct pos pos);

/* The width of a bitfield: a constant from 1 to the bits of its sized
   integer type. The field _ is the zero-width bitfield of C and has 0,
   as it does after an error. */
static uint8_t bitfield_width(struct checker *c, struct param *field,
                              struct type *t)
{
    struct const_value v;
    struct expr *e = field->bits;
    bool unit_break = field->name.length == 1 && field->name.text[0] == '_';

    if (is_error(t)) {
        return 0;
    }
    if (unit_break && e == NULL) {
        error_at(c, field->pos, "the field `_` is a zero-width bitfield, "
                 "written `_: T : 0`");
        return 0;
    }
    if (!type_is_integer(t) || type_is_target_sized(t)) {
        error_at(c, field->type->pos, "a bitfield has a sized integer type, "
                 "found `%s`", tn(t));
        return 0;
    }
    if (!require(c, e, check_expr(c, e, builtin(c, TYPE_I64)),
                 builtin(c, TYPE_I64)) ||
        !eval_const(c, e, &v)) {
        return 0;
    }
    if (unit_break && (v.kind == CONST_SYMBOLIC || v.as.integer != 0)) {
        error_at(c, field->pos, "the field `_` is a zero-width bitfield, "
                 "written `_: T : 0`");
        return 0;
    }
    if (unit_break) {
        return 0;
    }
    if (v.kind == CONST_SYMBOLIC || (int64_t)v.as.integer < 1 ||
        v.as.integer > (uint64_t)type_bits(t)) {
        error_at(c, e->pos, "a bitfield of `%s` has 1 to %d bits", tn(t),
                 type_bits(t));
        return 0;
    }
    return (uint8_t)v.as.integer;
}

/* The N of align(N): a constant power of two, or 0 after an error. */
static uint64_t alignment(struct checker *c, struct expr *e)
{
    struct const_value v;

    if (!require(c, e, check_expr(c, e, builtin(c, TYPE_I64)),
                 builtin(c, TYPE_I64)) ||
        !eval_const(c, e, &v)) {
        return 0;
    }
    if (v.kind == CONST_SYMBOLIC) {
        error_at(c, e->pos, "an alignment is a constant, not a value computed "
                 "from `size_of`");
        return 0;
    }
    if ((int64_t)v.as.integer < 1 || (v.as.integer & (v.as.integer - 1)) != 0) {
        error_at(c, e->pos, "an alignment is a power of two");
        return 0;
    }
    return v.as.integer;
}

/* The library that the module name refers to, or NULL after an error. */
static const struct interface *module_of(struct checker *c,
                                         const struct name *module,
                                         struct pos pos)
{
    struct symbol *sym = lookup(c, module);

    if (sym == NULL || sym->kind != SYMBOL_MODULE) {
        error_at(c, pos, "cannot find module `%.*s`", (int)module->length,
                 module->text);
        return NULL;
    }
    return sym->home;
}

/* The type of module.name, a pub struct of an imported module. */
static struct type *imported_struct(struct checker *c,
                                    const struct name *module,
                                    const struct name *name, struct pos pos)
{
    const struct interface *lib = module_of(c, module, pos);
    struct symbol *sym;

    if (lib == NULL) {
        return builtin(c, TYPE_ERROR);
    }
    sym = library_item(c, lib, name);
    if (sym == NULL || sym->kind != SYMBOL_STRUCT) {
        error_at(c, pos, "`%.*s` has no public struct `%.*s`",
                 (int)module->length, module->text, (int)name->length,
                 name->text);
        return builtin(c, TYPE_ERROR);
    }
    return sym->type;
}

static struct type *resolve_type_inner(struct checker *c, struct type_expr *t)
{
    struct type *element;
    struct symbol *sym;
    size_t i;

    switch (t->kind) {
    case TYPEX_BUILTIN:
        return builtin_of_token(c, t->builtin);
    case TYPEX_NAMED:
        if (t->module.length > 0) {
            return imported_struct(c, &t->module, &t->name, t->pos);
        }
        sym = scope_find_local(&c->module_scope, &t->name);
        /* DESIGN: `Object` is the root of every class chain, which the
           compiler declares. A program writes the name where the object
           model uses it, as in `equals(self, other: *Object)`, and a
           class of that name in the module wins over it. */
        if (sym == NULL && name_is(&t->name, "Object")) {
            return types_object(c->types);
        }
        if (sym == NULL || sym->kind != SYMBOL_STRUCT) {
            error_at(c, t->pos, "unknown type `%.*s`", (int)t->name.length,
                     t->name.text);
            return builtin(c, TYPE_ERROR);
        }
        return sym->type;
    case TYPEX_POINTER:
        element = resolve_type(c, t->element);
        return is_error(element) ? element : types_pointer(c->types, element);
    case TYPEX_SLICE:
        element = resolve_type(c, t->element);
        return is_error(element) ? element : types_slice(c->types, element);
    case TYPEX_ARRAY:
        return array_of(c, t->length, resolve_type(c, t->element));
    case TYPEX_FN: {
        struct type **params =
            arena_alloc(c->arena, (t->param_count + 1) * sizeof *params);
        struct type *result = builtin(c, TYPE_VOID);
        for (i = 0; i < t->param_count; i++) {
            params[i] = resolve_type(c, t->params[i]);
            if (is_error(params[i])) {
                return params[i];
            }
        }
        if (t->result != NULL && is_error(result = resolve_type(c, t->result))) {
            return result;
        }
        return types_fn(c->types, params, t->param_count, result);
    }
    }
    return builtin(c, TYPE_ERROR);
}

/* Resolve t and record the result in the node for later stages. */
static struct type *resolve_type(struct checker *c, struct type_expr *t)
{
    t->type = resolve_type_inner(c, t);
    return t->type;
}

/* The type of a function item, fn(params) -> result. A function of a
   struct body that takes self has a first parameter of type *T. */
static struct type *function_type(struct checker *c, struct item *it)
{
    size_t extra = it->has_self ? 1 : 0;
    struct type **params =
        arena_alloc(c->arena, (it->param_count + extra + 1) * sizeof *params);
    struct type *result = builtin(c, TYPE_VOID);
    size_t i;

    if (it->has_self) {
        const struct item *owner = it->owner;
        if (owner == NULL || owner->symbol == NULL ||
            owner->symbol->type == NULL) {
            return builtin(c, TYPE_ERROR);
        }
        params[0] = types_pointer(c->types, owner->symbol->type);
    }
    for (i = 0; i < it->param_count; i++) {
        params[i + extra] = resolve_type(c, it->params[i].type);
        if (is_error(params[i + extra])) {
            return params[i + extra];
        }
    }
    if (it->result != NULL && is_error(result = resolve_type(c, it->result))) {
        return result;
    }
    return types_fn(c->types, params, it->param_count + extra, result);
}

/* Places and literals */

/* An expression that denotes a location in memory, as chapter 2 lists. */
static bool is_place(const struct expr *e)
{
    const struct type *base;

    switch (e->kind) {
    case EXPR_NAME:
        return e->symbol != NULL && (e->symbol->kind == SYMBOL_LOCAL ||
                                     e->symbol->kind == SYMBOL_PARAM);
    case EXPR_UNARY:
        return e->as.unary.op == TOKEN_STAR;
    case EXPR_INDEX:
        base = e->as.index.base->type;
        return base->kind == TYPE_POINTER || base->kind == TYPE_SLICE ||
               (base->kind == TYPE_ARRAY && is_place(e->as.index.base));
    case EXPR_FIELD:
        base = e->as.field.base->type;
        return base->kind == TYPE_POINTER ||
               (type_has_fields(base) && is_place(e->as.field.base));
    default:
        return false;
    }
}

static const struct struct_field *find_field(const struct type *s,
                                             const struct name *name);
static struct type *struct_of(struct type *t);

/* Whether e reads a bitfield, which has no address. */
static bool is_bitfield(const struct expr *e)
{
    const struct type *s;
    const struct struct_field *f;

    if (e->kind != EXPR_FIELD || e->as.field.base->type == NULL) {
        return false;
    }
    s = struct_of(e->as.field.base->type);
    f = s != NULL ? find_field(s, &e->as.field.name) : NULL;
    return f != NULL && f->bits != 0;
}

static void mark_address_taken(struct expr *e)
{
    while (e->kind == EXPR_INDEX || e->kind == EXPR_FIELD) {
        struct expr *base = e->kind == EXPR_INDEX ? e->as.index.base
                                                  : e->as.field.base;
        if (base->type->kind == TYPE_POINTER || base->type->kind == TYPE_SLICE) {
            return;
        }
        e = base;
    }
    if (e->kind == EXPR_NAME && e->symbol != NULL) {
        e->symbol->address_taken = true;
    }
}

/* A literal whose type comes from its context: an integer or float
   literal, one of those after unary '-', or null. */
static bool is_untyped(const struct expr *e)
{
    if (e->kind == EXPR_UNARY && e->as.unary.op == TOKEN_MINUS) {
        e = e->as.unary.operand;
        return e->kind == EXPR_INT || e->kind == EXPR_FLOAT;
    }
    return e->kind == EXPR_INT || e->kind == EXPR_FLOAT || e->kind == EXPR_NULL;
}

static uint64_t max_of(const struct type *t)
{
    int bits = type_bits(t);
    uint64_t all = bits == 64 ? UINT64_MAX : (((uint64_t)1 << bits) - 1);
    return type_is_signed(t) ? all >> 1 : all;
}

static void set_type(struct expr *e, struct type *t)
{
    e->type = t;
}

/* An integer literal, with negative set for '-' in front of it. */
static struct type *integer_literal(struct checker *c, struct expr *e,
                                    struct expr *literal, bool negative,
                                    struct type *expected)
{
    struct type *t = builtin(c, TYPE_I64);
    uint64_t magnitude = literal->as.integer;
    const char *sign = negative ? "-" : "";

    if (expected != NULL && !is_error(expected) && expected->kind != TYPE_VOID) {
        if (type_is_integer(expected)) {
            t = expected;
        } else {
            error_at(c, e->pos, "expected `%s`, found an integer literal",
                     tn(expected));
            set_type(literal, builtin(c, TYPE_ERROR));
            return literal->type;
        }
    }
    if (negative ? (!type_is_signed(t) ? magnitude != 0
                                       : magnitude > max_of(t) + 1)
                 : magnitude > max_of(t)) {
        error_at(c, e->pos, "`%s%.*s` does not fit `%s`%s", sign,
                 (int)literal->spelling.length, literal->spelling.bytes,
                 tn(t), type_is_target_sized(t) ? " on every target" : "");
        t = builtin(c, TYPE_ERROR);
    }
    set_type(literal, t);
    return t;
}

static struct type *float_literal(struct checker *c, struct expr *e,
                                  struct expr *literal, bool negative,
                                  struct type *expected)
{
    struct type *t = builtin(c, TYPE_F64);
    char digits[128];

    (void)negative;
    if (expected != NULL && !is_error(expected) && expected->kind != TYPE_VOID) {
        if (type_is_float(expected)) {
            t = expected;
        } else {
            error_at(c, e->pos, "expected `%s`, found a float literal",
                     tn(expected));
            set_type(literal, builtin(c, TYPE_ERROR));
            return literal->type;
        }
    }
    snprintf(digits, sizeof digits, "%.*s", (int)literal->as.text.length,
             literal->as.text.bytes);
    if (t->kind == TYPE_F32 ? isinf(strtof(digits, NULL))
                            : isinf(strtod(digits, NULL))) {
        error_at(c, e->pos, "`%.*s` does not fit `%s`",
                 (int)literal->spelling.length, literal->spelling.bytes, tn(t));
        t = builtin(c, TYPE_ERROR);
    }
    set_type(literal, t);
    return t;
}

/* Report a value of type got where the context expects another type. */
/* DESIGN: a pointer to a struct converts to a pointer to any type of its
   `inherits` chain. That is the one implicit conversion of the language.
   The base lies at offset 0, so the address is the same. */
static bool converts_to_base(const struct type *got,
                             const struct type *expected)
{
    const struct type *t;

    if (got->kind != TYPE_POINTER || expected->kind != TYPE_POINTER) {
        return false;
    }
    for (t = inherited(got->element); t != NULL; t = inherited(t)) {
        if (t == expected->element) {
            return true;
        }
    }
    return false;
}

/* DESIGN: a pointer to a class converts to a pointer to any interface
   it implements. The sub-object sits inside the object, so the value
   moves by its offset, which is the one conversion in the language that
   changes an address. A name two sub-objects both reach has no single
   answer, and the program writes the path itself. */
static const struct struct_field *converts_to_interface(struct checker *c,
                                                        const struct expr *e,
                                                        const struct type *got,
                                                        const struct type *to)
{
    const struct struct_field *found = NULL;
    const struct type *t;

    if (got->kind != TYPE_POINTER || to->kind != TYPE_POINTER ||
        got->element->kind != TYPE_CLASS || to->element->kind != TYPE_CLASS) {
        return NULL;
    }
    for (t = got->element; t != NULL; t = inherited(t)) {
        size_t i;
        for (i = 0; i < t->field_count; i++) {
            if (t->fields[i].form != FIELD_IMPL ||
                !descends_from(t->fields[i].type, to->element)) {
                continue;
            }
            if (found != NULL) {
                error_at(c, e->pos, "`%s` converts to `%s` through `%.*s` and "
                         "through `%.*s`, name one", tn(got), tn(to),
                         (int)found->name.length, found->name.text,
                         (int)t->fields[i].name.length, t->fields[i].name.text);
                return NULL;
            }
            found = &t->fields[i];
        }
    }
    return found;
}

static bool require(struct checker *c, struct expr *e, struct type *got,
                    struct type *expected)
{
    const struct struct_field *iface;

    if (is_error(got) || is_error(expected) || got == expected) {
        return !is_error(got);
    }
    if (converts_to_base(got, expected)) {
        return true;
    }
    if ((iface = converts_to_interface(c, e, got, expected)) != NULL) {
        e->to_iface = iface;
        return true;
    }
    if (got->kind == TYPE_VOID) {
        if (e->kind == EXPR_CALL && e->as.call.callee->kind == EXPR_NAME) {
            error_at(c, e->pos, "`%.*s` returns no value",
                     (int)e->as.call.callee->as.name.length,
                     e->as.call.callee->as.name.text);
        } else {
            error_at(c, e->pos, "the call returns no value");
        }
        return false;
    }
    error_at(c, e->pos, "expected `%s`, found `%s`", tn(expected), tn(got));
    return false;
}

/* Expressions */

static struct symbol *operator_symbol(struct checker *c, struct type *t,
                                      const char *text);

static struct type *check_unary(struct checker *c, struct expr *e,
                                struct type *expected)
{
    struct expr *operand = e->as.unary.operand;
    struct type *t;

    /* Unary `-` and `~` call `neg` and `not` on a type that declares
       them, as the binary operators call their own. */
    if (e->as.unary.op == TOKEN_MINUS || e->as.unary.op == TOKEN_TILDE) {
        const char *called = e->as.unary.op == TOKEN_MINUS ? "neg" : "not";
        struct symbol *fn;
        if (operand->kind != EXPR_INT && operand->kind != EXPR_FLOAT) {
            t = check_expr(c, operand, NULL);
            operand->type = t;
            if (!is_error(t) && (fn = operator_symbol(c, t, called)) != NULL) {
                struct expr *call = new_node(c, EXPR_CALL, e->pos);
                struct expr *callee = new_node(c, EXPR_FIELD, e->pos);
                callee->as.field.base = operand;
                callee->as.field.name = fn->item->name;
                callee->as.field.promoted = true;
                call->as.call.callee = callee;
                call->as.call.arg_count = 0;
                *e = *call;
                return check_expr(c, e, NULL);
            }
        }
    }
    switch (e->as.unary.op) {
    case TOKEN_MINUS:
        if (operand->kind == EXPR_INT) {
            return integer_literal(c, e, operand, true, expected);
        }
        if (operand->kind == EXPR_FLOAT) {
            return float_literal(c, e, operand, true, expected);
        }
        t = check_expr(c, operand, expected);
        if (!is_error(t) && !type_is_signed(t) && !type_is_float(t)) {
            error_at(c, e->pos,
                     "unary `-` needs a signed integer or a float, found `%s`",
                     tn(t));
            return builtin(c, TYPE_ERROR);
        }
        return t;
    case TOKEN_BANG:
        t = check_expr(c, operand, NULL);
        if (!is_error(t) && t->kind != TYPE_BOOL) {
            error_at(c, e->pos, "unary `!` needs a `bool`, found `%s`", tn(t));
            return builtin(c, TYPE_ERROR);
        }
        return t;
    case TOKEN_TILDE:
        t = check_expr(c, operand, expected);
        if (!is_error(t) && !type_is_integer(t)) {
            error_at(c, e->pos, "unary `~` needs an integer, found `%s`", tn(t));
            return builtin(c, TYPE_ERROR);
        }
        return t;
    case TOKEN_STAR:
        t = check_expr(c, operand, NULL);
        if (is_error(t)) {
            return t;
        }
        if (t->kind != TYPE_POINTER) {
            error_at(c, e->pos, "unary `*` needs a pointer, found `%s`", tn(t));
            return builtin(c, TYPE_ERROR);
        }
        return t->element;
    case TOKEN_AMP:
        t = check_expr(c, operand, NULL);
        if (is_error(t)) {
            return t;
        }
        if (operand->kind == EXPR_NAME && operand->symbol != NULL &&
            operand->symbol->kind == SYMBOL_CONST) {
            error_at(c, operand->pos, "a constant has no address");
            return builtin(c, TYPE_ERROR);
        }
        if (!is_place(operand)) {
            error_at(c, operand->pos, "unary `&` needs a place");
            return builtin(c, TYPE_ERROR);
        }
        if (is_bitfield(operand)) {
            error_at(c, operand->pos, "a bitfield has no address");
            return builtin(c, TYPE_ERROR);
        }
        mark_address_taken(operand);
        return types_pointer(c->types, t);
    default:
        return builtin(c, TYPE_ERROR);
    }
}

/* Check both operands of a binary operator so that a literal takes the
   type of the other operand. outer is the type the context expects of
   the result, used when both operands are literals. */
static bool binary_operands(struct checker *c, struct expr *e,
                            struct type *outer, struct type **left,
                            struct type **right)
{
    struct expr *l = e->as.binary.left;
    struct expr *r = e->as.binary.right;

    if (is_untyped(l) && !is_untyped(r)) {
        *right = check_expr(c, r, outer);
        *left = check_expr(c, l, is_error(*right) ? outer : *right);
    } else {
        *left = check_expr(c, l, outer);
        *right = check_expr(c, r, is_error(*left) ? outer : *left);
    }
    return !is_error(*left) && !is_error(*right);
}

/* Copy the spelling of an operator without its backticks into buffer. */
static const char *op_text(enum token_kind op, char buffer[8])
{
    const char *quoted = token_kind_name(op);

    snprintf(buffer, 8, "%.*s", (int)(strlen(quoted) - 2), quoted + 1);
    return buffer;
}

/* DESIGN: the operator table is closed. An operator calls the function
   of that name on the left operand's type, and nothing else is
   overloadable. The table is the one the object model document holds. */
static const char *operator_name(enum token_kind op)
{
    switch (op) {
    case TOKEN_PLUS: return "add";
    case TOKEN_MINUS: return "sub";
    case TOKEN_STAR: return "mul";
    case TOKEN_SLASH: return "div";
    case TOKEN_PERCENT: return "rem";
    case TOKEN_EQ:
    case TOKEN_NE: return "eq";
    case TOKEN_LT:
    case TOKEN_LE:
    case TOKEN_GT:
    case TOKEN_GE: return "lt";
    case TOKEN_AMP: return "and";
    case TOKEN_PIPE: return "or";
    case TOKEN_CARET: return "xor";
    case TOKEN_SHL: return "shl";
    case TOKEN_SHR: return "shr";
    default: return NULL;
    }
}

/* DESIGN: analysis that only reports runs over every function a worker
   can reach. A worker may not `delete` its object, and it may not touch
   a `mutable` field of a singleton, because another worker may hold the
   same one. The walk is over the checked tree of this compilation. */
struct worker_walk {
    struct checker *c;
    const struct item *worker;      /* the worker the path started at */
    const struct item **seen;
    size_t seen_count;
    size_t seen_capacity;
};

static void walk_block(struct worker_walk *w, const struct block *b);

static void walk_expr(struct worker_walk *w, const struct expr *e)
{
    size_t i;

    if (e == NULL) {
        return;
    }
    switch (e->kind) {
    case EXPR_OBJECT:
        if (e->as.object.op == TOKEN_DELETE) {
            error_at(w->c, e->pos, "`%.*s` is a `worker fn` and cannot "
                     "`delete` an object another one may hold",
                     (int)w->worker->name.length, w->worker->name.text);
        }
        walk_expr(w, e->as.object.operand);
        return;
    case EXPR_FIELD: {
        const struct type *owner =
            e->as.field.base != NULL && e->as.field.base->type != NULL
                ? struct_of(e->as.field.base->type)
                : NULL;
        const struct struct_field *f =
            owner != NULL ? find_field(owner, &e->as.field.name) : NULL;
        if (f != NULL && f->writable && singleton_type(owner)) {
            error_at(w->c, e->pos, "`%.*s` is `mutable` in singleton `%s` and "
                     "`worker fn %.*s` reaches it",
                     (int)e->as.field.name.length, e->as.field.name.text,
                     tn(owner), (int)w->worker->name.length,
                     w->worker->name.text);
        }
        walk_expr(w, e->as.field.base);
        return;
    }
    case EXPR_CALL:
        walk_expr(w, e->as.call.callee);
        for (i = 0; i < e->as.call.arg_count; i++) {
            walk_expr(w, e->as.call.args[i]);
        }
        if (e->as.call.handler.kind == HANDLE_BLOCK) {
            walk_block(w, e->as.call.handler.body);
        }
        if (e->as.call.callee != NULL && e->as.call.callee->symbol != NULL) {
            walk_function(w, e->as.call.callee->symbol->item);
        }
        return;
    case EXPR_UNARY:
        walk_expr(w, e->as.unary.operand);
        return;
    case EXPR_BINARY:
        walk_expr(w, e->as.binary.left);
        walk_expr(w, e->as.binary.right);
        return;
    case EXPR_INDEX:
        walk_expr(w, e->as.index.base);
        walk_expr(w, e->as.index.index);
        return;
    case EXPR_CAST:
        walk_expr(w, e->as.cast.operand);
        return;
    case EXPR_ALLOC:
        walk_expr(w, e->as.alloc.value);
        walk_expr(w, e->as.alloc.count);
        return;
    case EXPR_FREE:
        walk_expr(w, e->as.free_pointer);
        return;
    default:
        return;
    }
}

static void walk_stmt(struct worker_walk *w, const struct stmt *s)
{
    size_t i;

    if (s == NULL) {
        return;
    }
    switch (s->kind) {
    case STMT_LET:
    case STMT_CONST:
        walk_expr(w, s->as.let.value);
        return;
    case STMT_EXPR:
        walk_expr(w, s->as.expr);
        return;
    case STMT_ASSIGN:
        walk_expr(w, s->as.assign.target);
        walk_expr(w, s->as.assign.value);
        return;
    case STMT_IF:
        for (i = 0; i < s->as.if_chain.count; i++) {
            walk_expr(w, s->as.if_chain.branches[i].cond);
            walk_block(w, s->as.if_chain.branches[i].body);
        }
        walk_block(w, s->as.if_chain.else_body);
        return;
    case STMT_WHILE:
    case STMT_DO_WHILE:
        walk_expr(w, s->as.loop.cond);
        walk_block(w, s->as.loop.body);
        return;
    case STMT_FOR:
        walk_expr(w, s->as.for_loop.over);
        walk_block(w, s->as.for_loop.body);
        return;
    case STMT_DEFER:
        walk_stmt(w, s->as.deferred);
        return;
    case STMT_RETURN:
        walk_expr(w, s->as.return_value);
        return;
    case STMT_YIELD:
        walk_expr(w, s->as.yielded);
        return;
    case STMT_TRY:
        walk_block(w, s->as.try_block.body);
        walk_block(w, s->as.try_block.handler.body);
        return;
    case STMT_BLOCK:
        walk_block(w, s->as.block);
        return;
    case STMT_SWITCH:
        walk_expr(w, s->as.switch_stmt.value);
        for (i = 0; i < s->as.switch_stmt.count; i++) {
            walk_stmt(w, s->as.switch_stmt.arms[i].body);
        }
        return;
    default:
        return;
    }
}

static void walk_block(struct worker_walk *w, const struct block *b)
{
    size_t i;

    for (i = 0; b != NULL && i < b->count; i++) {
        walk_stmt(w, b->stmts[i]);
    }
}

static void walk_function(struct worker_walk *w, const struct item *it)
{
    size_t i;

    if (it == NULL || it->kind != ITEM_FN || it->body == NULL) {
        return;
    }
    for (i = 0; i < w->seen_count; i++) {
        if (w->seen[i] == it) {
            return;
        }
    }
    if (w->seen_count == w->seen_capacity) {
        size_t capacity = w->seen_capacity == 0 ? 16 : w->seen_capacity * 2;
        const struct item **seen =
            realloc(w->seen, capacity * sizeof *seen);
        if (seen == NULL) {
            return;
        }
        w->seen = seen;
        w->seen_capacity = capacity;
    }
    w->seen[w->seen_count++] = it;
    walk_block(w, it->body);
}

/* Whether some class of the program inherits t or implements it. */
static bool fills(const struct type *t, const struct type *abstract)
{
    size_t i;

    if (t == NULL || t->kind != TYPE_CLASS || t == abstract) {
        return false;
    }
    if (descends_from(t, abstract) || implemented_in(t, abstract)) {
        return true;
    }
    for (i = 0; i < t->field_count; i++) {
        if (t->fields[i].form == FIELD_IMPL &&
            descends_from(t->fields[i].type, abstract)) {
            return true;
        }
    }
    return false;
}

static bool filled_somewhere(const struct checker *c, const struct type *t)
{
    size_t i;
    size_t j;

    for (i = 0; i < c->module->item_count; i++) {
        const struct item *it = c->module->items[i];
        if (it->kind == ITEM_CLASS && it->symbol != NULL && !it->is_abstract &&
            fills(it->symbol->type, t)) {
            return true;
        }
    }
    for (i = 0; i < c->library_count; i++) {
        const struct interface *lib = c->libraries[i];
        for (j = 0; j < lib->item_count; j++) {
            const struct symbol *sym = lib->items[j];
            if (sym->kind == SYMBOL_STRUCT && fills(sym->type, t)) {
                return true;
            }
        }
    }
    return false;
}

/* Add the generated `get` to a singleton class. It takes no arguments
   and gives the one instance. */
static void declare_get(struct checker *c, struct item *it)
{
    static const char get_text[] = "get";
    struct type *t = it->symbol->type;
    struct item **members;
    struct item *m = arena_alloc(c->arena, sizeof *m);
    struct symbol *sym = arena_alloc(c->arena, sizeof *sym);
    struct name name;
    size_t i;

    name.text = get_text;
    name.length = sizeof get_text - 1;
    if (find_member(t, &name) != NULL) {
        return;
    }
    memset(m, 0, sizeof *m);
    memset(sym, 0, sizeof *sym);
    m->kind = ITEM_FN;
    m->pub = true;
    m->vis = VIS_PUB;
    m->name = name;
    m->name_pos = it->name_pos;
    m->owner = it;
    m->symbol = sym;
    m->singleton_get = true;
    sym->kind = SYMBOL_FN;
    sym->name = name;
    sym->item = m;
    sym->type = types_fn(c->types, NULL, 0, types_pointer(c->types, t));
    members = arena_alloc(c->arena, (it->member_count + 1) * sizeof *members);
    for (i = 0; i < it->member_count; i++) {
        members[i] = it->members[i];
    }
    members[it->member_count] = m;
    it->members = members;
    it->member_count++;
    t->members = members;
    t->member_count = it->member_count;
}

/* Whether name is one of the fourteen the operator table holds. */
static bool operator_named(const struct name *name)
{
    static const char *const names[] = {
        "add", "sub", "mul", "div", "rem", "neg", "eq",
        "lt", "and", "or", "xor", "shl", "shr", "not"
    };
    size_t i;

    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        if (name_is(name, names[i])) {
            return true;
        }
    }
    return false;
}

/* The operator function `name` that the type t declares, or that the
   module of t declares for a struct. */
static struct symbol *operator_symbol(struct checker *c, struct type *t,
                                      const char *text)
{
    struct name name;
    struct item *m;
    struct symbol *sym;

    if (t == NULL || !type_has_fields(t)) {
        return NULL;
    }
    name.text = text;
    name.length = strlen(text);
    m = find_member(t, &name);
    if (m != NULL && m->kind == ITEM_FN && m->is_operator) {
        return m->symbol;
    }
    sym = method_symbol(c, t, &name);
    if (sym != NULL && sym->item != NULL && sym->item->is_operator) {
        return sym;
    }
    return NULL;
}

/* Rewrite `a op b` into the call the operator names. `!=`, `>`, `<=` and
   `>=` derive from `eq` and `lt`, so a type declares two functions and
   gets six operators. */
/* The receiver of an operator call: the operand itself, or its address
   when the function takes `self`. */
static struct expr *operator_receiver(struct checker *c, struct expr *a,
                                      struct type *first)
{
    struct expr *address;

    if (first->kind != TYPE_POINTER || a->type == first) {
        return a;
    }
    if (!is_place(a)) {
        struct expr *slot = new_node(c, EXPR_UNARY, a->pos);
        slot->as.unary.op = TOKEN_AMP;
        slot->as.unary.operand = a;
        slot->type = first;
        return slot;
    }
    mark_address_taken(a);
    address = new_node(c, EXPR_UNARY, a->pos);
    address->as.unary.op = TOKEN_AMP;
    address->as.unary.operand = a;
    address->type = first;
    return address;
}

static struct type *check_operator(struct checker *c, struct expr *e,
                                   struct type *right, struct symbol *fn)
{
    enum token_kind op = e->as.binary.op;
    bool negate = op == TOKEN_NE || op == TOKEN_LE || op == TOKEN_GE;
    bool swap = op == TOKEN_GT || op == TOKEN_LE;
    struct expr *a = swap ? e->as.binary.right : e->as.binary.left;
    struct expr *b = swap ? e->as.binary.left : e->as.binary.right;
    struct type *sig = fn->type;
    struct expr *call = new_node(c, EXPR_CALL, e->pos);
    struct expr *callee = new_node(c, EXPR_NAME, e->pos);
    struct expr **args = arena_alloc(c->arena, 2 * sizeof *args);

    if (sig->param_count != 2) {
        error_at(c, e->pos, "`operator fn %.*s` takes one operand beside its "
                 "own", (int)fn->name.length, fn->name.text);
        return builtin(c, TYPE_ERROR);
    }
    if (!require(c, b, right, sig->params[1])) {
        return builtin(c, TYPE_ERROR);
    }
    callee->symbol = fn;
    callee->type = sig;
    callee->as.name = fn->name;
    args[0] = operator_receiver(c, a, sig->params[0]);
    args[1] = b;
    call->as.call.callee = callee;
    call->as.call.args = args;
    call->as.call.arg_count = 2;
    call->type = sig->result;
    if (!negate) {
        *e = *call;
        return sig->result;
    }
    e->kind = EXPR_UNARY;
    e->as.unary.op = TOKEN_BANG;
    e->as.unary.operand = call;
    e->type = sig->result;
    return sig->result;
}

static struct type *check_binary(struct checker *c, struct expr *e,
                                 struct type *expected)
{
    enum token_kind op = e->as.binary.op;
    struct type *left;
    struct type *right;
    char spelling[8];
    const char *o = op_text(op, spelling);
    const char *called = operator_name(op);

    switch (op) {
    case TOKEN_AND_AND:
    case TOKEN_OR_OR:
        left = check_expr(c, e->as.binary.left, NULL);
        right = check_expr(c, e->as.binary.right, NULL);
        if (is_error(left) || is_error(right)) {
            return builtin(c, TYPE_ERROR);
        }
        if (left->kind != TYPE_BOOL || right->kind != TYPE_BOOL) {
            error_at(c, e->pos, "`%s` needs `bool` operands, found `%s`", o,
                     tn(left->kind != TYPE_BOOL ? left : right));
            return builtin(c, TYPE_ERROR);
        }
        return left;
    case TOKEN_EQ:
    case TOKEN_NE:
    case TOKEN_LT:
    case TOKEN_LE:
    case TOKEN_GT:
    case TOKEN_GE:
        if (!binary_operands(c, e, NULL, &left, &right)) {
            return builtin(c, TYPE_ERROR);
        }
        if (called != NULL) {
            struct symbol *fn = operator_symbol(c, left, called);
            if (fn != NULL) {
                return check_operator(c, e, right, fn);
            }
        }
        /* DESIGN: `==` on two class pointers compares the identity of
           the objects. A pointer to one interface of an object and a
           pointer to another are therefore equal. Their types differ,
           and the comparison is still the one the program means. */
        if (left != right &&
            !((op == TOKEN_EQ || op == TOKEN_NE) &&
              left->kind == TYPE_POINTER && right->kind == TYPE_POINTER &&
              left->element->kind == TYPE_CLASS &&
              right->element->kind == TYPE_CLASS)) {
            error_at(c, e->pos, "the operands of `%s` have the types `%s` and "
                     "`%s`", o, tn(left), tn(right));
            return builtin(c, TYPE_ERROR);
        }
        if (op == TOKEN_EQ || op == TOKEN_NE) {
            if (type_has_fields(left) || left->kind == TYPE_ARRAY ||
                left->kind == TYPE_SLICE || left->kind == TYPE_STR) {
                error_at(c, e->pos, "`%s` is not defined on `%s`", o, tn(left));
                return builtin(c, TYPE_ERROR);
            }
        } else if (!type_is_numeric(left) && left->kind != TYPE_CHAR) {
            error_at(c, e->pos, "`%s` needs numeric or `char` operands, found "
                     "`%s`", o, tn(left));
            return builtin(c, TYPE_ERROR);
        }
        return builtin(c, TYPE_BOOL);
    default:
        if (!binary_operands(c, e, expected, &left, &right)) {
            return builtin(c, TYPE_ERROR);
        }
        if (called != NULL) {
            struct symbol *fn = operator_symbol(c, left, called);
            if (fn != NULL) {
                return check_operator(c, e, right, fn);
            }
        }
        /* DESIGN: `==` on two class pointers compares the identity of
           the objects. A pointer to one interface of an object and a
           pointer to another are therefore equal. Their types differ,
           and the comparison is still the one the program means. */
        if (left != right &&
            !((op == TOKEN_EQ || op == TOKEN_NE) &&
              left->kind == TYPE_POINTER && right->kind == TYPE_POINTER &&
              left->element->kind == TYPE_CLASS &&
              right->element->kind == TYPE_CLASS)) {
            error_at(c, e->pos, "the operands of `%s` have the types `%s` and "
                     "`%s`", o, tn(left), tn(right));
            return builtin(c, TYPE_ERROR);
        }
        if (op == TOKEN_PLUS || op == TOKEN_MINUS || op == TOKEN_STAR ||
            op == TOKEN_SLASH) {
            if (!type_is_numeric(left)) {
                error_at(c, e->pos, "`%s` needs numeric operands, found `%s`",
                         o, tn(left));
                return builtin(c, TYPE_ERROR);
            }
        } else if (!type_is_integer(left)) {
            error_at(c, e->pos, "`%s` needs integer operands, found `%s`", o,
                     tn(left));
            return builtin(c, TYPE_ERROR);
        }
        if ((op == TOKEN_SLASH || op == TOKEN_PERCENT || op == TOKEN_SHL ||
             op == TOKEN_SHR) && type_is_integer(left) &&
            undefined_on_constants(c, e, left)) {
            return builtin(c, TYPE_ERROR);
        }
        return left;
    }
}

/* The conversion table of chapter 2. */
static bool can_convert(const struct type *from, const struct type *to)
{
    /* DESIGN: an enum converts to and from its underlying type and to
       any other numeric type, as a C enum does. Its values carry no
       other meaning to the compiler. */
    if (from->kind == TYPE_ENUM) {
        from = from->base;
    }
    if (to->kind == TYPE_ENUM) {
        to = to->base;
    }
    if (type_is_numeric(from) && type_is_numeric(to)) {
        return true;
    }
    if (from->kind == TYPE_BOOL && type_is_integer(to)) {
        return true;
    }
    if ((from->kind == TYPE_CHAR && to->kind == TYPE_U32) ||
        (from->kind == TYPE_U32 && to->kind == TYPE_CHAR)) {
        return true;
    }
    return from->kind == TYPE_POINTER && to->kind == TYPE_POINTER;
}

/* Whether a is b or a class below b, so a pointer to a converts to a
   pointer to b without a check. */
static bool descends_from(const struct type *a, const struct type *b)
{
    for (; a != NULL; a = a->kind == TYPE_CLASS ? a->base : NULL) {
        if (a == b) {
            return true;
        }
    }
    return false;
}

/* DESIGN: `p is *T` and `p as *T` on a class pointer compare the
   ancestor of the object at T's depth with T's descriptor. `is` gives a
   bool, `as` traps on a mismatch and `as?` gives null. Both need a class
   pointer on each side, and a conversion up the chain is the implicit
   one, which needs no check. */
static struct type *check_class_cast(struct checker *c, struct expr *e,
                                     struct type *from, struct type *to)
{
    const char *op = e->as.cast.test ? "is" : "as";

    if (from->kind != TYPE_POINTER || from->element->kind != TYPE_CLASS ||
        to->kind != TYPE_POINTER || to->element->kind != TYPE_CLASS) {
        error_at(c, e->pos, "`%s` needs a class pointer on each side, found "
                 "`%s` and `%s`", op, tn(from), tn(to));
        return builtin(c, TYPE_ERROR);
    }
    /* A conversion to an interface the class implements is the implicit
       one. It needs no check, only the offset of the sub-object. */
    if (!e->as.cast.test) {
        const struct struct_field *iface =
            converts_to_interface(c, e, from, to);
        if (iface != NULL) {
            e->as.cast.operand->to_iface = iface;
            e->as.cast.target = NULL;
            return to;
        }
    }
    /* A cast from an interface pointer down to a class that implements
       it reaches the object through the offset the descriptor holds. */
    if (implemented_in(to->element, from->element)) {
        e->as.cast.from_sub = true;
    } else if (!descends_from(to->element, from->element) &&
               !descends_from(from->element, to->element)) {
        error_at(c, e->pos, "`%s` is never `%s`, the classes share no chain",
                 tn(from), tn(to));
        return builtin(c, TYPE_ERROR);
    }
    e->as.cast.target = to->element;
    return e->as.cast.test ? builtin(c, TYPE_BOOL) : to;
}

static struct type *check_cast(struct checker *c, struct expr *e)
{
    struct type *from = check_expr(c, e->as.cast.operand, NULL);
    struct type *to = resolve_type(c, e->as.cast.type);

    if (is_error(from) || is_error(to)) {
        return builtin(c, TYPE_ERROR);
    }
    if (e->as.cast.test || e->as.cast.checked ||
        (from->kind == TYPE_POINTER && from->element->kind == TYPE_CLASS &&
         to->kind == TYPE_POINTER && to->element->kind == TYPE_CLASS)) {
        return check_class_cast(c, e, from, to);
    }
    if (!can_convert(from, to)) {
        error_at(c, e->pos, "cannot convert `%s` to `%s`", tn(from), tn(to));
        return builtin(c, TYPE_ERROR);
    }
    if (type_is_float(from) && type_is_integer(to) &&
        undefined_on_constants(c, e, to)) {
        return builtin(c, TYPE_ERROR);
    }
    return to;
}

static struct symbol *function_symbol(const struct expr *callee)
{
    if ((callee->kind == EXPR_NAME || callee->kind == EXPR_FIELD) &&
        callee->symbol != NULL &&
        (callee->symbol->kind == SYMBOL_FN ||
         callee->symbol->kind == SYMBOL_EXTERN_FN)) {
        return callee->symbol;
    }
    return NULL;
}

/* The struct or class behind a value of type T or *T, or NULL. */
static struct type *struct_of(struct type *t)
{
    if (t->kind == TYPE_POINTER) {
        t = t->element;
    }
    return type_has_fields(t) ? t : NULL;
}

static const struct struct_field *find_field(const struct type *s,
                                             const struct name *name)
{
    size_t i;

    for (i = 0; i < s->field_count; i++) {
        if (same_name(&s->fields[i].name, name) &&
            !type_field_is_unit_break(&s->fields[i])) {
            return &s->fields[i];
        }
    }
    return NULL;
}

static struct expr *new_node(struct checker *c, enum expr_kind kind,
                             struct pos pos)
{
    struct expr *e = arena_alloc(c->arena, sizeof *e);
    e->kind = kind;
    e->pos = pos;
    return e;
}

static struct item *find_member(const struct type *t,
                               const struct name *name);

/* DESIGN: a class with an open function is never a complete value. It
   appears as a base and behind a pointer, and nowhere else. A local, a
   plain field, an `alloc` and a literal of it are all refused. */
static bool refuse_abstract_value(struct checker *c, struct pos pos,
                                  const char *what, const struct type *t)
{
    if (!type_has_fields(t) || !t->has_abstract) {
        return false;
    }
    error_at(c, pos, "`%s` is abstract and has no complete value, and %s "
             "needs one", tn((struct type *)t), what);
    return true;
}

/* DESIGN: anti.rt.Object declares seven public functions whose bodies
   live in the runtime. The checker builds one item per function, so
   `v.type_name()` resolves like any inherited call and lowering finds the
   runtime symbol behind it. The list is built once per session. */
static void declare_root(struct checker *c)
{
    static const struct {
        const char *name;
        int params;             /* besides self */
        enum type_kind result;
    } root[] = {
        {"type_name", 0, TYPE_STR},   {"to_text", 0, TYPE_STR},
        {"equals", 1, TYPE_BOOL},     {"hash", 0, TYPE_U64},
        {"serialize", 1, TYPE_VOID},  {"destruct", 0, TYPE_VOID},
        {"copy", 1, TYPE_VOID}
    };
    struct type *object = types_object(c->types);
    struct item **members;
    size_t i;

    if (object->member_count > 0) {
        return;
    }
    members = arena_alloc(c->arena, sizeof root / sizeof root[0] *
                                        sizeof *members);
    for (i = 0; i < sizeof root / sizeof root[0]; i++) {
        struct item *it = arena_alloc(c->arena, sizeof *it);
        struct symbol *sym = arena_alloc(c->arena, sizeof *sym);
        struct type **params = arena_alloc(c->arena, 2 * sizeof *params);
        memset(it, 0, sizeof *it);
        memset(sym, 0, sizeof *sym);
        params[0] = types_pointer(c->types, object);
        params[1] = params[0];
        it->kind = ITEM_FN;
        it->pub = true;
        it->vis = VIS_PUB;
        it->has_self = true;
        it->runtime = root[i].name;
        it->name.text = root[i].name;
        it->name.length = strlen(root[i].name);
        it->symbol = sym;
        sym->kind = SYMBOL_FN;
        sym->name = it->name;
        sym->item = it;
        sym->type = types_fn(c->types, params, (size_t)root[i].params + 1,
                             builtin(c, root[i].result));
        members[i] = it;
    }
    object->members = members;
    object->member_count = sizeof root / sizeof root[0];
}

/* The class that t inherits, or NULL. */
static const struct type *inherited(const struct type *t)
{
    return t != NULL && t->kind == TYPE_CLASS ? t->base : NULL;
}

/* DESIGN: a `use` or `inherits` field promotes the names of its type onto
   the struct that holds it. The checker rewrites `v.x` into `v.name.x`
   and `v.f(args)` into `T.f(&v.name, args)`, so nothing below the checker
   knows about promotion. The containing struct's own names win, and a
   name that two fields both provide is an error at the use. */
static const struct struct_field *promoting_field(const struct checker *c,
                                                  const struct type *s,
                                                  const struct name *name,
                                                  bool *ambiguous);

/* Whether the type t provides name as a field or as a public function,
   directly or through its own promoting fields. */
/* DESIGN: a `use` field and an interface sub-object promote the public
   members of their type and nothing else. A base promotes what the class
   itself may see, because the chain is one namespace. */
static bool provides(const struct checker *c, const struct type *t,
                     const struct name *name, bool pub_only)
{
    const struct struct_field *f;
    const struct item *m;
    bool ambiguous = false;

    if (!type_has_fields(t)) {
        return false;
    }
    f = find_field(t, name);
    if (f != NULL) {
        return !pub_only || t->kind != TYPE_CLASS || f->vis == VIS_PUB ||
               f->form != FIELD_PLAIN;
    }
    m = find_member(t, name);
    if (m != NULL && m->pub) {
        return true;
    }
    return promoting_field(c, t, name, &ambiguous) != NULL;
}

static const struct struct_field *promoting_field(const struct checker *c,
                                                  const struct type *s,
                                                  const struct name *name,
                                                  bool *ambiguous)
{
    const struct struct_field *found = NULL;
    size_t i;

    for (i = 0; s != NULL && i < s->field_count; i++) {
        const struct struct_field *f = &s->fields[i];
        if (f->form == FIELD_PLAIN ||
            !provides(c, f->type, name, f->form != FIELD_BASE)) {
            continue;
        }
        if (found != NULL) {
            *ambiguous = true;
            error_at((struct checker *)c, f->pos,
                     "`%.*s` is provided by both `%.*s` and `%.*s`",
                     (int)name->length, name->text,
                     (int)found->name.length, found->name.text,
                     (int)f->name.length, f->name.text);
            return NULL;
        }
        found = f;
    }
    return found;
}

/* Replace the base of e with `base.name`, the promoting field, so the
   next check sees the value that really holds the name. */
static void promote_base(struct checker *c, struct expr *e,
                         const struct struct_field *f)
{
    struct expr *inner = new_node(c, EXPR_FIELD, e->as.field.base->pos);

    inner->as.field.base = e->as.field.base;
    inner->as.field.name = f->name;
    inner->as.field.promoted = true;
    e->as.field.base = inner;
}

/* The class a member or field belongs to, or NULL. */
static const struct type *declaring_class(const struct item *m)
{
    return m != NULL && m->owner != NULL && m->owner->symbol != NULL
               ? m->owner->symbol->type
               : NULL;
}

/* The class whose function is being checked, or NULL outside one. */
static const struct type *checking_class(const struct checker *c)
{
    const struct item *owner = c->function != NULL ? c->function->owner : NULL;
    return owner != NULL && owner->symbol != NULL ? owner->symbol->type : NULL;
}

/* DESIGN: the four levels of the object model document. A public member
   is visible everywhere. A protected one reaches the class that declares
   it and every class below it. A private one reaches its own class
   alone. Every level is decided here and costs nothing at run time. */
static bool level_allows(const struct checker *c, enum visibility vis,
                         const struct type *declared_in, const struct type *t)
{
    const struct type *from = checking_class(c);

    if (vis == VIS_PUB) {
        return true;
    }
    if (from == NULL) {
        return false;
    }
    if (declared_in == NULL) {
        declared_in = t;
    }
    if (vis == VIS_PROTECTED) {
        return descends_from(from, declared_in);
    }
    return from == declared_in;
}

static bool member_visible(const struct checker *c, const struct type *t,
                           const struct item *m)
{
    return level_allows(c, m->vis, declaring_class(m), t);
}

/* Whether t is a class declared `singleton`. */
static bool singleton_type(const struct type *t)
{
    size_t i;

    if (t == NULL || t->kind != TYPE_CLASS) {
        return false;
    }
    for (i = 0; i < t->member_count; i++) {
        if (t->members[i]->singleton_get) {
            return true;
        }
    }
    return false;
}

/* DESIGN: every field of a struct is readable and writable everywhere,
   because a struct is the bytes C declares. A field of a class carries
   the level its declaration gave it. The base and the table pointer are
   the checker's own fields and belong to the class that holds them. */
static bool field_visible(const struct checker *c, const struct type *s,
                          const struct struct_field *f)
{
    /* DESIGN: the base, the table pointer, a `use` field and an
       interface sub-object carry no level of their own. They are the
       shape of the class rather than data it hides. The example of the
       object model document names a `use` field in a literal from
       outside the class. What they promote keeps its own level. */
    if (s->kind != TYPE_CLASS || f->form != FIELD_PLAIN) {
        return true;
    }
    /* DESIGN: a singleton is the program's one place for a value, so
       every field of it is readable wherever the class is. What a
       marker decides there is writing, not reading. */
    if (s->members != NULL && singleton_type(s)) {
        return true;
    }
    return level_allows(c, f->vis, f->home != NULL ? f->home : s, s);
}

/* DESIGN: `v.f(args)` resolves in the namespace of v's type first, and
   then in the module that declares the type. A function of the body wins
   over a free function of the same name. */
static struct symbol *method_symbol(const struct checker *c,
                                    const struct type *s,
                                    const struct name *name)
{
    const struct interface *lib;
    struct item *m = find_member(s, name);

    if (m != NULL && m->kind == ITEM_FN && m->symbol != NULL &&
        member_visible(c, s, m)) {
        return m->symbol;
    }
    if (same_name(&s->module, &c->module_name)) {
        return scope_find_local(&c->module_scope, name);
    }
    lib = find_library(c, &s->module);
    return lib != NULL ? library_item(c, lib, name) : NULL;
}

/* The import that the base of module.name refers to, or NULL when the
   base is not the name of a module. */
static const struct symbol *qualifier(const struct checker *c,
                                      const struct expr *field)
{
    const struct expr *base = field->as.field.base;
    const struct symbol *sym;

    if (base->kind != EXPR_NAME) {
        return NULL;
    }
    sym = lookup(c, &base->as.name);
    return sym != NULL && sym->kind == SYMBOL_MODULE ? sym : NULL;
}

/* Rewrite module.name into a plain name of the imported item. A callee
   may name a variadic extern fn, which has no function pointer type. */
static struct type *check_qualified(struct checker *c, struct expr *e,
                                    const struct symbol *module, bool callee)
{
    struct name base = e->as.field.base->as.name;
    struct name name = e->as.field.name;
    struct symbol *item = library_item(c, module->home, &name);

    if (item == NULL) {
        error_at(c, e->pos, "`%.*s` has no public item `%.*s`",
                 (int)base.length, base.text, (int)name.length, name.text);
        return builtin(c, TYPE_ERROR);
    }
    if (item->kind == SYMBOL_STRUCT) {
        error_at(c, e->pos, "`%.*s.%.*s` is a type, not a value",
                 (int)base.length, base.text, (int)name.length, name.text);
        return builtin(c, TYPE_ERROR);
    }
    if (!callee && item->kind == SYMBOL_EXTERN_FN && item->variadic) {
        error_at(c, e->pos, "a variadic function has no function pointer "
                 "type");
        return builtin(c, TYPE_ERROR);
    }
    e->kind = EXPR_NAME;
    e->as.name = name;
    e->symbol = item;
    return item->type;
}

/* DESIGN: an atomic field is read and written by calls alone, so that
   every access is one sequentially consistent operation. The checker
   rewrites `p.add(1)` on an atomic place into one node, which lowering
   turns into a call of the runtime. */
static const struct {
    const char *name;
    enum atomic_op op;
    int args;
    bool gives_value;
    bool gives_bool;
} atomic_ops[] = {
    {"load", ATOMIC_LOAD, 0, true, false},
    {"store", ATOMIC_STORE, 1, false, false},
    {"swap", ATOMIC_SWAP, 1, true, false},
    {"add", ATOMIC_ADD, 1, true, false},
    {"sub", ATOMIC_SUB, 1, true, false},
    {"and", ATOMIC_AND, 1, true, false},
    {"or", ATOMIC_OR, 1, true, false},
    {"compare_swap", ATOMIC_CAS, 2, false, true}
};

/* The type of the atomic field that e denotes, or NULL. */
static struct type *atomic_place(struct checker *c, struct expr *e)
{
    const struct struct_field *f;
    struct type *s;

    if (e->kind != EXPR_FIELD) {
        return NULL;
    }
    if (e->symbol != NULL && e->symbol->item != NULL &&
        e->symbol->item->is_static && e->symbol->item->atomic) {
        return e->symbol->type;
    }
    s = e->as.field.base->type != NULL ? struct_of(e->as.field.base->type)
                                       : NULL;
    if (s == NULL) {
        return NULL;
    }
    f = find_field(s, &e->as.field.name);
    return f != NULL && f->atomic ? f->type : NULL;
    (void)c;
}

/* Rewrite a call on an atomic place. Returns false when the callee is no
   such call, and reports nothing then. */
static bool atomic_call(struct checker *c, struct expr *e, struct type **out)
{
    struct expr *callee = e->as.call.callee;
    struct expr *place;
    struct type *t;
    size_t i;

    if (callee->kind != EXPR_FIELD ||
        callee->as.field.base->kind != EXPR_FIELD) {
        return false;
    }
    (void)0;
    /* DESIGN: the base is checked quietly, because this is a probe. A
       call on anything else reaches the branches below, which report
       what is wrong with it. */
    place = callee->as.field.base;
    c->atomic_place = true;
    c->quiet++;
    t = check_expr(c, place, NULL);
    c->quiet--;
    c->atomic_place = false;
    if (t == NULL || is_error(t)) {
        return false;
    }
    if (atomic_place(c, place) == NULL) {
        return false;
    }
    t = atomic_place(c, place);
    for (i = 0; i < sizeof atomic_ops / sizeof atomic_ops[0]; i++) {
        if (!name_is(&callee->as.field.name, atomic_ops[i].name)) {
            continue;
        }
        if ((int)e->as.call.arg_count != atomic_ops[i].args) {
            error_at(c, e->pos, "`%s` takes %d argument%s", atomic_ops[i].name,
                     atomic_ops[i].args, atomic_ops[i].args == 1 ? "" : "s");
            *out = builtin(c, TYPE_ERROR);
            return true;
        }
        e->as.atomic.a = atomic_ops[i].args > 0 ? e->as.call.args[0] : NULL;
        e->as.atomic.b = atomic_ops[i].args > 1 ? e->as.call.args[1] : NULL;
        e->as.atomic.op = atomic_ops[i].op;
        e->as.atomic.place = place;
        e->kind = EXPR_ATOMIC;
        if (e->as.atomic.a != NULL &&
            !require(c, e->as.atomic.a, check_expr(c, e->as.atomic.a, t), t)) {
            *out = builtin(c, TYPE_ERROR);
            return true;
        }
        if (e->as.atomic.b != NULL &&
            !require(c, e->as.atomic.b, check_expr(c, e->as.atomic.b, t), t)) {
            *out = builtin(c, TYPE_ERROR);
            return true;
        }
        *out = atomic_ops[i].gives_bool  ? builtin(c, TYPE_BOOL)
               : atomic_ops[i].gives_value ? t
                                           : builtin(c, TYPE_VOID);
        return true;
    }
    error_at(c, callee->pos, "an atomic field has no `%.*s`",
             (int)callee->as.field.name.length, callee->as.field.name.text);
    *out = builtin(c, TYPE_ERROR);
    return true;
}

/* Whether any class of the program places a sub-object of t inside
   itself. A pointer to such a class points into the middle of an
   object. The receiver of a call through it needs the offset that only
   the thunk of the concrete class knows. */
static bool implemented_in(const struct type *t, const struct type *iface)
{
    size_t i;

    for (; t != NULL; t = t->kind == TYPE_CLASS ? t->base : NULL) {
        for (i = 0; i < t->field_count; i++) {
            const struct struct_field *f = &t->fields[i];
            if (f->form == FIELD_IMPL && descends_from(f->type, iface)) {
                return true;
            }
        }
    }
    return false;
}

/* Rewrite v.f(args) into f(receiver, args). The struct T of v has no
   field f, and the module declares a function f whose first parameter is
   T or *T. Returns false after reporting an error. */
static bool method_call(struct checker *c, struct expr *call)
{
    struct expr *field = call->as.call.callee;
    struct expr *receiver = field->as.field.base;
    struct type *t = receiver->type;
    struct type *s = struct_of(t);
    struct symbol *f = method_symbol(c, s, &field->as.field.name);
    const struct item *member = s != NULL
                                    ? find_member(s, &field->as.field.name)
                                    : NULL;
    struct type *first;
    struct expr **args;
    struct expr *callee;

    if (f == NULL || (f->kind != SYMBOL_FN && f->kind != SYMBOL_EXTERN_FN) ||
        is_error(f->type) || f->type->param_count == 0 ||
        !descends_from(s, struct_of(f->type->params[0]))) {
        const struct item *hidden = find_member(s, &field->as.field.name);
        bool ambiguous = false;
        const struct struct_field *through =
            promoting_field(c, s, &field->as.field.name, &ambiguous);
        if (ambiguous) {
            return false;
        }
        if (through != NULL) {
            promote_base(c, field, through);
            field->as.field.base->type =
                check_expr(c, field->as.field.base, NULL);
            return method_call(c, call);
        }
        if (hidden != NULL && !member_visible(c, s, hidden)) {
            const struct type *owner = declaring_class(hidden);
            error_at(c, field->pos, "`%.*s` is %s `%s`",
                     (int)field->as.field.name.length,
                     field->as.field.name.text,
                     hidden->vis == VIS_PROTECTED ? "protected in"
                                                  : "private to",
                     tn(owner != NULL ? owner : s));
            return false;
        }
        error_at(c, field->pos, "`%s` has no function `%.*s`", tn(s),
                 (int)field->as.field.name.length, field->as.field.name.text);
        return false;
    }
    first = f->type->params[0];
    if (first->kind == TYPE_POINTER && type_has_fields(t)) {
        struct expr *address = new_node(c, EXPR_UNARY, receiver->pos);
        if (!is_place(receiver)) {
            error_at(c, receiver->pos, "calling `%.*s` needs a place",
                     (int)f->name.length, f->name.text);
            return false;
        }
        mark_address_taken(receiver);
        address->as.unary.op = TOKEN_AMP;
        address->as.unary.operand = receiver;
        address->type = first;
        receiver = address;
    } else if (type_has_fields(first) && t->kind == TYPE_POINTER) {
        struct expr *deref = new_node(c, EXPR_UNARY, receiver->pos);
        deref->as.unary.op = TOKEN_STAR;
        deref->as.unary.operand = receiver;
        deref->type = first;
        receiver = deref;
    }
    /* `destruct` is the one function a program never calls itself. The
       compiler chains it, so `delete` and `destroy` are the spellings. */
    if (member != NULL && member->runtime == NULL &&
        name_is(&field->as.field.name, "destruct")) {
        error_at(c, field->pos, "`destruct` is never called directly, use "
                 "`delete` or `destroy`");
        return false;
    }
    /* `construct` runs after a literal and after `alloc`. A base with
       arguments is reached through `self.super`, and nowhere else. */
    if (member != NULL && name_is(&field->as.field.name, "construct") &&
        receiver->kind != EXPR_FIELD) {
        error_at(c, field->pos, "`construct` runs after a literal and after "
                 "`alloc`, and is not called directly");
        return false;
    }
    /* DESIGN: a call on a class reached through a pointer goes through
       the table. The object may be of a class below the static type. A
       `final` function and a `final` class have no class below them, so
       both call directly. A value has its concrete type, and a private
       function has no entry, so both call directly as well.

       DESIGN: a call is never made direct because no class replaces the
       function. The checker reads one module, and a class of a module
       that imports this one replaces what this one cannot see. Such a
       direct call is a miscompile, so `final` is the only way to one. */
    if (t->kind == TYPE_POINTER && s->kind == TYPE_CLASS &&
        member != NULL && member->pub && !member->is_final && !s->is_final) {
        call->as.call.dispatch = s;
        call->as.call.entry = field->as.field.name;
    }
    callee = new_node(c, EXPR_NAME, field->pos);
    callee->as.name = f->name;
    callee->symbol = f;
    callee->type = f->type;
    args = arena_alloc(c->arena, (call->as.call.arg_count + 1) * sizeof *args);
    args[0] = receiver;
    memcpy(args + 1, call->as.call.args,
           call->as.call.arg_count * sizeof *args);
    call->as.call.callee = callee;
    call->as.call.args = args;
    call->as.call.arg_count++;
    return true;
}

static bool variadic_ok(const struct type *t)
{
    switch (t->kind) {
    case TYPE_I32:
    case TYPE_U32:
    case TYPE_I64:
    case TYPE_U64:
    case TYPE_F64:
    case TYPE_POINTER:
        return true;
    default:
        return false;
    }
}

static struct type *check_type_member(struct checker *c, struct expr *e,
                                      struct type *t);
static bool const_symbol(struct checker *c, struct symbol *sym,
                         struct pos use);

/* DESIGN: a function that can fail returns a pointer to `anti.error`'s
   `Error` or to a class below it. The compiler knows the convention by
   the module path and the class name, and nothing else of the standard
   library reaches the checker. */
static bool is_failing(const struct checker *c, const struct type *t)
{
    (void)c;
    if (t == NULL || t->kind != TYPE_POINTER ||
        t->element->kind != TYPE_CLASS) {
        return false;
    }
    for (t = t->element; t != NULL; t = t->base) {
        if (name_is(&t->name, "Error") && t->module.length == 10 &&
            memcmp(t->module.text, "anti.error", 10) == 0) {
            return true;
        }
    }
    return false;
}

/* DESIGN: the error a `catch` binds belongs to the handler, which ends
   it. A handler that wants to keep it copies it with `dup`, so no error
   outlives the block that owns it. */
static void refuse_escaping_error(struct checker *c, const struct expr *e)
{
    if (e != NULL && e->kind == EXPR_NAME && e->symbol != NULL &&
        e->symbol->caught) {
        error_at(c, e->pos, "`%.*s` outlives its `catch`, use `dup`",
                 (int)e->as.name.length, e->as.name.text);
    }
}

/* Whether the block hands the error `sym` to its caller with `return`.
   The handler then keeps its hands off it, because the caller owns it. */
static bool returns_error(const struct block *b, const struct symbol *sym);

static bool stmt_returns_error(const struct stmt *s, const struct symbol *sym)
{
    size_t i;

    if (s == NULL || sym == NULL) {
        return false;
    }
    switch (s->kind) {
    case STMT_RETURN:
        return s->as.return_value != NULL &&
               s->as.return_value->kind == EXPR_NAME &&
               s->as.return_value->symbol == sym;
    case STMT_BLOCK:
        return returns_error(s->as.block, sym);
    case STMT_IF:
        for (i = 0; i < s->as.if_chain.count; i++) {
            if (returns_error(s->as.if_chain.branches[i].body, sym)) {
                return true;
            }
        }
        return s->as.if_chain.else_body != NULL &&
               returns_error(s->as.if_chain.else_body, sym);
    case STMT_WHILE:
    case STMT_DO_WHILE:
        return returns_error(s->as.loop.body, sym);
    case STMT_FOR:
        return returns_error(s->as.for_loop.body, sym);
    default:
        return false;
    }
}

static bool returns_error(const struct block *b, const struct symbol *sym)
{
    size_t i;

    for (i = 0; b != NULL && i < b->count; i++) {
        if (stmt_returns_error(b->stmts[i], sym)) {
            return true;
        }
    }
    return false;
}

/* DESIGN: a static function of the error class itself builds an error
   rather than reporting one, so `Error.new` gives a value like any other
   call. Every other function that returns `*Error` reports a failure. */
static bool makes_error(const struct type *owner)
{
    for (; owner != NULL; owner = owner->base) {
        if (name_is(&owner->name, "Error") && owner->module.length == 10 &&
            memcmp(owner->module.text, "anti.error", 10) == 0) {
            return true;
        }
    }
    return false;
}

/* The value a handled call gives: what the out parameter points at, or
   nothing when the function writes no result. */
static struct type *handled_result(struct checker *c, const struct expr *e,
                                   const struct type *fn)
{
    if (e->as.call.builds != NULL) {
        return e->as.call.on_heap
                   ? types_pointer(c->types, (struct type *)e->as.call.builds)
                   : (struct type *)e->as.call.builds;
    }
    return e->as.call.out != NULL
               ? fn->params[fn->param_count - 1]->element
               : builtin(c, TYPE_VOID);
}

/* Check the handler of a call that can fail. Every such call carries
   one, because an error that nothing reads is a bug the checker can
   see. */
static struct type *check_handled(struct checker *c, struct expr *e,
                                  struct type *fn)
{
    struct handler *h = &e->as.call.handler;
    struct type *result = handled_result(c, e, fn);
    const struct symbol *callee = function_symbol(e->as.call.callee);
    struct scope scope;
    struct type *outer_yield = c->yields;
    int outer_depth = c->handler_depth;

    /* Inside a `try` block every failing call reaches the one handler,
       so it needs none of its own. */
    if (h->kind == HANDLE_NONE && c->try_block != NULL) {
        h->kind = HANDLE_ENCLOSING;
        if (c->error_type == NULL) {
            c->error_type = fn->result;
        }
    }
    switch (h->kind) {
    case HANDLE_ENCLOSING:
        return result;
    case HANDLE_NONE:
        if (callee != NULL) {
            error_at(c, e->pos, "the error of `%.*s` is not handled",
                     (int)callee->name.length, callee->name.text);
        } else {
            error_at(c, e->pos, "the error of this call is not handled");
        }
        return builtin(c, TYPE_ERROR);
    case HANDLE_TRY:
        if (c->function == NULL || !is_failing(c, c->function->symbol == NULL
                                                      ? NULL
                                                      : c->function->symbol
                                                            ->type->result)) {
            error_at(c, h->pos, "`try` stands in a function that returns "
                     "`*Error`");
            return builtin(c, TYPE_ERROR);
        }
        return result;
    case HANDLE_FATAL:
        return result;
    case HANDLE_BLOCK:
        enter_scope(c, &scope);
        if (h->name.length > 0) {
            h->symbol = declare(c, SYMBOL_LOCAL, &h->name, h->pos,
                                "`%.*s` is already declared in this block");
            if (h->symbol != NULL) {
                h->symbol->type = fn->result;
                h->symbol->read_only = true;
            }
        }
        if (h->symbol != NULL) {
            h->symbol->caught = true;
        }
        c->yields = result;
        c->handler_depth++;
        check_block(c, h->body);
        c->handler_depth = outer_depth;
        c->yields = outer_yield;
        h->passes = returns_error(h->body, h->symbol);
        leave_scope(c, &scope);
        return result;
    }
    return result;
}

/* DESIGN: `Circle(2.0)` builds a value and runs the `construct` of the
   class with those arguments. The defaults are written first, so the
   body sees a complete object. A `construct` that can fail makes the
   whole expression a failing call, which needs a handler like any
   other. */
static struct type *check_construct(struct checker *c, struct expr *e,
                                    struct type *t)
{
    static const struct name construct_name = {"construct", 9};
    struct item *m = find_member(t, &construct_name);
    struct type *fn;
    size_t i;
    bool ok = true;

    if (m == NULL || m->kind != ITEM_FN || m->symbol == NULL ||
        m->param_count == 0) {
        error_at(c, e->pos, "`%s` has no `construct` with arguments, so a "
                 "literal builds it", tn(t));
        return builtin(c, TYPE_ERROR);
    }
    if (refuse_abstract_value(c, e->pos, "a value", t)) {
        return builtin(c, TYPE_ERROR);
    }
    fn = m->symbol->type;
    e->as.call.builds = t;
    e->as.call.callee->symbol = m->symbol;
    e->as.call.callee->type = fn;
    if (e->as.call.arg_count + 1 != fn->param_count) {
        error_at(c, e->pos, "`%s.construct` takes %zu argument%s, found %zu",
                 tn(t), fn->param_count - 1,
                 fn->param_count == 2 ? "" : "s", e->as.call.arg_count);
        return builtin(c, TYPE_ERROR);
    }
    for (i = 0; i < e->as.call.arg_count; i++) {
        struct expr *arg = e->as.call.args[i];
        ok = require(c, arg, check_expr(c, arg, fn->params[i + 1]),
                     fn->params[i + 1]) && ok;
    }
    if (!ok) {
        return builtin(c, TYPE_ERROR);
    }
    if (is_failing(c, fn->result)) {
        return check_handled(c, e, fn);
    }
    return handled_result(c, e, fn);
}

static struct type *check_call(struct checker *c, struct expr *e)
{
    struct expr *callee = e->as.call.callee;
    struct type *fn;
    struct symbol *sym;
    bool variadic = false;
    size_t fixed;
    size_t i;
    bool ok = true;
    const struct symbol *module;
    const struct type *owner_type = NULL;

    /* An operation on an atomic field becomes one node of its own. */
    if (atomic_call(c, e, &fn)) {
        return fn;
    }
    if (callee->kind == EXPR_FIELD && (module = qualifier(c, callee)) != NULL) {
        fn = check_qualified(c, callee, module, true);
        callee->type = fn;
        if (!is_error(fn) && callee->symbol->kind == SYMBOL_EXTERN_FN) {
            variadic = callee->symbol->variadic;
        }
        fixed = 0;
    } else if (callee->kind == EXPR_FIELD &&
               callee->as.field.base->kind == EXPR_NAME &&
               (sym = (struct symbol *)lookup(c,
                        &callee->as.field.base->as.name)) != NULL &&
               sym->kind == SYMBOL_STRUCT) {
        /* T.f(args) calls a function of the body of T, which takes no
           self. An enum value is not callable. */
        owner_type = sym->type;
        fn = check_type_member(c, callee, sym->type);
        callee->type = fn;
        if (is_error(fn)) {
            return fn;
        }
        if (fn->kind != TYPE_FN) {
            error_at(c, callee->pos, "`%s` is not a function", tn(fn));
            return builtin(c, TYPE_ERROR);
        }
        fixed = 0;
    } else if (callee->kind == EXPR_FIELD &&
               callee->as.field.base->kind == EXPR_FIELD &&
               (module = qualifier(c, callee->as.field.base)) != NULL &&
               (sym = library_item(c, module->home,
                        &callee->as.field.base->as.field.name)) != NULL &&
               sym->kind == SYMBOL_STRUCT) {
        /* `m.T.f(args)` calls a function of the body of a type of
           another module, which takes no self. */
        owner_type = sym->type;
        fn = check_type_member(c, callee, sym->type);
        callee->type = fn;
        if (is_error(fn)) {
            return fn;
        }
        if (fn->kind != TYPE_FN) {
            error_at(c, callee->pos, "`%s` is not a function", tn(fn));
            return builtin(c, TYPE_ERROR);
        }
        fixed = 0;
    } else if (callee->kind == EXPR_FIELD) {
        struct type *base = check_expr(c, callee->as.field.base, NULL);
        struct type *s;
        if (is_error(base)) {
            return base;
        }
        s = struct_of(base);
        /* DESIGN: a union has no methods, so v.f(args) on a union is
           always a call of the function pointer in field f. */
        if (s != NULL && !s->is_union &&
            find_field(s, &callee->as.field.name) == NULL) {
            if (!method_call(c, e)) {
                return builtin(c, TYPE_ERROR);
            }
            callee = e->as.call.callee;
            fn = callee->type;
            fixed = 1;
        } else {
            fn = check_expr(c, callee, NULL);
            fixed = 0;
        }
    } else if (callee->kind == EXPR_NAME) {
        sym = lookup(c, &callee->as.name);
        callee->symbol = sym;
        /* A class name in the place of a function builds a value. */
        if (sym != NULL && sym->kind == SYMBOL_STRUCT &&
            sym->type != NULL && sym->type->kind == TYPE_CLASS) {
            return check_construct(c, e, sym->type);
        }
        if (sym != NULL && sym->kind == SYMBOL_EXTERN_FN) {
            fn = sym->type;
            callee->type = fn;
            variadic = sym->variadic;
        } else {
            fn = check_expr(c, callee, NULL);
        }
        fixed = 0;
    } else {
        fn = check_expr(c, callee, NULL);
        fixed = 0;
    }
    if (is_error(fn)) {
        return builtin(c, TYPE_ERROR);
    }
    if (fn->kind != TYPE_FN) {
        error_at(c, e->pos, "cannot call `%s`", tn(fn));
        return builtin(c, TYPE_ERROR);
    }
    sym = function_symbol(callee);
    /* DESIGN: a function that can fail returns `*Error` and writes its
       result through the last parameter. A call that gives one argument
       fewer than the function takes leaves that place to the compiler.
       The compiler passes the address of what the `let` declares. */
    if (is_failing(c, fn->result) && !variadic &&
        e->as.call.arg_count + 1 == fn->param_count &&
        fn->params[fn->param_count - 1]->kind == TYPE_POINTER) {
        e->as.call.out = e;
    }
    if (e->as.call.out != NULL) {
        /* The out parameter is not written at the call, so the count of
           arguments the program gave is one less. */
    } else if (variadic ? e->as.call.arg_count < fn->param_count
                 : e->as.call.arg_count != fn->param_count) {
        size_t n = fn->param_count - fixed;
        if (sym != NULL) {
            error_at(c, e->pos, "`%.*s` takes %s%zu argument%s, found %zu",
                     (int)sym->name.length, sym->name.text,
                     variadic ? "at least " : "", n, n == 1 ? "" : "s",
                     e->as.call.arg_count - fixed);
        } else {
            error_at(c, e->pos, "the call takes %zu argument%s, found %zu", n,
                     n == 1 ? "" : "s", e->as.call.arg_count - fixed);
        }
        return builtin(c, TYPE_ERROR);
    }
    for (i = fixed; i < e->as.call.arg_count; i++) {
        struct expr *arg = e->as.call.args[i];
        if (i < fn->param_count) {
            ok = require(c, arg, check_expr(c, arg, fn->params[i]),
                         fn->params[i]) && ok;
        } else {
            struct type *t = check_expr(c, arg, NULL);
            if (!is_error(t) && !variadic_ok(t)) {
                error_at(c, arg->pos, "a variadic argument has type i32, u32, "
                         "int, u64, float or a pointer, found `%s`", tn(t));
                ok = false;
            }
        }
    }
    if (!ok) {
        return builtin(c, TYPE_ERROR);
    }
    if (is_failing(c, fn->result) && !makes_error(owner_type)) {
        return check_handled(c, e, fn);
    }
    if (e->as.call.handler.kind != HANDLE_NONE) {
        error_at(c, e->as.call.handler.pos,
                 "this call cannot fail, so it has no error to handle");
        return builtin(c, TYPE_ERROR);
    }
    return fn->result;
}

/* DESIGN: the function or constant that the body of t declares under
   name, or the one a class above it declares. A class inherits the
   namespace of its base, so a call on a derived pointer finds the
   inherited function and dispatches from the derived class. */
static struct item *find_member(const struct type *t, const struct name *name)
{
    size_t i;

    for (; t != NULL; t = t->kind == TYPE_CLASS ? t->base : NULL) {
        for (i = 0; i < t->member_count; i++) {
            if (same_name(&t->members[i]->name, name)) {
                return t->members[i];
            }
        }
    }
    return NULL;
}

/* A type name on the left of a dot reaches the namespace of that type.
   It names a value of an enum, a constant of a body, or a function.
   `T.f(&v, args)` calls a function with `self` written out. */
static struct type *check_type_member(struct checker *c, struct expr *e,
                                      struct type *t)
{
    struct name *name = &e->as.field.name;
    struct item *m = find_member(t, name);
    const struct struct_field *f;

    if (t->kind == TYPE_ENUM && (f = find_field(t, name)) != NULL) {
        e->as.field.enum_value = (uint32_t)(f - t->fields) + 1;
        return t;
    }
    if (m == NULL) {
        error_at(c, e->pos, "`%s` has no function `%.*s`", tn(t),
                 (int)name->length, name->text);
        return builtin(c, TYPE_ERROR);
    }
    if (m->symbol == NULL) {
        return builtin(c, TYPE_ERROR);
    }
    /* DESIGN: a static field is a global of the class and not a
       constant. Its value is written into the data of the module and
       never folded into a use. The value is still evaluated here,
       because a global holds its bytes before the program runs. */
    if (m->kind == ITEM_CONST && !const_symbol(c, m->symbol, e->pos)) {
        return builtin(c, TYPE_ERROR);
    }
    if (m->is_static && m->atomic && !c->atomic_place) {
        error_at(c, e->pos, "`%.*s` is atomic, so it is read with `load()` "
                 "and written with `store(v)`", (int)name->length,
                 name->text);
        return builtin(c, TYPE_ERROR);
    }
    e->symbol = m->symbol;
    return m->symbol->type != NULL ? m->symbol->type
                                   : builtin(c, TYPE_ERROR);
}

static struct type *check_field(struct checker *c, struct expr *e)
{
    const struct symbol *module = qualifier(c, e);
    struct type *base;
    struct name *name = &e->as.field.name;
    struct type *s;
    const struct struct_field *f;

    if (module != NULL) {
        return check_qualified(c, e, module, false);
    }
    if (e->as.field.base->kind == EXPR_NAME) {
        const struct symbol *sym = lookup(c, &e->as.field.base->as.name);
        if (sym != NULL && sym->kind == SYMBOL_STRUCT) {
            return check_type_member(c, e, sym->type);
        }
    }
    /* A type of another module reaches its namespace as well, so
       `m.Kind.Round` and `m.T.f` read like the unqualified forms. */
    if (e->as.field.base->kind == EXPR_FIELD) {
        const struct symbol *outer = qualifier(c, e->as.field.base);
        struct symbol *sym =
            outer != NULL
                ? library_item(c, outer->home, &e->as.field.base->as.field.name)
                : NULL;
        if (sym != NULL && sym->kind == SYMBOL_STRUCT) {
            return check_type_member(c, e, sym->type);
        }
    }
    base = check_expr(c, e->as.field.base, NULL);
    if (is_error(base)) {
        return base;
    }
    if ((s = struct_of(base)) != NULL) {
        if ((f = find_field(s, name)) == NULL) {
            const struct item *m = find_member(s, name);
            bool ambiguous = false;
            const struct struct_field *through =
                promoting_field(c, s, name, &ambiguous);
            if (ambiguous) {
                return builtin(c, TYPE_ERROR);
            }
            if (through != NULL) {
                promote_base(c, e, through);
                return check_field(c, e);
            }
            /* DESIGN: a public function named on a value and not
               called is a bound function. It holds the object and the
               entry of its table. Its type is the signature without
               `self`, and calling it needs no receiver. */
            if (m != NULL && m->kind == ITEM_FN && m->pub &&
                m->symbol != NULL && m->symbol->type != NULL &&
                m->symbol->type->kind == TYPE_FN &&
                m->symbol->type->param_count > 0) {
                struct type *fn = m->symbol->type;
                e->symbol = m->symbol;
                return types_bound_fn(c->types, fn->params + 1,
                                      fn->param_count - 1, fn->result);
            }
            /* DESIGN: a constant of a body is reached as `T.N`, never
               through a value, so a constant is never mistaken for a
               field. */
            if (m != NULL && m->kind == ITEM_CONST) {
                error_at(c, e->pos, "`%.*s` is a constant of `%s`, reached as "
                         "`%s.%.*s`", (int)name->length, name->text, tn(s),
                         tn(s), (int)name->length, name->text);
                return builtin(c, TYPE_ERROR);
            }
            error_at(c, e->pos, "`%s` has no field `%.*s`", tn(s),
                     (int)name->length, name->text);
            return builtin(c, TYPE_ERROR);
        }
        /* A field the checker wrote to reach a base or a promoted name
           carries no level of its own. */
        if (!e->as.field.promoted && !field_visible(c, s, f)) {
            error_at(c, e->pos, "`%.*s` is %s `%s`", (int)name->length,
                     name->text,
                     f->vis == VIS_PROTECTED ? "protected in" : "private to",
                     tn(s));
            return builtin(c, TYPE_ERROR);
        }
        /* DESIGN: an atomic field is read and written by its own calls
           alone, so that every access is one operation of the memory
           model. A mention of it anywhere else is refused. */
        if (f->atomic && !c->atomic_place) {
            error_at(c, e->pos, "`%.*s` is atomic, so it is read with "
                     "`load()` and written with `store(v)`",
                     (int)name->length, name->text);
            return builtin(c, TYPE_ERROR);
        }
        return f->type;
    }
    if (name_is(name, "len") && (base->kind == TYPE_STR ||
                                 base->kind == TYPE_SLICE ||
                                 base->kind == TYPE_ARRAY)) {
        return builtin(c, TYPE_I64);
    }
    if (name_is(name, "ptr") && base->kind == TYPE_STR) {
        return types_pointer(c->types, builtin(c, TYPE_U8));
    }
    if (name_is(name, "ptr") && base->kind == TYPE_SLICE) {
        return types_pointer(c->types, base->element);
    }
    error_at(c, e->pos, "`%s` has no field `%.*s`", tn(base),
             (int)name->length, name->text);
    return builtin(c, TYPE_ERROR);
}

/* The fields of a struct or slice literal against the fields of type s.
   Every field appears exactly once, and a union literal names one field,
   which skip_missing allows. */
/* DESIGN: a class literal names the fields of the whole chain directly,
   in any order, and never writes the base as a nested value. The checker
   flattens the chain into one list, base first, and checks the literal
   against it. The base and the table pointer are left out, because no
   literal names them and lowering writes them. */
static size_t chain_fields(const struct type *t, struct struct_field *out)
{
    size_t count = 0;
    size_t i;

    if (t == NULL) {
        return 0;
    }
    if (t->kind == TYPE_CLASS) {
        count = chain_fields(t->base, out);
    }
    for (i = 0; i < t->field_count; i++) {
        if (t->fields[i].form == FIELD_BASE ||
            t->fields[i].form == FIELD_TABLE) {
            continue;
        }
        if (out != NULL) {
            out[count] = t->fields[i];
        }
        count++;
    }
    return count;
}

/* DESIGN: a literal outside the class names its public fields alone. A
   private or protected field then takes its default, so the value is
   still complete. A literal inside the class may name any field. */
static bool check_field_inits(struct checker *c, struct expr *e,
                              struct field_init *inits, size_t count,
                              const struct struct_field *fields,
                              size_t field_count, const char *type_name,
                              bool skip_missing)
{
    bool ok = true;
    size_t i;
    size_t j;

    for (i = 0; i < count; i++) {
        const struct struct_field *f = NULL;
        for (j = 0; j < field_count; j++) {
            if (same_name(&fields[j].name, &inits[i].name) &&
                !type_field_is_unit_break(&fields[j])) {
                f = &fields[j];
            }
        }
        if (f == NULL) {
            error_at(c, inits[i].pos, "`%s` has no field `%.*s`", type_name,
                     (int)inits[i].name.length, inits[i].name.text);
            ok = false;
            continue;
        }
        for (j = 0; j < i; j++) {
            if (same_name(&inits[j].name, &inits[i].name)) {
                error_at(c, inits[i].pos, "the field `%.*s` appears twice",
                         (int)inits[i].name.length, inits[i].name.text);
                ok = false;
            }
        }
        if (f->home != NULL && f->home->kind == TYPE_CLASS &&
            !field_visible(c, f->home, f)) {
            error_at(c, inits[i].pos, "`%.*s` is %s `%s`",
                     (int)inits[i].name.length, inits[i].name.text,
                     f->vis == VIS_PROTECTED ? "protected in" : "private to",
                     tn(f->home));
            ok = false;
        }
        ok = require(c, inits[i].value,
                     check_expr(c, inits[i].value, f->type), f->type) && ok;
    }
    if (!ok || skip_missing) {
        return ok;
    }
    for (j = 0; j < field_count; j++) {
        /* An interface sub-object is the compiler's field. It carries a
           table pointer that the literal never writes, so a literal that
           leaves it out is complete. */
        if (type_field_is_unit_break(&fields[j]) ||
            fields[j].form == FIELD_IMPL) {
            continue;
        }
        for (i = 0; i < count; i++) {
            if (same_name(&fields[j].name, &inits[i].name)) {
                break;
            }
        }
        /* DESIGN: a field with a default may be left out of a literal.
           The lowering writes the default in its place, so the value is
           complete and the layout is unchanged. */
        if (i == count && fields[j].value == NULL) {
            error_at(c, e->pos, "the literal of `%s` misses the field `%.*s`",
                     type_name, (int)fields[j].name.length, fields[j].name.text);
            return false;
        }
    }
    return true;
}

/* DESIGN: the safety of `parallel` rests on the types. The runtime does
   the splitting, so a worker reaches one contiguous slice of the array
   and nothing else. Its parameters and its result are pointer-free, so
   no worker can reach memory that another one writes. The rule is no
   data race, not memory safety. */
static bool worker_type(struct checker *c, struct pos pos, const char *what,
                        struct type *t)
{
    if (is_error(t)) {
        return false;
    }
    if (!type_pointer_free(t)) {
        error_at(c, pos, "%s has type `%s`, which holds a pointer. A worker "
                 "takes and returns values alone", what, tn(t));
        return false;
    }
    return true;
}

/* Check `parallel a by n -> f(x)`. It splits a into n chunks and runs f
   on each through the worker pool. The results come back in chunk
   order. */
static struct type *check_parallel(struct checker *c, struct expr *e)
{
    struct expr *call = e->as.parallel.call;
    struct expr *callee = call->kind == EXPR_CALL ? call->as.call.callee : call;
    struct expr **args = call->kind == EXPR_CALL ? call->as.call.args : NULL;
    size_t arg_count = call->kind == EXPR_CALL ? call->as.call.arg_count : 0;
    struct type *array = check_expr(c, e->as.parallel.array, NULL);
    struct symbol *sym;
    struct type *fn;
    size_t i;
    bool ok = true;

    if (e->as.parallel.chunks != NULL) {
        struct expr *n = e->as.parallel.chunks;
        ok = require(c, n, check_expr(c, n, builtin(c, TYPE_I64)),
                     builtin(c, TYPE_I64));
    }
    if (is_error(array)) {
        return builtin(c, TYPE_ERROR);
    }
    if (array->kind != TYPE_SLICE) {
        error_at(c, e->as.parallel.array->pos,
                 "`parallel` splits a slice, and this is `%s`", tn(array));
        return builtin(c, TYPE_ERROR);
    }
    if (callee->kind != EXPR_NAME) {
        error_at(c, callee->pos, "`parallel` runs a worker named here");
        return builtin(c, TYPE_ERROR);
    }
    sym = lookup(c, &callee->as.name);
    callee->symbol = sym;
    fn = check_expr(c, callee, NULL);
    if (is_error(fn)) {
        return builtin(c, TYPE_ERROR);
    }
    if (fn->kind != TYPE_FN || sym == NULL || !sym->worker) {
        error_at(c, callee->pos, "`%.*s` is not a `worker fn`",
                 (int)callee->as.name.length, callee->as.name.text);
        return builtin(c, TYPE_ERROR);
    }
    if (fn->param_count != arg_count + 1) {
        error_at(c, call->pos, "`%.*s` takes %zu argument%s beside the chunk, "
                 "found %zu", (int)callee->as.name.length,
                 callee->as.name.text, fn->param_count - 1,
                 fn->param_count == 2 ? "" : "s", arg_count);
        return builtin(c, TYPE_ERROR);
    }
    if (fn->params[0]->kind != TYPE_SLICE ||
        fn->params[0]->element != array->element) {
        error_at(c, callee->pos, "`%.*s` takes `%s` as its chunk, and the "
                 "slice is `%s`", (int)callee->as.name.length,
                 callee->as.name.text, tn(fn->params[0]), tn(array));
        return builtin(c, TYPE_ERROR);
    }
    ok = worker_type(c, callee->pos, "the chunk", array->element) && ok;
    ok = worker_type(c, callee->pos, "the result of a worker", fn->result) &&
         ok;
    for (i = 0; i < arg_count; i++) {
        ok = require(c, args[i], check_expr(c, args[i], fn->params[i + 1]),
                     fn->params[i + 1]) && ok;
        ok = worker_type(c, args[i]->pos, "an argument of a worker",
                         fn->params[i + 1]) && ok;
    }
    if (call->kind == EXPR_CALL) {
        call->type = fn->result;
    }
    return ok ? types_slice(c->types, fn->result) : builtin(c, TYPE_ERROR);
}

/* DESIGN: `dispatch obj -> f(args)` gives one object to the pool for a
   `worker fn` whose first parameter is a pointer to the object's class.
   The other arguments follow the pointer-free rule of `parallel`, and the
   expression gives a Job of the worker's result. */
static struct type *check_dispatch(struct checker *c, struct expr *e)
{
    struct expr *call = e->as.dispatch.call;
    struct expr *callee = call->kind == EXPR_CALL ? call->as.call.callee : call;
    struct expr **args = call->kind == EXPR_CALL ? call->as.call.args : NULL;
    size_t arg_count = call->kind == EXPR_CALL ? call->as.call.arg_count : 0;
    struct type *object = check_expr(c, e->as.dispatch.object, NULL);
    struct symbol *sym;
    struct type *fn;
    size_t i;
    bool ok = true;

    if (is_error(object)) {
        return builtin(c, TYPE_ERROR);
    }
    if (object->kind != TYPE_POINTER || object->element->kind != TYPE_CLASS) {
        error_at(c, e->as.dispatch.object->pos,
                 "`dispatch` submits a class pointer, and this is `%s`",
                 tn(object));
        return builtin(c, TYPE_ERROR);
    }
    if (callee->kind != EXPR_NAME) {
        error_at(c, callee->pos, "`dispatch` runs a worker named here");
        return builtin(c, TYPE_ERROR);
    }
    sym = lookup(c, &callee->as.name);
    callee->symbol = sym;
    fn = check_expr(c, callee, NULL);
    if (is_error(fn)) {
        return builtin(c, TYPE_ERROR);
    }
    if (fn->kind != TYPE_FN || sym == NULL || !sym->worker) {
        error_at(c, callee->pos, "`%.*s` is not a `worker fn`",
                 (int)callee->as.name.length, callee->as.name.text);
        return builtin(c, TYPE_ERROR);
    }
    if (fn->param_count != arg_count + 1) {
        error_at(c, call->pos, "`%.*s` takes %zu argument%s beside the object, "
                 "found %zu", (int)callee->as.name.length,
                 callee->as.name.text, fn->param_count - 1,
                 fn->param_count == 2 ? "" : "s", arg_count);
        return builtin(c, TYPE_ERROR);
    }
    if (fn->params[0] != object) {
        error_at(c, callee->pos, "`%.*s` takes `%s` as its object, and this is "
                 "`%s`", (int)callee->as.name.length, callee->as.name.text,
                 tn(fn->params[0]), tn(object));
        return builtin(c, TYPE_ERROR);
    }
    ok = worker_type(c, callee->pos, "the result of a worker", fn->result) &&
         ok;
    for (i = 0; i < arg_count; i++) {
        ok = require(c, args[i], check_expr(c, args[i], fn->params[i + 1]),
                     fn->params[i + 1]) && ok;
        ok = worker_type(c, args[i]->pos, "an argument of a worker",
                         fn->params[i + 1]) && ok;
    }
    if (call->kind == EXPR_CALL) {
        call->type = fn->result;
    }
    return ok ? types_job(c->types, fn->result) : builtin(c, TYPE_ERROR);
}

/* `join(job)` waits for one job and gives its result. `join_all(jobs)`
   waits for a slice of jobs and gives nothing. */
static struct type *check_join(struct checker *c, struct expr *e)
{
    struct type *t = check_expr(c, e->as.join.job, NULL);
    const char *what = e->as.join.all ? "join_all" : "join";
    struct type *job = e->as.join.all && t->kind == TYPE_SLICE ? t->element : t;

    if (is_error(t)) {
        return t;
    }
    if (e->as.join.all && t->kind != TYPE_SLICE) {
        error_at(c, e->as.join.job->pos, "`join_all` waits for a slice of "
                 "jobs, and this is `%s`", tn(t));
        return builtin(c, TYPE_ERROR);
    }
    if (job->kind != TYPE_STRUCT || job->result == NULL ||
        !name_is(&job->name, "Job")) {
        error_at(c, e->as.join.job->pos, "`%s` waits for a job of `dispatch`, "
                 "and this is `%s`", what, tn(t));
        return builtin(c, TYPE_ERROR);
    }
    return e->as.join.all ? builtin(c, TYPE_VOID) : job->result;
}

static struct type *check_expr_inner(struct checker *c, struct expr *e,
                                     struct type *expected)
{
    struct type *t;
    struct symbol *sym;
    size_t i;

    switch (e->kind) {
    case EXPR_INT:
        return integer_literal(c, e, e, false, expected);
    case EXPR_FLOAT:
        return float_literal(c, e, e, false, expected);
    case EXPR_CHAR:
        return builtin(c, TYPE_CHAR);
    case EXPR_STRING:
        return builtin(c, TYPE_STR);
    case EXPR_BYTES:
        return types_slice(c->types, builtin(c, TYPE_U8));
    case EXPR_BOOL:
        return builtin(c, TYPE_BOOL);
    case EXPR_NULL:
        if (expected != NULL && (expected->kind == TYPE_POINTER ||
                                 expected->kind == TYPE_FN)) {
            return expected;
        }
        if (expected == NULL || !is_error(expected)) {
            error_at(c, e->pos, "`null` needs a pointer type from its context");
        }
        return builtin(c, TYPE_ERROR);
    case EXPR_NAME:
        sym = lookup(c, &e->as.name);
        e->symbol = sym;
        if (sym == NULL) {
            error_at(c, e->pos, "unknown name `%.*s`", (int)e->as.name.length,
                     e->as.name.text);
            return builtin(c, TYPE_ERROR);
        }
        if (sym->kind == SYMBOL_STRUCT) {
            error_at(c, e->pos, "`%.*s` is a type, not a value",
                     (int)e->as.name.length, e->as.name.text);
            return builtin(c, TYPE_ERROR);
        }
        if (sym->kind == SYMBOL_MODULE) {
            error_at(c, e->pos, "`%.*s` is a module, not a value",
                     (int)e->as.name.length, e->as.name.text);
            return builtin(c, TYPE_ERROR);
        }
        if (sym->kind == SYMBOL_EXTERN_FN && sym->variadic) {
            error_at(c, e->pos, "a variadic function has no function pointer "
                     "type");
            return builtin(c, TYPE_ERROR);
        }
        if (sym->kind == SYMBOL_CONST && sym->type == NULL) {
            struct const_value v;
            if (!eval_const(c, e, &v)) {
                return builtin(c, TYPE_ERROR);
            }
        }
        return sym->type != NULL ? sym->type : builtin(c, TYPE_ERROR);
    case EXPR_UNARY:
        return check_unary(c, e, expected);
    case EXPR_BINARY:
        return check_binary(c, e, expected);
    case EXPR_CAST:
        return check_cast(c, e);
    case EXPR_CALL:
        return check_call(c, e);
    case EXPR_PARALLEL:
        return check_parallel(c, e);
    case EXPR_DISPATCH:
        return check_dispatch(c, e);
    case EXPR_JOIN:
        return check_join(c, e);
    case EXPR_INDEX:
        t = check_expr(c, e->as.index.base, NULL);
        if (!require(c, e->as.index.index,
                     check_expr(c, e->as.index.index, builtin(c, TYPE_I64)),
                     builtin(c, TYPE_I64)) || is_error(t)) {
            return builtin(c, TYPE_ERROR);
        }
        switch (t->kind) {
        case TYPE_ARRAY:
        case TYPE_SLICE:
        case TYPE_POINTER:
            return t->element;
        case TYPE_STR:
            return builtin(c, TYPE_U8);
        default:
            error_at(c, e->pos, "cannot index `%s`", tn(t));
            return builtin(c, TYPE_ERROR);
        }
    case EXPR_SLICE: {
        struct type *index = builtin(c, TYPE_I64);
        bool ok;
        t = check_expr(c, e->as.slice.base, NULL);
        ok = require(c, e->as.slice.low,
                     check_expr(c, e->as.slice.low, index), index);
        ok = require(c, e->as.slice.high,
                     check_expr(c, e->as.slice.high, index), index) && ok;
        if (!ok || is_error(t)) {
            return builtin(c, TYPE_ERROR);
        }
        if (t->kind == TYPE_ARRAY) {
            if (!is_place(e->as.slice.base)) {
                error_at(c, e->pos, "slicing an array needs a place");
                return builtin(c, TYPE_ERROR);
            }
            mark_address_taken(e->as.slice.base);
            return types_slice(c->types, t->element);
        }
        if (t->kind == TYPE_SLICE) {
            return t;
        }
        if (t->kind == TYPE_STR) {
            return types_slice(c->types, builtin(c, TYPE_U8));
        }
        error_at(c, e->pos, "cannot slice `%s`", tn(t));
        return builtin(c, TYPE_ERROR);
    }
    case EXPR_FIELD:
        return check_field(c, e);
    case EXPR_STRUCT_LIT: {
        struct name *name = &e->as.struct_lit.name;
        if (e->as.struct_lit.module.length > 0) {
            t = imported_struct(c, &e->as.struct_lit.module, name, e->pos);
            if (is_error(t)) {
                return t;
            }
        } else {
            sym = scope_find_local(&c->module_scope, name);
            if (sym == NULL || sym->kind != SYMBOL_STRUCT) {
                error_at(c, e->pos, "unknown struct `%.*s`",
                         (int)name->length, name->text);
                return builtin(c, TYPE_ERROR);
            }
            t = sym->type;
        }
        if (singleton_type(t) && checking_class(c) != t) {
            error_at(c, e->pos, "`%s` is a singleton, and `%s.get()` gives "
                     "its one instance", tn(t), tn(t));
            return builtin(c, TYPE_ERROR);
        }
        if (refuse_abstract_value(c, e->pos, "a literal", t)) {
            return builtin(c, TYPE_ERROR);
        }
        if (t->is_union && e->as.struct_lit.field_count != 1) {
            error_at(c, e->pos, "a literal of union `%s` names exactly one "
                     "field", tn(t));
            return builtin(c, TYPE_ERROR);
        }
        if (t->kind == TYPE_CLASS) {
            struct struct_field *flat;
            size_t count = chain_fields(t, NULL);
            flat = arena_alloc(c->arena, (count + 1) * sizeof *flat);
            chain_fields(t, flat);
            return check_field_inits(c, e, e->as.struct_lit.fields,
                                     e->as.struct_lit.field_count, flat,
                                     count, tn(t), false)
                       ? t
                       : builtin(c, TYPE_ERROR);
        }
        return check_field_inits(c, e, e->as.struct_lit.fields,
                                 e->as.struct_lit.field_count, t->fields,
                                 t->field_count, tn(t), t->is_union)
                   ? t
                   : builtin(c, TYPE_ERROR);
    }
    case EXPR_SLICE_LIT: {
        struct struct_field fields[2];
        struct type *element = resolve_type(c, e->as.slice_lit.element);
        if (is_error(element)) {
            return element;
        }
        t = types_slice(c->types, element);
        memset(fields, 0, sizeof fields);
        fields[0].name.text = "ptr";
        fields[0].name.length = 3;
        fields[0].type = types_pointer(c->types, element);
        fields[1].name.text = "len";
        fields[1].name.length = 3;
        fields[1].type = builtin(c, TYPE_I64);
        return check_field_inits(c, e, e->as.slice_lit.fields,
                                 e->as.slice_lit.field_count, fields, 2, tn(t),
                                 false)
                   ? t
                   : builtin(c, TYPE_ERROR);
    }
    case EXPR_ARRAY_LIT: {
        struct type *element = expected != NULL && expected->kind == TYPE_ARRAY
                                   ? expected->element
                                   : NULL;
        bool ok = true;
        for (i = 0; i < e->as.array_lit.count; i++) {
            struct expr *item = e->as.array_lit.elements[i];
            struct type *it = check_expr(c, item, element);
            if (element == NULL) {
                element = it;
            } else {
                ok = require(c, item, it, element) && ok;
            }
        }
        if (!ok || is_error(element)) {
            return builtin(c, TYPE_ERROR);
        }
        return types_array(c->types, element, e->as.array_lit.count);
    }
    case EXPR_ARRAY_REPEAT: {
        struct type *element = expected != NULL && expected->kind == TYPE_ARRAY
                                   ? expected->element
                                   : NULL;
        t = check_expr(c, e->as.array_repeat.value, element);
        return array_of(c, e->as.array_repeat.count, t);
    }
    case EXPR_ALLOC:
        /* DESIGN: `alloc T { ... }` allocates one object and writes the
           literal into it, so its type is the type of the literal. */
        if (e->as.alloc.value != NULL) {
            /* `alloc T(args)` puts the object on the heap and runs its
               `construct` with the arguments. */
            if (e->as.alloc.value->kind == EXPR_CALL) {
                e->as.alloc.value->as.call.on_heap = true;
                t = check_expr(c, e->as.alloc.value, NULL);
                if (is_error(t) ||
                    e->as.alloc.value->as.call.builds == NULL) {
                    error_at(c, e->as.alloc.value->pos,
                             "`alloc` of one object takes a literal or a "
                             "class with arguments");
                    return builtin(c, TYPE_ERROR);
                }
                return t;
            }
            if (e->as.alloc.value->kind != EXPR_STRUCT_LIT) {
                error_at(c, e->as.alloc.value->pos,
                         "`alloc` of one object takes a literal");
                return builtin(c, TYPE_ERROR);
            }
            t = check_expr(c, e->as.alloc.value, NULL);
            if (is_error(t)) {
                return t;
            }
            return types_pointer(c->types, t);
        }
        t = resolve_type(c, e->as.alloc.type);
        if (!require(c, e->as.alloc.count,
                     check_expr(c, e->as.alloc.count, builtin(c, TYPE_I64)),
                     builtin(c, TYPE_I64)) || is_error(t)) {
            return builtin(c, TYPE_ERROR);
        }
        if (refuse_abstract_value(c, e->pos, "`alloc`", t)) {
            return builtin(c, TYPE_ERROR);
        }
        return types_pointer(c->types, t);
    case EXPR_FREE:
        t = check_expr(c, e->as.free_pointer, NULL);
        if (!is_error(t) && t->kind != TYPE_POINTER) {
            error_at(c, e->as.free_pointer->pos,
                     "`free` needs a pointer, found `%s`", tn(t));
            return builtin(c, TYPE_ERROR);
        }
        return is_error(t) ? t : builtin(c, TYPE_VOID);
    /* DESIGN: `dup` copies an object of its concrete class and gives a
       pointer of the static type back. `delete` runs the destruct chain and
       frees the object. `destroy` runs the chain without the free, for
       an object that is not on a heap of its own. All three need a class
       pointer, because all three read the table of the object. */
    /* A node the checker makes, already typed. */
    case EXPR_ATOMIC:
        return e->type;
    case EXPR_OBJECT: {
        const char *what = e->as.object.op == TOKEN_DUP      ? "dup"
                           : e->as.object.op == TOKEN_DELETE ? "delete"
                                                             : "destroy";
        t = check_expr(c, e->as.object.operand, NULL);
        if (is_error(t)) {
            return t;
        }
        if (e->as.object.op == TOKEN_DELETE && t->kind == TYPE_POINTER &&
            singleton_type(t->element)) {
            error_at(c, e->pos, "`%s` is a singleton and outlives the "
                     "program", tn(t->element));
            return builtin(c, TYPE_ERROR);
        }
        if (t->kind != TYPE_POINTER || t->element->kind != TYPE_CLASS) {
            error_at(c, e->as.object.operand->pos,
                     "`%s` needs a class pointer, found `%s`", what, tn(t));
            return builtin(c, TYPE_ERROR);
        }
        return e->as.object.op == TOKEN_DUP ? t : builtin(c, TYPE_VOID);
    }
    case EXPR_SIZE_OF:
        t = resolve_type(c, e->as.size_of);
        return is_error(t) ? t : builtin(c, TYPE_I64);
    }
    return builtin(c, TYPE_ERROR);
}

static struct type *check_expr(struct checker *c, struct expr *e,
                               struct type *expected)
{
    struct type *t = check_expr_inner(c, e, expected);
    e->type = t;
    return t;
}

/* Constants */

/* DESIGN: a constant of a target-sized type is computed at 64 bits and
   must fit the narrower width, so it has one value on every target. */
static void wrap(struct const_value *v)
{
    int bits = type_is_target_sized(v->type) ? 64 : type_bits(v->type);

    if (bits < 64) {
        uint64_t mask = ((uint64_t)1 << bits) - 1;
        v->as.integer &= mask;
        if (type_is_signed(v->type) && (v->as.integer >> (bits - 1)) != 0) {
            v->as.integer |= ~mask;
        }
    }
}

static bool const_symbol(struct checker *c, struct symbol *sym,
                         struct pos use);

/* Report a constant of a target-sized type whose value differs between
   targets. */
static bool fits_every_target(struct checker *c, const struct expr *e,
                              const struct const_value *v)
{
    int bits = type_bits(v->type);
    bool fits;

    if (v->kind != CONST_INT || !type_is_target_sized(v->type)) {
        return true;
    }
    fits = type_is_signed(v->type)
               ? (int64_t)v->as.integer >= -((int64_t)1 << (bits - 1)) &&
                     (int64_t)v->as.integer < ((int64_t)1 << (bits - 1))
               : v->as.integer < ((uint64_t)1 << bits);
    if (!fits) {
        error_at(c, e->pos, "the value does not fit `%s` on every target",
                 tn(v->type));
    }
    return fits;
}

static bool fail_const(struct checker *c, const struct expr *e,
                       const char *what)
{
    error_at(c, e->pos, "%s is not a constant expression", what);
    return false;
}

/* A constant as a symbolic node: a symbolic value itself, or a number,
   a bool or a character as a node of its type. */
static const struct symbolic *as_symbolic(struct checker *c,
                                          const struct const_value *v)
{
    struct symbolic key;

    if (v->kind == CONST_SYMBOLIC) {
        return v->as.symbolic;
    }
    memset(&key, 0, sizeof key);
    key.kind = SYMBOLIC_INT;
    key.type = v->type;
    key.value = v->kind == CONST_BOOL   ? (uint64_t)v->as.boolean
                : v->kind == CONST_CHAR ? v->as.character
                                        : v->as.integer;
    return types_symbolic(c->types, &key);
}

/* Make out the symbolic value of an operation on a and, for a binary
   operation, b. */
static bool symbolic_value(struct checker *c, struct const_value *out,
                           enum symbolic_kind kind, enum token_kind op,
                           const struct const_value *a,
                           const struct const_value *b)
{
    struct symbolic key;

    memset(&key, 0, sizeof key);
    key.kind = kind;
    key.type = out->type;
    key.op = op;
    key.a = as_symbolic(c, a);
    key.b = b != NULL ? as_symbolic(c, b) : NULL;
    out->kind = CONST_SYMBOLIC;
    out->as.symbolic = types_symbolic(c->types, &key);
    return true;
}

/* True when a float converts to integer type t with a value that t
   holds after truncation toward zero. */
static bool float_fits(double x, const struct type *t)
{
    int bits = type_bits(t);
    double lo = type_is_signed(t) ? -ldexp(1.0, bits - 1) : 0.0;
    double hi = type_is_signed(t) ? ldexp(1.0, bits - 1) : ldexp(1.0, bits);

    return x > lo - 1.0 && x < hi;
}

/* Append the float x as an Anti float literal. It has the fewest
   significant digits that read back as x, and digits on both sides of the
   dot. An exponent follows outside 1e-5 to 1e21. NaN and the infinities
   have no literal and print as nan and inf. */
static void float_as_literal(struct text *out, double x, bool single)
{
    char buffer[40];
    char digits[24];
    const char *p;
    size_t count = 0;
    size_t i;
    int precision;
    int exponent;

    if (isnan(x)) {
        text_append(out, "nan");
        return;
    }
    if (isinf(x)) {
        text_append(out, x < 0 ? "-inf" : "inf");
        return;
    }
    for (precision = 1; precision < 17; precision++) {
        snprintf(buffer, sizeof buffer, "%.*e", precision - 1, x);
        if (single ? strtof(buffer, NULL) == (float)x
                   : strtod(buffer, NULL) == x) {
            break;
        }
    }
    snprintf(buffer, sizeof buffer, "%.*e", precision - 1, x);
    p = buffer;
    if (*p == '-') {
        text_append(out, "-");
        p++;
    }
    for (; *p != 'e'; p++) {
        if (*p != '.') {
            digits[count++] = *p;
        }
    }
    exponent = atoi(p + 1);
    while (count > 1 && digits[count - 1] == '0') {
        count--;
    }
    if (exponent < -5 || exponent >= 21) {
        text_append_bytes(out, digits, 1);
        text_append(out, ".");
        text_append_bytes(out, count > 1 ? digits + 1 : "0",
                          count > 1 ? count - 1 : 1);
        text_appendf(out, "e%d", exponent);
    } else if (exponent < 0) {
        text_append(out, "0.");
        for (i = 1; i < (size_t)-exponent; i++) {
            text_append(out, "0");
        }
        text_append_bytes(out, digits, count);
    } else {
        for (i = 0; i <= (size_t)exponent; i++) {
            text_append_bytes(out, i < count ? digits + i : "0", 1);
        }
        text_append(out, ".");
        if (count > (size_t)exponent + 1) {
            text_append_bytes(out, digits + exponent + 1,
                              count - (size_t)exponent - 1);
        } else {
            text_append(out, "0");
        }
    }
}

/* Append an operand of a constant error in the form the reader wrote it.
   A literal keeps its spelling and a negated literal its sign. Any other
   operand prints as the literal of its value v. */
static void operand_text(struct text *out, const struct expr *e,
                         const struct const_value *v)
{
    const struct expr *literal = e;

    if (e->kind == EXPR_UNARY && e->as.unary.op == TOKEN_MINUS) {
        literal = e->as.unary.operand;
    }
    if ((literal->kind == EXPR_INT || literal->kind == EXPR_FLOAT) &&
        literal->spelling.length > 0) {
        text_append(out, literal == e ? "" : "-");
        text_append_bytes(out, literal->spelling.bytes,
                          literal->spelling.length);
    } else if (v->kind == CONST_SYMBOLIC) {
        symbolic_print(out, v->as.symbolic, false);
    } else if (v->kind == CONST_FLOAT) {
        float_as_literal(out, v->as.floating, v->type->kind == TYPE_F32);
    } else if (type_is_signed(v->type)) {
        text_appendf(out, "%lld", (long long)v->as.integer);
    } else {
        text_appendf(out, "%llu", (unsigned long long)v->as.integer);
    }
}

/* Report an operation on the constant operands a and b that has no value.
   Integer division and remainder fail by zero and for the minimum value by
   -1. A shift fails for a count outside the bits of the type, and a float
   conversion outside the range of the integer type. e is a binary
   expression or a conversion to result, and b is NULL for a conversion. */
static bool reports_undefined(struct checker *c, const struct expr *e,
                              const struct type *result,
                              const struct const_value *a,
                              const struct const_value *b)
{
    struct text x = {0};
    struct text y = {0};
    char spelling[8];
    bool found = false;

    if (e->kind == EXPR_CAST) {
        if (a->kind == CONST_FLOAT && type_is_integer(result) &&
            !float_fits(a->as.floating, result)) {
            operand_text(&x, e->as.cast.operand, a);
            error_at(c, e->pos, "`%s as %s` does not fit `%s`",
                     text_cstr(&x), tn(result), tn(result));
            found = true;
        }
    } else if (type_is_integer(e->as.binary.left->type) &&
               b->kind != CONST_SYMBOLIC) {
        enum token_kind op = e->as.binary.op;
        const struct type *t = e->as.binary.left->type;
        int bits = type_bits(t);
        uint64_t mask = bits == 64 ? UINT64_MAX : ((uint64_t)1 << bits) - 1;
        bool divides = op == TOKEN_SLASH || op == TOKEN_PERCENT;
        const char *o = op_text(op, spelling);

        operand_text(&x, e->as.binary.left, a);
        operand_text(&y, e->as.binary.right, b);
        if (divides && b->as.integer == 0) {
            error_at(c, e->pos, "`%s %s %s` divides by zero", text_cstr(&x), o,
                     text_cstr(&y));
            found = true;
        } else if (divides && a->kind != CONST_SYMBOLIC && type_is_signed(t) &&
                   (a->as.integer & mask) == (uint64_t)1 << (bits - 1) &&
                   (int64_t)b->as.integer == -1) {
            error_at(c, e->pos, "`%s %s %s` does not fit `%s`", text_cstr(&x),
                     o, text_cstr(&y), tn(t));
            found = true;
        } else if ((op == TOKEN_SHL || op == TOKEN_SHR) &&
                   ((type_is_signed(t) && (int64_t)b->as.integer < 0) ||
                    b->as.integer >= (uint64_t)bits)) {
            error_at(c, e->pos, "`%s %s %s` shifts out of range",
                     text_cstr(&x), o, text_cstr(&y));
            found = true;
        }
    }
    text_free(&x);
    text_free(&y);
    return found;
}

/* DESIGN: C leaves these operations undefined, so no value exists that
   the constant folder could produce. With constant operands they are
   compile errors wherever they appear. A variable operand leaves them to
   run time, as chapter 2 states. The operands are already checked, so a
   constant name among them has its value, and the quiet evaluation adds
   no message of its own. */
static bool undefined_on_constants(struct checker *c, struct expr *e,
                                   const struct type *result)
{
    struct const_value a;
    struct const_value b;
    bool constant;

    c->quiet++;
    if (e->kind == EXPR_CAST) {
        constant = eval_const(c, e->as.cast.operand, &a);
    } else {
        constant = eval_const(c, e->as.binary.left, &a) &&
                   eval_const(c, e->as.binary.right, &b);
    }
    c->quiet--;
    return constant &&
           reports_undefined(c, e, result, &a,
                             e->kind == EXPR_CAST ? NULL : &b);
}

/* Evaluate a checked expression. The expression must use only what
   chapter 2 allows in a constant. */
static bool eval_const(struct checker *c, struct expr *e,
                       struct const_value *out)
{
    struct const_value a;
    struct const_value b;
    size_t i;

    memset(out, 0, sizeof *out);
    out->type = e->type;
    switch (e->kind) {
    case EXPR_INT:
        out->kind = CONST_INT;
        out->as.integer = e->as.integer;
        wrap(out);
        return true;
    case EXPR_FLOAT: {
        char digits[128];
        snprintf(digits, sizeof digits, "%.*s", (int)e->as.text.length,
                 e->as.text.bytes);
        out->kind = CONST_FLOAT;
        out->as.floating = e->type->kind == TYPE_F32 ? strtof(digits, NULL)
                                                    : strtod(digits, NULL);
        return true;
    }
    case EXPR_CHAR:
        out->kind = CONST_CHAR;
        out->as.character = e->as.character;
        return true;
    case EXPR_BOOL:
        out->kind = CONST_BOOL;
        out->as.boolean = e->as.boolean;
        return true;
    case EXPR_NULL:
        out->kind = CONST_NULL;
        return true;
    case EXPR_STRING:
    case EXPR_BYTES:
        out->kind = CONST_TEXT;
        out->as.text = e->as.text;
        return true;
    case EXPR_NAME:
        if (e->symbol == NULL || e->symbol->kind != SYMBOL_CONST) {
            return fail_const(c, e, "a variable");
        }
        if (!const_symbol(c, e->symbol, e->pos)) {
            return false;
        }
        *out = *e->symbol->value;
        return true;
    case EXPR_SIZE_OF: {
        struct symbolic key;
        memset(&key, 0, sizeof key);
        key.kind = SYMBOLIC_SIZE_OF;
        key.type = e->type;
        key.of = e->as.size_of->type;
        out->kind = CONST_SYMBOLIC;
        out->as.symbolic = types_symbolic(c->types, &key);
        return !is_error(key.of);
    }
    case EXPR_CAST:
        if (!eval_const(c, e->as.cast.operand, &a)) {
            return false;
        }
        out->type = e->type;
        if (a.kind == CONST_SYMBOLIC) {
            if (!type_is_integer(e->type)) {
                error_at(c, e->pos, "a value computed from `size_of` converts "
                         "only to an integer type in a constant expression");
                return false;
            }
            return symbolic_value(c, out, SYMBOLIC_CAST, TOKEN_AS, &a, NULL);
        }
        if (type_is_float(e->type)) {
            out->kind = CONST_FLOAT;
            out->as.floating = a.kind == CONST_FLOAT ? a.as.floating
                               : type_is_signed(a.type)
                                   ? (double)(int64_t)a.as.integer
                                   : (double)a.as.integer;
            if (e->type->kind == TYPE_F32) {
                out->as.floating = (float)out->as.floating;
            }
        } else if (type_is_integer(e->type)) {
            out->kind = CONST_INT;
            if (a.kind == CONST_FLOAT) {
                if (reports_undefined(c, e, e->type, &a, NULL)) {
                    return false;
                }
                out->as.integer = type_is_signed(e->type)
                                      ? (uint64_t)(int64_t)a.as.floating
                                      : (uint64_t)a.as.floating;
            } else if (a.kind == CONST_BOOL) {
                out->as.integer = a.as.boolean ? 1 : 0;
            } else if (a.kind == CONST_CHAR) {
                out->as.integer = a.as.character;
            } else {
                out->as.integer = a.as.integer;
            }
            wrap(out);
        } else if (e->type->kind == TYPE_CHAR) {
            out->kind = CONST_CHAR;
            out->as.character = (uint32_t)a.as.integer;
        } else {
            return fail_const(c, e, "a pointer conversion");
        }
        return fits_every_target(c, e, out);
    case EXPR_UNARY:
        if (e->as.unary.op == TOKEN_AMP || e->as.unary.op == TOKEN_STAR) {
            return fail_const(c, e, "an address or a dereference");
        }
        if (!eval_const(c, e->as.unary.operand, &a)) {
            return false;
        }
        if (a.kind == CONST_SYMBOLIC) {
            return symbolic_value(c, out, SYMBOLIC_UNARY, e->as.unary.op, &a,
                                  NULL);
        }
        *out = a;
        out->type = e->type;
        if (e->as.unary.op == TOKEN_BANG) {
            out->as.boolean = !a.as.boolean;
        } else if (e->as.unary.op == TOKEN_TILDE) {
            out->as.integer = ~a.as.integer;
            wrap(out);
        } else if (a.kind == CONST_FLOAT) {
            out->as.floating = -a.as.floating;
        } else {
            out->as.integer = (uint64_t)0 - a.as.integer;
            wrap(out);
        }
        return fits_every_target(c, e, out);
    case EXPR_BINARY: {
        enum token_kind op = e->as.binary.op;
        struct type *operand = e->as.binary.left->type;
        bool is_float;
        bool is_signed = type_is_signed(operand);
        int bits = type_bits(operand);

        if (!eval_const(c, e->as.binary.left, &a)) {
            return false;
        }
        if (op == TOKEN_AND_AND || op == TOKEN_OR_OR) {
            if (a.kind != CONST_SYMBOLIC &&
                a.as.boolean == (op == TOKEN_OR_OR)) {
                *out = a;
                return true;
            }
            if (a.kind != CONST_SYMBOLIC) {
                return eval_const(c, e->as.binary.right, out);
            }
        }
        if (!eval_const(c, e->as.binary.right, &b)) {
            return false;
        }
        if (reports_undefined(c, e, e->type, &a, &b)) {
            return false;
        }
        if (a.kind == CONST_SYMBOLIC || b.kind == CONST_SYMBOLIC) {
            out->type = e->type;
            return symbolic_value(c, out, SYMBOLIC_BINARY, op, &a, &b);
        }
        is_float = a.kind == CONST_FLOAT;
        out->kind = CONST_INT;
        out->type = e->type;
        if (e->type->kind == TYPE_BOOL) {
            int cmp;
            out->kind = CONST_BOOL;
            if (is_float) {
                cmp = a.as.floating < b.as.floating ? -1
                      : a.as.floating > b.as.floating ? 1 : 0;
            } else if (a.kind == CONST_NULL || b.kind == CONST_NULL) {
                cmp = a.kind == b.kind ? 0 : 1;
            } else if (is_signed) {
                cmp = (int64_t)a.as.integer < (int64_t)b.as.integer ? -1
                      : (int64_t)a.as.integer > (int64_t)b.as.integer ? 1 : 0;
            } else {
                uint64_t x = a.kind == CONST_CHAR ? a.as.character
                             : a.kind == CONST_BOOL ? a.as.boolean
                                                    : a.as.integer;
                uint64_t y = b.kind == CONST_CHAR ? b.as.character
                             : b.kind == CONST_BOOL ? b.as.boolean
                                                    : b.as.integer;
                cmp = x < y ? -1 : x > y ? 1 : 0;
            }
            out->as.boolean = op == TOKEN_EQ ? cmp == 0
                              : op == TOKEN_NE ? cmp != 0
                              : op == TOKEN_LT ? cmp < 0
                              : op == TOKEN_LE ? cmp <= 0
                              : op == TOKEN_GT ? cmp > 0
                                               : cmp >= 0;
            if (is_float && (isnan(a.as.floating) || isnan(b.as.floating))) {
                out->as.boolean = op == TOKEN_NE;
            }
            return true;
        }
        if (is_float) {
            out->kind = CONST_FLOAT;
            out->as.floating = op == TOKEN_PLUS    ? a.as.floating + b.as.floating
                               : op == TOKEN_MINUS ? a.as.floating - b.as.floating
                               : op == TOKEN_STAR  ? a.as.floating * b.as.floating
                                                   : a.as.floating / b.as.floating;
            if (e->type->kind == TYPE_F32) {
                out->as.floating = (float)out->as.floating;
            }
            return true;
        }
        switch (op) {
        case TOKEN_PLUS: out->as.integer = a.as.integer + b.as.integer; break;
        case TOKEN_MINUS: out->as.integer = a.as.integer - b.as.integer; break;
        case TOKEN_STAR: out->as.integer = a.as.integer * b.as.integer; break;
        case TOKEN_AMP: out->as.integer = a.as.integer & b.as.integer; break;
        case TOKEN_PIPE: out->as.integer = a.as.integer | b.as.integer; break;
        case TOKEN_CARET: out->as.integer = a.as.integer ^ b.as.integer; break;
        case TOKEN_SLASH:
        case TOKEN_PERCENT: {
            if (is_signed) {
                int64_t x = (int64_t)a.as.integer;
                int64_t y = (int64_t)b.as.integer;
                out->as.integer = (uint64_t)(op == TOKEN_SLASH ? x / y : x % y);
            } else {
                out->as.integer = op == TOKEN_SLASH ? a.as.integer / b.as.integer
                                                    : a.as.integer % b.as.integer;
            }
            break;
        }
        case TOKEN_SHL:
        case TOKEN_SHR:
            if (op == TOKEN_SHL) {
                out->as.integer = a.as.integer << b.as.integer;
            } else if (is_signed) {
                out->as.integer = (uint64_t)((int64_t)a.as.integer >>
                                             b.as.integer);
            } else {
                uint64_t mask = bits == 64 ? UINT64_MAX
                                           : ((uint64_t)1 << bits) - 1;
                out->as.integer = (a.as.integer & mask) >> b.as.integer;
            }
            break;
        default:
            break;
        }
        wrap(out);
        return fits_every_target(c, e, out);
    }
    case EXPR_ARRAY_LIT:
        out->kind = CONST_ARRAY;
        out->as.aggregate.count = e->as.array_lit.count;
        out->as.aggregate.items = arena_alloc(
            c->arena, e->as.array_lit.count * sizeof *out->as.aggregate.items);
        for (i = 0; i < e->as.array_lit.count; i++) {
            if (!eval_const(c, e->as.array_lit.elements[i],
                            &out->as.aggregate.items[i])) {
                return false;
            }
        }
        return true;
    case EXPR_ARRAY_REPEAT:
        if (e->type->length_of != NULL) {
            return fail_const(c, e, "an array with a length from `size_of`");
        }
        if (!eval_const(c, e->as.array_repeat.value, &a)) {
            return false;
        }
        out->kind = CONST_ARRAY;
        out->as.aggregate.count = e->type->length;
        out->as.aggregate.items = arena_alloc(
            c->arena, e->type->length * sizeof *out->as.aggregate.items);
        for (i = 0; i < e->type->length; i++) {
            out->as.aggregate.items[i] = a;
        }
        return true;
    case EXPR_STRUCT_LIT: {
        struct type *s = e->type;
        size_t j;
        if (s->is_union) {
            return fail_const(c, e, "a union");
        }
        out->kind = CONST_STRUCT;
        out->as.aggregate.count = s->field_count;
        out->as.aggregate.items =
            arena_alloc(c->arena, s->field_count * sizeof *out->as.aggregate.items);
        /* A zero-width bitfield holds no value and keeps a zero. */
        for (j = 0; j < s->field_count; j++) {
            out->as.aggregate.items[j].kind = CONST_INT;
            out->as.aggregate.items[j].type = s->fields[j].type;
            out->as.aggregate.items[j].as.integer = 0;
        }
        for (i = 0; i < e->as.struct_lit.field_count; i++) {
            for (j = 0; j < s->field_count; j++) {
                if (same_name(&s->fields[j].name, &e->as.struct_lit.fields[i].name)) {
                    if (!eval_const(c, e->as.struct_lit.fields[i].value,
                                    &out->as.aggregate.items[j])) {
                        return false;
                    }
                }
            }
        }
        return true;
    }
    case EXPR_FIELD: {
        const struct struct_field *f;
        struct type *base;
        /* A value of an enum is the number the declaration folded. Its
           base is a type name, which carries no value of its own. */
        if (e->as.field.enum_value != 0 && e->type->kind == TYPE_ENUM) {
            out->kind = CONST_INT;
            out->type = e->type;
            out->as.integer =
                e->type->fields[e->as.field.enum_value - 1].number;
            return true;
        }
        base = e->as.field.base->type;
        if (base == NULL) {
            return fail_const(c, e, "this field");
        }
        if (type_has_fields(base)) {
            if (!eval_const(c, e->as.field.base, &a)) {
                return false;
            }
            f = find_field(base, &e->as.field.name);
            *out = a.as.aggregate.items[f - base->fields];
            return true;
        }
        if (name_is(&e->as.field.name, "len") && base->kind == TYPE_ARRAY &&
            base->length_of != NULL) {
            return fail_const(c, e, "the length of an array from `size_of`");
        }
        /* The base of .len is a constant array or a string literal. */
        if (name_is(&e->as.field.name, "len") && base->kind == TYPE_ARRAY &&
            !eval_const(c, e->as.field.base, &a)) {
            return false;
        }
        if (name_is(&e->as.field.name, "len") &&
            (base->kind == TYPE_ARRAY || e->as.field.base->kind == EXPR_STRING)) {
            out->kind = CONST_INT;
            out->as.integer = base->kind == TYPE_ARRAY
                                  ? base->length
                                  : e->as.field.base->as.text.length;
            return true;
        }
        return fail_const(c, e, "this field");
    }
    case EXPR_INDEX:
        if (e->as.index.base->type->kind != TYPE_ARRAY) {
            return fail_const(c, e, "indexing anything but a constant array");
        }
        if (!eval_const(c, e->as.index.base, &a) ||
            !eval_const(c, e->as.index.index, &b)) {
            return false;
        }
        if (b.kind == CONST_SYMBOLIC) {
            return fail_const(c, e->as.index.index,
                              "an index computed from `size_of`");
        }
        if (b.as.integer >= a.as.aggregate.count) {
            error_at(c, e->as.index.index->pos, "the index %lld is outside the "
                     "constant array", (long long)b.as.integer);
            return false;
        }
        *out = a.as.aggregate.items[b.as.integer];
        return true;
    case EXPR_CALL:
    case EXPR_FREE:
    case EXPR_OBJECT:
    case EXPR_ATOMIC:
        return fail_const(c, e, "a call");
    case EXPR_ALLOC:
        return fail_const(c, e, "`alloc`");
    case EXPR_PARALLEL:
        return fail_const(c, e, "`parallel`");
    case EXPR_DISPATCH:
    case EXPR_JOIN:
        return fail_const(c, e, "a call");
    case EXPR_SLICE:
    case EXPR_SLICE_LIT:
        return fail_const(c, e, "a slice");
    }
    return false;
}

/* Give a constant symbol its type and value, once. */
static bool const_symbol(struct checker *c, struct symbol *sym,
                         struct pos use)
{
    struct type_expr *type_expr;
    struct expr *value;
    struct scope *saved = c->scope;
    struct type *t;
    bool ok;

    if (sym->state == EVAL_DONE) {
        return sym->value != NULL;
    }
    if (sym->state == EVAL_BUSY) {
        error_at(c, use, "`%.*s` depends on itself", (int)sym->name.length,
                 sym->name.text);
        return false;
    }
    sym->state = EVAL_BUSY;
    if (sym->item != NULL) {
        type_expr = sym->item->type;
        value = sym->item->value;
        c->scope = &c->module_scope;
    } else {
        type_expr = sym->stmt->as.let.type;
        value = sym->stmt->as.let.value;
    }
    t = resolve_type(c, type_expr);
    ok = !is_error(t) && require(c, value, check_expr(c, value, t), t);
    if (ok) {
        sym->value = arena_alloc(c->arena, sizeof *sym->value);
        ok = eval_const(c, value, sym->value);
        if (!ok) {
            sym->value = NULL;
        }
    }
    sym->type = ok ? t : builtin(c, TYPE_ERROR);
    sym->state = EVAL_DONE;
    c->scope = saved;
    return ok;
}

/* Statements */

static void check_block(struct checker *c, struct block *b);

static bool block_returns(const struct block *b);

/* The missing-return rule of chapter 2 decides from the form alone. */
static bool stmt_returns(const struct stmt *s)
{
    size_t i;

    if (s->kind == STMT_RETURN) {
        return true;
    }
    if (s->kind == STMT_IF && s->as.if_chain.else_body != NULL) {
        for (i = 0; i < s->as.if_chain.count; i++) {
            if (!block_returns(s->as.if_chain.branches[i].body)) {
                return false;
            }
        }
        return block_returns(s->as.if_chain.else_body);
    }
    return false;
}

static bool block_returns(const struct block *b)
{
    return b->count > 0 && stmt_returns(b->stmts[b->count - 1]);
}

static void check_condition(struct checker *c, struct expr *cond)
{
    struct type *t = check_expr(c, cond, NULL);

    if (!is_error(t) && t->kind != TYPE_BOOL) {
        error_at(c, cond->pos, "a condition has type `bool`, found `%s`", tn(t));
    }
}

static void check_assign(struct checker *c, struct stmt *s)
{
    struct expr *target = s->as.assign.target;
    struct type *t = check_expr(c, target, NULL);
    struct type *v;
    enum token_kind op = s->as.assign.op;

    if (is_error(t)) {
        check_expr(c, s->as.assign.value, NULL);
        return;
    }
    /* The variable of a `for` is read-only, so the loop keeps its step
       and the sequence it walks. */
    if (target->kind == EXPR_NAME && target->symbol != NULL &&
        target->symbol->read_only) {
        error_at(c, target->pos, "`%.*s` is the variable of a `for` and is "
                 "read-only", (int)target->as.name.length,
                 target->as.name.text);
        check_expr(c, s->as.assign.value, NULL);
        return;
    }
    /* DESIGN: a field of a singleton is read-only after creation, so
       that a program has one place to look for the value. `construct`
       sets it, and `mutable` marks the fields the program may write
       anywhere. An atomic field has its own operations. */
    if (target->kind == EXPR_FIELD) {
        const struct type *owner = struct_of(target->as.field.base->type);
        const struct struct_field *f =
            owner != NULL ? find_field(owner, &target->as.field.name) : NULL;
        if (f != NULL && !f->writable && singleton_type(owner) &&
            (c->function == NULL ||
             !name_is(&c->function->name, "construct"))) {
            error_at(c, target->pos, "`%.*s` of singleton `%s` is read-only "
                     "after creation, and `mutable` marks a field the "
                     "program writes", (int)target->as.field.name.length,
                     target->as.field.name.text, tn(owner));
            check_expr(c, s->as.assign.value, NULL);
            return;
        }
    }
    if (!is_place(target) ||
        (target->kind == EXPR_INDEX &&
         target->as.index.base->type->kind == TYPE_STR)) {
        error_at(c, target->pos, "cannot assign to this expression");
        return;
    }
    v = check_expr(c, s->as.assign.value, t);
    if (!require(c, s->as.assign.value, v, t) || op == TOKEN_ASSIGN) {
        return;
    }
    if ((op == TOKEN_PLUS_ASSIGN || op == TOKEN_MINUS_ASSIGN ||
         op == TOKEN_STAR_ASSIGN || op == TOKEN_SLASH_ASSIGN)
            ? !type_is_numeric(t)
            : !type_is_integer(t)) {
        char spelling[8];
        error_at(c, s->pos, "`%s` does not apply to `%s`",
                 op_text(op, spelling), tn(t));
    }
}

/* Every value of an enum needs an arm when a switch has no else. The
   message names the ones that have none. */
static void check_switch_covers(struct checker *c, const struct stmt *s,
                                const struct type *over)
{
    struct text missing = {0};
    size_t found = 0;
    size_t i;
    size_t j;

    for (i = 0; i < over->field_count; i++) {
        bool covered = false;
        for (j = 0; j < s->as.switch_stmt.count && !covered; j++) {
            struct const_value v;
            covered = eval_const(c, s->as.switch_stmt.arms[j].value, &v) &&
                      v.as.integer == over->fields[i].number;
        }
        if (covered) {
            continue;
        }
        text_appendf(&missing, "%s`%.*s`", found++ > 0 ? ", " : "",
                     (int)over->fields[i].name.length,
                     over->fields[i].name.text);
    }
    if (found > 0) {
        error_at(c, s->pos, "this `switch` on `%s` has no arm for %s",
                 tn((struct type *)over), text_cstr(&missing));
    }
    text_free(&missing);
}

/* Whether e holds a call anywhere inside it. */
static bool expr_calls(const struct expr *e)
{
    size_t i;

    if (e == NULL) {
        return false;
    }
    switch (e->kind) {
    case EXPR_CALL:
    case EXPR_ALLOC:
    case EXPR_FREE:
    case EXPR_OBJECT:
    case EXPR_ATOMIC:
    case EXPR_PARALLEL:
    case EXPR_DISPATCH:
    case EXPR_JOIN:
        return true;
    case EXPR_UNARY:
        return expr_calls(e->as.unary.operand);
    case EXPR_BINARY:
        return expr_calls(e->as.binary.left) ||
               expr_calls(e->as.binary.right);
    case EXPR_CAST:
        return expr_calls(e->as.cast.operand);
    case EXPR_FIELD:
        return expr_calls(e->as.field.base);
    case EXPR_INDEX:
        return expr_calls(e->as.index.base) || expr_calls(e->as.index.index);
    case EXPR_SLICE:
        return expr_calls(e->as.slice.base) || expr_calls(e->as.slice.low) ||
               expr_calls(e->as.slice.high);
    case EXPR_STRUCT_LIT:
        for (i = 0; i < e->as.struct_lit.field_count; i++) {
            if (expr_calls(e->as.struct_lit.fields[i].value)) {
                return true;
            }
        }
        return false;
    case EXPR_ARRAY_LIT:
        for (i = 0; i < e->as.array_lit.count; i++) {
            if (expr_calls(e->as.array_lit.elements[i])) {
                return true;
            }
        }
        return false;
    default:
        return false;
    }
}

/* Refuse a step of zero, which would never leave the range.
   https://foundingfuture.com/programming/writing-a-compiler/02-the-anti-language/#fn:heederik
   names the loop this check remembers. */
static void heederik_guardrail(struct checker *c, struct expr *step,
                               const struct const_value *v)
{
    if (v->as.integer == 0) {
        error_at(c, step->pos, "`by 0` never advances");
    }
}

/* DESIGN: the step of a `for` range is a constant. The back end then
   knows which way the loop walks, and the checker can refuse a step of
   zero. A variable would hide both until the program hangs. */
static void check_step(struct checker *c, struct stmt *s, struct type *element)
{
    struct expr *step = s->as.for_loop.step;
    struct const_value v;
    struct type *type = check_expr(c, step, element);

    if (is_error(type)) {
        return;
    }
    if (!type_is_integer(type)) {
        error_at(c, step->pos, "a `for` step is an integer, found `%s`",
                 tn(type));
        return;
    }
    if (!eval_const(c, step, &v)) {
        return;
    }
    s->as.for_loop.step_value = (int64_t)v.as.integer;
    heederik_guardrail(c, step, &v);
}

static void check_stmt(struct checker *c, struct stmt *s)
{
    struct type *t;
    struct symbol *sym;
    size_t i;
    struct type *result = c->function->symbol->type->result;

    switch (s->kind) {
    case STMT_LET: {
        struct type *declared;
        c->target_sized = true;
        declared = s->as.let.type != NULL ? resolve_type(c, s->as.let.type)
                                          : NULL;
        t = check_expr(c, s->as.let.value, declared);
        c->target_sized = false;
        if (declared != NULL) {
            require(c, s->as.let.value, t, declared);
            t = declared;
        } else if (!is_error(t) && t->kind == TYPE_VOID) {
            require(c, s->as.let.value, t, builtin(c, TYPE_I64));
            t = builtin(c, TYPE_ERROR);
        }
        if (refuse_abstract_value(c, s->as.let.name_pos, "this local", t)) {
            t = builtin(c, TYPE_ERROR);
        }
        sym = declare(c, SYMBOL_LOCAL, &s->as.let.name, s->as.let.name_pos,
                      "`%.*s` is already declared in this block");
        refuse_escaping_error(c, s->as.let.value);
        if (sym != NULL) {
            sym->type = t;
            s->as.let.symbol = sym;
            /* A failing call writes its result through a pointer, so
               the local it writes to needs a place of its own. So does
               the pointer of `alloc T(args)`, which a handler may
               replace. */
            if (s->as.let.value->kind == EXPR_CALL &&
                s->as.let.value->as.call.out != NULL) {
                sym->address_taken = true;
            }
            if (s->as.let.value->kind == EXPR_ALLOC &&
                s->as.let.value->as.alloc.value != NULL &&
                s->as.let.value->as.alloc.value->kind == EXPR_CALL &&
                s->as.let.value->as.alloc.value->as.call.builds != NULL) {
                sym->address_taken = true;
            }
        }
        return;
    }
    case STMT_CONST:
        sym = declare(c, SYMBOL_CONST, &s->as.let.name, s->as.let.name_pos,
                      "`%.*s` is already declared in this block");
        if (sym != NULL) {
            sym->stmt = s;
            s->as.let.symbol = sym;
            const_symbol(c, sym, s->as.let.name_pos);
        }
        return;
    case STMT_EXPR:
        check_expr(c, s->as.expr, NULL);
        return;
    case STMT_ASSIGN:
        refuse_escaping_error(c, s->as.assign.value);
        check_assign(c, s);
        return;
    case STMT_IF:
        for (i = 0; i < s->as.if_chain.count; i++) {
            check_condition(c, s->as.if_chain.branches[i].cond);
            check_block(c, s->as.if_chain.branches[i].body);
        }
        if (s->as.if_chain.else_body != NULL) {
            check_block(c, s->as.if_chain.else_body);
        }
        return;
    case STMT_WHILE:
    case STMT_DO_WHILE:
        c->loop_depth++;
        if (s->kind == STMT_WHILE) {
            check_condition(c, s->as.loop.cond);
        }
        check_block(c, s->as.loop.body);
        if (s->kind == STMT_DO_WHILE) {
            check_condition(c, s->as.loop.cond);
        }
        c->loop_depth--;
        return;
    /* DESIGN: `for i in lo..hi` counts over an integer range, and
       `for x in slice` walks a slice or an array. The variable is
       read-only in the body, so the loop cannot lose its step. */
    /* `yield v` gives the value that takes the place of the result the
       failing call would have written. */
    case STMT_YIELD:
        if (c->handler_depth == 0) {
            error_at(c, s->pos, "`yield` stands in a `catch` handler");
            return;
        }
        if (s->as.yielded == NULL) {
            if (c->yields->kind != TYPE_VOID) {
                error_at(c, s->pos, "`yield` gives a value of type `%s`",
                         tn(c->yields));
            }
            return;
        }
        if (c->yields->kind == TYPE_VOID) {
            error_at(c, s->pos, "the call writes no result, so `yield` takes "
                     "no value");
            check_expr(c, s->as.yielded, NULL);
            return;
        }
        require(c, s->as.yielded,
                check_expr(c, s->as.yielded, c->yields), c->yields);
        return;
    /* `try { } catch e { }` handles every failing call of the block. */
    case STMT_TRY: {
        struct scope try_scope;
        struct handler *h = &s->as.try_block.handler;
        struct block *outer_try = c->try_block;
        struct type *outer_error = c->error_type;
        c->try_block = s->as.try_block.body;
        c->error_type = NULL;
        check_block(c, s->as.try_block.body);
        c->try_block = outer_try;
        if (h->kind != HANDLE_BLOCK) {
            return;
        }
        enter_scope(c, &try_scope);
        if (h->name.length > 0) {
            h->symbol = declare(c, SYMBOL_LOCAL, &h->name, h->pos,
                                "`%.*s` is already declared in this block");
            if (h->symbol != NULL) {
                h->symbol->type = c->error_type != NULL
                                      ? c->error_type
                                      : builtin(c, TYPE_ERROR);
                h->symbol->read_only = true;
                h->symbol->caught = true;
            }
        }
        check_block(c, h->body);
        h->passes = returns_error(h->body, h->symbol);
        leave_scope(c, &try_scope);
        c->error_type = outer_error;
        return;
    }
    case STMT_FOR: {
        struct scope for_scope;
        struct type *element = NULL;
        struct symbol *loop_var;
        enter_scope(c, &for_scope);
        if (s->as.for_loop.over != NULL) {
            struct type *over = check_expr(c, s->as.for_loop.over, NULL);
            /* A range without a name repeats its block. A slice has an
               element to read, so it names one. */
            if (s->as.for_loop.name.length == 0) {
                error_at(c, s->pos, "a `for` over a slice names its element");
            }
            if (!is_error(over) && over->kind != TYPE_SLICE &&
                over->kind != TYPE_ARRAY) {
                error_at(c, s->as.for_loop.over->pos,
                         "`for` walks a slice or an array, found `%s`",
                         tn(over));
                element = builtin(c, TYPE_ERROR);
            } else if (!is_error(over)) {
                element = over->element;
                if (s->as.for_loop.by_pointer) {
                    element = types_pointer(c->types, element);
                }
            } else {
                element = over;
            }
        } else {
            struct type *low = check_expr(c, s->as.for_loop.low, NULL);
            struct type *high =
                check_expr(c, s->as.for_loop.high,
                           is_error(low) ? NULL : low);
            if (!is_error(low) && !type_is_integer(low)) {
                error_at(c, s->as.for_loop.low->pos,
                         "a `for` range counts over an integer, found `%s`",
                         tn(low));
                low = builtin(c, TYPE_ERROR);
            } else if (!is_error(low)) {
                require(c, s->as.for_loop.high, high, low);
            }
            element = low;
        }
        s->as.for_loop.step_value = 1;
        if (s->as.for_loop.step != NULL) {
            check_step(c, s, element);
        }
        /* A range without a name repeats its block and counts in a
           temporary that no body can read. */
        if (s->as.for_loop.name.length > 0) {
            loop_var = declare(c, SYMBOL_LOCAL, &s->as.for_loop.name,
                               s->as.for_loop.name_pos,
                               "`%.*s` is already declared in this block");
            if (loop_var != NULL) {
                loop_var->type = element;
                loop_var->read_only = true;
                s->as.for_loop.symbol = loop_var;
            }
        }
        c->loop_depth++;
        check_block(c, s->as.for_loop.body);
        c->loop_depth--;
        leave_scope(c, &for_scope);
        return;
    }
    /* DESIGN: `switch` runs one arm and never falls through. A switch on
       an enum without `else` covers every value, and the message names
       the ones it misses. */
    case STMT_SWITCH: {
        struct type *over = check_expr(c, s->as.switch_stmt.value, NULL);
        size_t j;
        if (!is_error(over) && !type_is_integer(over) &&
            over->kind != TYPE_ENUM) {
            error_at(c, s->as.switch_stmt.value->pos,
                     "`switch` takes an enum or an integer, found `%s`",
                     tn(over));
            over = builtin(c, TYPE_ERROR);
        }
        for (i = 0; i < s->as.switch_stmt.count; i++) {
            struct switch_arm *arm = &s->as.switch_stmt.arms[i];
            struct const_value v;
            require(c, arm->value, check_expr(c, arm->value, over), over);
            if (!is_error(over) && eval_const(c, arm->value, &v)) {
                for (j = 0; j < i; j++) {
                    struct const_value other;
                    if (eval_const(c, s->as.switch_stmt.arms[j].value,
                                   &other) &&
                        other.kind == v.kind &&
                        other.as.integer == v.as.integer) {
                        error_at(c, arm->pos, "this value already has an arm");
                    }
                }
            }
            check_stmt(c, arm->body);
        }
        if (s->as.switch_stmt.otherwise != NULL) {
            check_stmt(c, s->as.switch_stmt.otherwise);
        } else if (!is_error(over) && over->kind == TYPE_ENUM) {
            check_switch_covers(c, s, over);
        }
        return;
    }
    /* DESIGN: a release build removes the whole statement, so a call in
       the condition does not run there. The warning says so, because the
       program would behave differently in the two builds. */
    case STMT_ASSERT:
        require(c, s->as.assertion.cond,
                check_expr(c, s->as.assertion.cond, builtin(c, TYPE_BOOL)),
                builtin(c, TYPE_BOOL));
        if (expr_calls(s->as.assertion.cond)) {
            diagnostics_warn(c->diags, s->as.assertion.cond->pos.line,
                             s->as.assertion.cond->pos.column,
                             "this `assert` condition calls a function, "
                             "which a release build does not run");
        }
        return;
    case STMT_DEFER:
        check_stmt(c, s->as.deferred);
        return;
    case STMT_BREAK:
    case STMT_CONTINUE:
        if (c->loop_depth == 0) {
            error_at(c, s->pos, "`%s` outside a loop",
                     s->kind == STMT_BREAK ? "break" : "continue");
        }
        return;
    case STMT_RETURN:
        if (s->as.return_value == NULL) {
            if (result->kind != TYPE_VOID) {
                error_at(c, s->pos, "`return` needs a value of type `%s`",
                         tn(result));
            }
            return;
        }
        if (result->kind == TYPE_VOID) {
            check_expr(c, s->as.return_value, NULL);
            error_at(c, s->as.return_value->pos, "`%.*s` returns no value",
                     (int)c->function->name.length, c->function->name.text);
            return;
        }
        require(c, s->as.return_value,
                check_expr(c, s->as.return_value, result), result);
        return;
    case STMT_BLOCK:
        check_block(c, s->as.block);
        return;
    }
}

static void check_block(struct checker *c, struct block *b)
{
    struct scope scope;
    size_t i;

    enter_scope(c, &scope);
    for (i = 0; i < b->count; i++) {
        check_stmt(c, b->stmts[i]);
    }
    leave_scope(c, &scope);
}

static void check_function(struct checker *c, struct item *it)
{
    struct scope params;
    size_t i;

    if (is_error(it->symbol->type)) {
        return;
    }
    c->function = it;
    enter_scope(c, &params);
    if (it->has_self) {
        static const struct name self_name = {"self", 4};
        struct symbol *sym =
            declare(c, SYMBOL_PARAM, &self_name, it->pos,
                    "`%.*s` is already declared in this block");
        if (sym != NULL) {
            sym->type = it->symbol->type->params[0];
            it->self = sym;
        }
    }
    for (i = 0; i < it->param_count; i++) {
        struct symbol *sym =
            declare(c, SYMBOL_PARAM, &it->params[i].name, it->params[i].pos,
                    "`%.*s` is already declared in this block");
        if (sym != NULL) {
            sym->type = it->symbol->type->params[i + (it->has_self ? 1 : 0)];
            it->params[i].symbol = sym;
        }
    }
    check_block(c, it->body);
    leave_scope(c, &params);
    if (it->symbol->type->result->kind != TYPE_VOID &&
        !block_returns(it->body)) {
        error_at(c, it->name_pos, "`%.*s` can reach its end without `return`",
                 (int)it->name.length, it->name.text);
    }
    c->function = NULL;
}

/* main takes one of the three forms of chapter 2. */
static void check_main(struct checker *c, struct item *it)
{
    struct type *t = it->symbol->type;
    struct type *strs = types_slice(c->types, builtin(c, TYPE_STR));
    size_t i;
    bool ok = t->result->kind == TYPE_I64 && t->param_count <= 2;

    for (i = 0; ok && i < t->param_count; i++) {
        ok = t->params[i] == strs;
    }
    if (!ok) {
        error_at(c, it->name_pos, "`main` must be fn main() -> int, fn main("
                 "args: []str) -> int or fn main(args: []str, env: []str) -> "
                 "int");
    }
}

static enum symbol_kind item_symbol_kind(enum item_kind kind)
{
    switch (kind) {
    case ITEM_FN: return SYMBOL_FN;
    case ITEM_EXTERN_FN: return SYMBOL_EXTERN_FN;
    case ITEM_STRUCT:
    case ITEM_UNION:
    case ITEM_ENUM:
    case ITEM_CLASS: return SYMBOL_STRUCT;
    default: return SYMBOL_CONST;
    }
}

/* An import declares the last segment of the module path, or its alias,
   at module level. */
static void declare_import(struct checker *c, const struct import *imp)
{
    const struct name *module = &imp->module;
    const struct interface *lib;
    struct symbol *sym;
    struct name local = *module;
    size_t i;

    for (i = 0; i < module->length; i++) {
        if (module->text[i] >= 'A' && module->text[i] <= 'Z') {
            error_at(c, imp->module_pos, "the module path `%.*s` is not "
                     "lowercase", (int)module->length, module->text);
            return;
        }
        if (module->text[i] == '.') {
            local.text = module->text + i + 1;
            local.length = module->length - i - 1;
        }
    }
    if (same_name(module, &c->module_name)) {
        error_at(c, imp->module_pos, "`%.*s` cannot import itself",
                 (int)module->length, module->text);
        return;
    }
    lib = find_library(c, module);
    if (lib == NULL) {
        error_at(c, imp->module_pos, "cannot find module `%.*s`",
                 (int)module->length, module->text);
        return;
    }
    if (depends_on(c, lib, &c->module_name, 0)) {
        error_at(c, imp->module_pos, "`%.*s` depends on `%.*s`, so the import "
                 "forms a cycle", (int)module->length, module->text,
                 (int)c->module_name.length, c->module_name.text);
        return;
    }
    sym = declare(c, SYMBOL_MODULE,
                  imp->alias.length > 0 ? &imp->alias : &local,
                  imp->module_pos, "`%.*s` is already declared");
    if (sym != NULL) {
        sym->home = lib;
    }
}

static void check_export(struct checker *c, struct item *it);

bool sema_check(struct module *module, const char *module_name,
                const char *package,
                const struct interface *const *libraries,
                size_t library_count, struct types *types,
                struct arena *arena, struct diagnostics *diags,
                bool whole_program)
{
    struct checker c;
    size_t i;
    size_t j;

    memset(&c, 0, sizeof c);
    c.types = types;
    c.arena = arena;
    c.diags = diags;
    c.module = module;
    c.module_name.text = module_name;
    c.module_name.length = strlen(module_name);
    c.package = package != NULL ? package : module_name;
    c.libraries = libraries;
    c.library_count = library_count;
    c.scope = &c.module_scope;
    c.ok = true;
    c.whole_program = whole_program;
    declare_root(&c);

    for (i = 0; i < module->import_count; i++) {
        declare_import(&c, &module->imports[i]);
    }

    /* Declare every item first, so each can be used before its
       declaration. */
    for (i = 0; i < module->item_count; i++) {
        struct item *it = module->items[i];
        it->symbol = declare(&c, item_symbol_kind(it->kind), &it->name,
                             it->name_pos, "`%.*s` is already declared");
        if (it->symbol == NULL) {
            continue;
        }
        it->symbol->item = it;
        it->symbol->variadic = it->variadic;
        it->symbol->worker = it->worker;
        it->symbol->exported = it->exported;
        it->symbol->doc = it->doc;
        if (it->kind == ITEM_STRUCT || it->kind == ITEM_UNION) {
            it->symbol->type = types_struct(types, c.module_name, it->name);
            it->symbol->type->is_union = it->kind == ITEM_UNION;
        } else if (it->kind == ITEM_CLASS) {
            it->symbol->type = types_struct(types, c.module_name, it->name);
            it->symbol->type->kind = TYPE_CLASS;
            it->symbol->type->has_abstract = it->is_abstract;
            it->symbol->type->is_final = it->is_final;
        } else if (it->kind == ITEM_ENUM) {
            /* DESIGN: the underlying type of an enum is c_int unless the
               declaration names one, as an unfixed C enum is an int. */
            struct type *base = it->base != NULL
                                    ? resolve_type(&c, it->base)
                                    : types_builtin(types, TYPE_I32);
            it->symbol->type =
                types_enum(types, c.module_name, it->name, base);
        }
        if (it->symbol->type != NULL) {
            it->symbol->type->members = it->members;
            it->symbol->type->member_count = it->member_count;
        }
    }

    /* DESIGN: a class names its base in its header. The base is nested
       whole at offset 0, so the checker resolves it before the fields,
       which put the base at index 0. A base that is not a class, or that
       is `final`, is refused. A class without `inherits` takes the root
       `anti.rt.Object`, which the compiler declares. */
    for (i = 0; i < module->item_count; i++) {
        struct item *it = module->items[i];
        struct symbol *base;
        if (it->kind != ITEM_CLASS || it->symbol == NULL) {
            continue;
        }
        if (it->base_name.length == 0) {
            it->symbol->type->base = types_object(types);
            continue;
        }
        base = scope_find_local(&c.module_scope, &it->base_name);
        if (base == NULL || base->kind != SYMBOL_STRUCT ||
            base->type == NULL || base->type->kind != TYPE_CLASS) {
            error_at(&c, it->base_pos, "`%.*s` is not a class",
                     (int)it->base_name.length, it->base_name.text);
            continue;
        }
        if (base->type->is_final) {
            error_at(&c, it->base_pos,
                     "`%.*s` cannot inherit `final` class `%.*s`",
                     (int)it->name.length, it->name.text,
                     (int)it->base_name.length, it->base_name.text);
            continue;
        }
        it->symbol->type->base = base->type;
    }

    /* DESIGN: the values of an enum live in its fields, each with the
       enum as its type. A value without `=` follows the one before it,
       starting at 0, as C numbers an enumerator. */
    for (i = 0; i < module->item_count; i++) {
        struct item *it = module->items[i];
        struct struct_field *values;
        if (it->symbol == NULL || it->kind != ITEM_ENUM) {
            continue;
        }
        values = arena_alloc(arena, (it->param_count + 1) * sizeof *values);
        for (j = 0; j < it->param_count; j++) {
            size_t k;
            memset(&values[j], 0, sizeof values[j]);
            values[j].name = it->params[j].name;
            values[j].pos = it->params[j].pos;
            values[j].doc = it->params[j].doc;
            values[j].type = it->symbol->type;
            values[j].value = it->params[j].value;
            /* DESIGN: a value without `=` follows the one before it and
               the first is 0, as C numbers an enumerator. */
            values[j].number = j == 0 ? 0 : values[j - 1].number + 1;
            if (it->params[j].value != NULL) {
                struct const_value v;
                struct type *base = it->symbol->type->base;
                if (require(&c, it->params[j].value,
                            check_expr(&c, it->params[j].value, base), base) &&
                    eval_const(&c, it->params[j].value, &v) &&
                    v.kind == CONST_INT) {
                    values[j].number = v.as.integer;
                }
            }
            for (k = 0; k < j; k++) {
                if (same_name(&values[k].name, &values[j].name)) {
                    error_at(&c, values[j].pos, "enum `%.*s` has two values "
                             "named `%.*s`", (int)it->name.length,
                             it->name.text, (int)values[j].name.length,
                             values[j].name.text);
                }
            }
        }
        types_set_fields(types, it->symbol->type, values, it->param_count);
    }
    for (i = 0; i < module->item_count; i++) {
        struct item *it = module->items[i];
        struct struct_field *fields;
        size_t base_fields;
        if (it->symbol == NULL ||
            (it->kind != ITEM_STRUCT && it->kind != ITEM_UNION &&
             it->kind != ITEM_CLASS)) {
            continue;
        }
        /* DESIGN: a class carries its base as field 0, named `super`.
           The name is a keyword, so no declared field collides with it,
           and `self.super` is then ordinary field access. The base is
           nested whole, so the C rules of chapter 18 place it at offset
           0 and the class's own fields after it. */
        base_fields = it->kind == ITEM_CLASS ? 1 : 0;
        fields = arena_alloc(arena,
                             (it->param_count + base_fields) * sizeof *fields);
        if (base_fields != 0) {
            static const char super_text[] = "super";
            memset(&fields[0], 0, sizeof fields[0]);
            fields[0].name.text = super_text;
            fields[0].name.length = sizeof super_text - 1;
            fields[0].pos = it->name_pos;
            fields[0].form = FIELD_BASE;
            fields[0].type = it->symbol->type->base;
        }
        fields += base_fields;
        for (j = 0; j < it->param_count; j++) {
            size_t k;
            fields[j].name = it->params[j].name;
            fields[j].pos = it->params[j].pos;
            fields[j].doc = it->params[j].doc;
            fields[j].form = it->params[j].form;
            fields[j].vis = it->params[j].vis;
            fields[j].owned = it->params[j].owned;
            fields[j].atomic = it->params[j].atomic;
            fields[j].writable = it->params[j].writable;

            fields[j].value = it->params[j].value;
            c.target_sized = true;
            fields[j].type = resolve_type(&c, it->params[j].type);
            c.target_sized = false;
            if (it->params[j].bits != NULL ||
                type_field_is_unit_break(&fields[j])) {
                fields[j].bits = bitfield_width(&c, &it->params[j],
                                                fields[j].type);
            }
            if (it->kind == ITEM_UNION && type_field_is_unit_break(&fields[j])) {
                error_at(&c, fields[j].pos, "a union holds no zero-width "
                         "bitfield");
            }
            for (k = 0; k < j && !type_field_is_unit_break(&fields[j]); k++) {
                if (same_name(&fields[k].name, &fields[j].name)) {
                    error_at(&c, fields[j].pos, "%s `%.*s` has two fields "
                             "named `%.*s`",
                             it->kind == ITEM_UNION ? "union" : "struct",
                             (int)it->name.length, it->name.text,
                             (int)fields[j].name.length, fields[j].name.text);
                }
            }
        }
        /* DESIGN: `own` says the object frees the memory behind the
           field, so the field holds an address the object alone reaches.
           `str` is immutable and shared, and a class or struct field is
           inline and owned by the object already. */
        for (j = 0; j < it->param_count; j++) {
            const struct type *ft = fields[j].type;
            if (!fields[j].owned || is_error(ft)) {
                continue;
            }
            if (ft->kind != TYPE_POINTER && ft->kind != TYPE_SLICE) {
                error_at(&c, fields[j].pos, "`own` needs a pointer or a "
                         "slice, and `%.*s` has type `%s`",
                         (int)fields[j].name.length, fields[j].name.text,
                         tn(ft));
            }
        }
        /* A default is checked against the type of its field, so the
           value that lowering writes is complete and typed. */
        for (j = 0; j < it->param_count; j++) {
            if (it->params[j].value == NULL) {
                continue;
            }
            require(&c, it->params[j].value,
                    check_expr(&c, it->params[j].value, fields[j].type),
                    fields[j].type);
        }
        types_set_fields(types, it->symbol->type, fields - base_fields,
                         it->param_count + base_fields);
        it->symbol->type->packed = it->packed;
        if (it->align != NULL) {
            it->symbol->type->align = alignment(&c, it->align);
        }
    }
    for (i = 0; i < module->item_count; i++) {
        struct item *it = module->items[i];
        if (it->symbol != NULL &&
            (it->kind == ITEM_STRUCT || it->kind == ITEM_UNION) &&
            types_find_cycle(it->symbol->type) != NULL) {
            error_at(&c, it->name_pos, "%s `%.*s` contains itself",
                     it->kind == ITEM_UNION ? "union" : "struct",
                     (int)it->name.length, it->name.text);
        }
    }

    for (i = 0; i < module->item_count; i++) {
        struct item *it = module->items[i];
        if (it->symbol != NULL &&
            (it->kind == ITEM_FN || it->kind == ITEM_EXTERN_FN)) {
            it->symbol->type = function_type(&c, it);
        }
    }
    /* DESIGN: a function of a struct body carries the name `T.f`, so its
       symbol is `module.T.f`, one segment more than a free function. It
       is not declared in the module scope, because it is reached through
       its type. */
    for (i = 0; i < module->item_count; i++) {
        struct item *it = module->items[i];
        if (it->symbol == NULL || it->symbol->type == NULL) {
            continue;
        }
        for (j = 0; j < it->member_count; j++) {
            struct item *m = it->members[j];
            struct symbol *sym = arena_alloc(arena, sizeof *sym);
            struct text qualified = {0};
            char *text;
            size_t k;
            for (k = 0; k < j; k++) {
                if (same_name(&it->members[k]->name, &m->name)) {
                    error_at(&c, m->name_pos, "`%.*s` declares `%.*s` twice",
                             (int)it->name.length, it->name.text,
                             (int)m->name.length, m->name.text);
                }
            }
            text_appendf(&qualified, "%.*s.%.*s", (int)it->name.length,
                         it->name.text, (int)m->name.length, m->name.text);
            text = arena_alloc(arena, qualified.length + 1);
            memcpy(text, qualified.data, qualified.length + 1);
            text_free(&qualified);
            if (m->contract == FN_ABSTRACT) {
                it->symbol->type->has_abstract = true;
            }
            /* A static field is a global, which lowering writes, and a
               constant of a body is folded where it is named. */
            /* DESIGN: every public function of an export class has the
               C symbol `Class_fn`, because the generated header declares
               a prototype for each one. */
            if (it->exported && m->kind == ITEM_FN && m->pub) {
                m->exported = true;
            }
            sym->kind = m->kind != ITEM_CONST  ? SYMBOL_FN
                        : m->is_static         ? SYMBOL_GLOBAL
                                               : SYMBOL_CONST;
            sym->name.text = text;
            sym->name.length = strlen(text);
            sym->pos = m->name_pos;
            sym->item = m;
            sym->exported = m->exported;
            sym->doc = m->doc;

            m->symbol = sym;
            if (m->kind == ITEM_FN) {
                sym->type = function_type(&c, m);
            }
        }
    }
    for (i = 0; i < module->item_count; i++) {
        struct item *it = module->items[i];
        if (it->symbol != NULL &&
            (it->kind == ITEM_STRUCT || it->kind == ITEM_UNION ||
             it->kind == ITEM_CLASS || it->kind == ITEM_ENUM)) {
            it->symbol->type->item_exported = it->exported;
        }
    }
    for (i = 0; i < module->item_count; i++) {
        struct item *it = module->items[i];
        if (it->symbol != NULL && it->kind == ITEM_CONST) {
            const_symbol(&c, it->symbol, it->name_pos);
        }
    }
    /* DESIGN: a singleton has one instance, which `Config.get()` makes
       on the first call. The program never allocates one, so the checker
       declares `get` itself, before any body names it. */
    for (i = 0; i < module->item_count; i++) {
        struct item *it = module->items[i];
        if (it->kind == ITEM_CLASS && it->is_singleton &&
            it->symbol != NULL && it->symbol->type != NULL) {
            declare_get(&c, it);
        }
    }
    for (i = 0; i < module->item_count; i++) {
        struct item *it = module->items[i];
        if (it->symbol == NULL || it->kind != ITEM_FN) {
            continue;
        }
        check_function(&c, it);
        if (name_is(&it->name, "main") && !is_error(it->symbol->type)) {
            check_main(&c, it);
        }
    }
    /* DESIGN: `implements name: I` places a sub-object of the abstract
       class I inside the class. Only an abstract class may be
       implemented, and one class implements an interface once, so that
       every name it provides has one path. */
    for (i = 0; i < module->item_count; i++) {
        struct item *it = module->items[i];
        struct type *t = it->symbol != NULL ? it->symbol->type : NULL;
        if (!type_has_fields(t)) {
            continue;
        }
        for (j = 0; j < t->field_count; j++) {
            const struct type *iface = t->fields[j].type;
            const struct type *chain;
            size_t k;
            if (t->fields[j].form != FIELD_IMPL) {
                continue;
            }
            if (iface == NULL || iface->kind != TYPE_CLASS ||
                !iface->has_abstract) {
                error_at(&c, t->fields[j].pos, "`%s` is not abstract and "
                         "cannot be implemented", tn(iface));
                continue;
            }
            for (k = 0; k < j; k++) {
                if (t->fields[k].form == FIELD_IMPL &&
                    t->fields[k].type == iface) {
                    error_at(&c, t->fields[j].pos,
                             "`%.*s` implements `%s` twice",
                             (int)it->name.length, it->name.text, tn(iface));
                }
            }
            for (chain = inherited(t); chain != NULL;
                 chain = inherited(chain)) {
                for (k = 0; k < chain->field_count; k++) {
                    if (chain->fields[k].form == FIELD_IMPL &&
                        chain->fields[k].type == iface) {
                        error_at(&c, t->fields[j].pos, "`%.*s` implements "
                                 "`%s`, which `%.*s` implements already",
                                 (int)it->name.length, it->name.text,
                                 tn(iface), (int)chain->name.length,
                                 chain->name.text);
                    }
                }
            }
        }
    }
    /* A plain or `use` field of an abstract class is refused here,
       after every class knows whether a contract reaches it. The base
       and an interface sub-object are the two places an abstract class
       is a value, so they are skipped. */
    for (i = 0; i < module->item_count; i++) {
        struct item *it = module->items[i];
        struct type *t = it->symbol != NULL ? it->symbol->type : NULL;
        if (!type_has_fields(t)) {
            continue;
        }
        for (j = 0; j < t->field_count; j++) {
            char what[96];
            if (t->fields[j].form == FIELD_BASE ||
                t->fields[j].form == FIELD_TABLE ||
                t->fields[j].form == FIELD_IMPL) {
                continue;
            }
            snprintf(what, sizeof what, "the field `%.*s`",
                     (int)t->fields[j].name.length, t->fields[j].name.text);
            refuse_abstract_value(&c, t->fields[j].pos, what,
                                  t->fields[j].type);
        }
    }
    /* DESIGN: a contract is declared with `abstract fn` and filled with
       `concrete fn` of the same signature. The checker walks the chain of
       `inherits` fields of every struct and refuses one that leaves a
       contract unfilled. */
    for (i = 0; i < module->item_count; i++) {
        struct item *it = module->items[i];
        struct type *t = it->symbol != NULL ? it->symbol->type : NULL;
        const struct type *base;
        if (!type_has_fields(t)) {
            continue;
        }
        for (j = 0; j < it->member_count; j++) {
            struct item *m = it->members[j];
            const struct item *above = NULL;
            size_t k;
            if (m->kind != ITEM_FN) {
                continue;
            }
            for (base = inherited(t); base != NULL; base = inherited(base)) {
                const struct item *found = find_member(base, &m->name);
                if (found != NULL && found->kind == ITEM_FN) {
                    above = found;
                    break;
                }
            }
            /* An interface declares functions the class fills, so a
               `concrete fn` matches there as well as in the base
               chain. */
            for (k = 0; above == NULL && k < t->field_count; k++) {
                const struct type *iface;
                if (t->fields[k].form != FIELD_IMPL) {
                    continue;
                }
                for (iface = t->fields[k].type;
                     iface != NULL && above == NULL;
                     iface = inherited(iface)) {
                    const struct item *found = find_member(iface, &m->name);
                    if (found != NULL && found->kind == ITEM_FN) {
                        above = found;
                    }
                }
            }
            if (m->contract == FN_ABSTRACT) {
                continue;
            }
            if (m->contract == FN_CONCRETE && above == NULL) {
                error_at(&c, m->name_pos, "`concrete fn %.*s` of `%.*s` fills "
                         "no abstract function", (int)m->name.length,
                         m->name.text, (int)it->name.length, it->name.text);
            } else if (m->contract == FN_PLAIN && above != NULL &&
                       above->contract != FN_PLAIN) {
                error_at(&c, m->name_pos, "`%.*s` of `%.*s` matches an "
                         "abstract function and needs `concrete`",
                         (int)m->name.length, m->name.text,
                         (int)it->name.length, it->name.text);
            }
        }
        /* DESIGN: a struct may not redeclare a name that its chain
           already has. A `concrete fn` is the exception, because it
           fills the abstract function of that name. */
        for (base = inherited(t); base != NULL; base = inherited(base)) {
            for (j = 0; j < t->field_count; j++) {
                if (t->fields[j].form == FIELD_BASE ||
                    t->fields[j].form == FIELD_TABLE) {
                    continue;
                }
                if (find_field(base, &t->fields[j].name) != NULL ||
                    find_member(base, &t->fields[j].name) != NULL) {
                    error_at(&c, t->fields[j].pos, "`%.*s` already has `%.*s`",
                             (int)base->name.length, base->name.text,
                             (int)t->fields[j].name.length,
                             t->fields[j].name.text);
                }
            }
            for (j = 0; j < it->member_count; j++) {
                const struct item *m = it->members[j];
                /* `construct` and `destruct` repeat down a chain by
                   design, because the compiler runs one body per level.
                   A `concrete fn` replaces an entry, and an `abstract
                   fn` re-opens one, which an interface does when it
                   names a function of the root. */
                if (m->contract != FN_PLAIN ||
                    name_is(&m->name, "construct") ||
                    name_is(&m->name, "destruct")) {
                    continue;
                }
                if (find_field(base, &m->name) != NULL ||
                    find_member(base, &m->name) != NULL) {
                    error_at(&c, m->name_pos, "`%.*s` already has `%.*s`",
                             (int)base->name.length, base->name.text,
                             (int)m->name.length, m->name.text);
                }
            }
        }
        /* DESIGN: every abstract function of the chain needs a concrete
           one at or below the class that declares it. An abstract class
           may leave one open. It is never a complete value, and every
           class below it is checked here. */
        for (base = inherited(t); base != NULL && !it->is_abstract;
             base = inherited(base)) {
            for (j = 0; j < base->member_count; j++) {
                const struct item *a = base->members[j];
                const struct item *filled;
                if (a->kind != ITEM_FN || a->contract != FN_ABSTRACT) {
                    continue;
                }
                filled = find_member(t, &a->name);
                if (filled == NULL || filled->contract != FN_CONCRETE) {
                    error_at(&c, it->name_pos, "`%.*s` lacks `concrete fn "
                             "%.*s`", (int)it->name.length, it->name.text,
                             (int)a->name.length, a->name.text);
                }
            }
        }
        /* An `operator fn` carries one of the fourteen names the table
           holds, and nothing else. */
        for (j = 0; j < it->member_count; j++) {
            const struct item *m = it->members[j];
            if (m->kind == ITEM_FN && m->is_operator &&
                !operator_named(&m->name)) {
                error_at(&c, m->name_pos, "`operator fn` takes one of `add`, "
                         "`sub`, `mul`, `div`, `rem`, `neg`, `eq`, `lt`, "
                         "`and`, `or`, `xor`, `shl`, `shr` and `not`");
            }
        }
        /* DESIGN: one `construct` per class, and every alternative is a
           static function with a name of its own. A `construct` without
           arguments cannot fail and returns nothing. */
        {
            const struct item *first = NULL;
            for (j = 0; j < it->member_count; j++) {
                struct item *m = it->members[j];
                if (m->kind != ITEM_FN || !name_is(&m->name, "construct")) {
                    continue;
                }
                if (first != NULL) {
                    error_at(&c, m->name_pos, "`%.*s` has one `construct`, "
                             "and every other maker is a static function",
                             (int)it->name.length, it->name.text);
                }
                first = m;
                if (!m->has_self) {
                    error_at(&c, m->name_pos,
                             "`construct` takes `self` as its first "
                             "parameter");
                } else if (m->param_count == 0 && m->result != NULL) {
                    error_at(&c, m->name_pos, "a `construct` without "
                             "arguments cannot fail and returns nothing");
                }
            }
        }
        /* DESIGN: the qualifier of a `concrete fn` names the table it
           fills: the class itself, a class of its chain, or an interface
           it implements. Any other name reaches no table. */
        for (j = 0; j < it->member_count; j++) {
            const struct item *m = it->members[j];
            const struct type *up;
            bool found = false;
            size_t k;
            if (m->kind != ITEM_FN || m->qualifier.length == 0) {
                continue;
            }
            for (up = t; up != NULL && !found;
                 up = up->kind == TYPE_CLASS ? up->base : NULL) {
                found = same_name(&m->qualifier, &up->name);
                for (k = 0; k < up->field_count && !found; k++) {
                    const struct type *iface;
                    if (up->fields[k].form != FIELD_IMPL) {
                        continue;
                    }
                    for (iface = up->fields[k].type; iface != NULL && !found;
                         iface = inherited(iface)) {
                        found = same_name(&m->qualifier, &iface->name);
                    }
                }
            }
            if (!found) {
                error_at(&c, m->qualifier_pos, "`%.*s` is no base and no "
                         "interface of `%.*s`", (int)m->qualifier.length,
                         m->qualifier.text, (int)it->name.length,
                         it->name.text);
            }
        }
        /* An interface leaves its functions open, and the class that
           implements it fills them. The chain of the interface counts,
           because an interface may inherit another abstract class. */
        for (j = 0; j < t->field_count && !it->is_abstract; j++) {
            const struct type *iface;
            if (t->fields[j].form != FIELD_IMPL) {
                continue;
            }
            for (iface = t->fields[j].type; iface != NULL;
                 iface = inherited(iface)) {
                size_t k;
                for (k = 0; k < iface->member_count; k++) {
                    const struct item *a = iface->members[k];
                    const struct item *filled;
                    if (a->kind != ITEM_FN || a->contract != FN_ABSTRACT) {
                        continue;
                    }
                    filled = find_member(t, &a->name);
                    if (filled == NULL || filled->contract != FN_CONCRETE) {
                        error_at(&c, it->name_pos, "`%.*s` lacks `concrete "
                                 "fn %.*s` of `%s`", (int)it->name.length,
                                 it->name.text, (int)a->name.length,
                                 a->name.text, tn(iface));
                    }
                }
            }
        }
    }
    /* A function of a body is checked like a free function, with `self`
       declared as a parameter of type *T. */
    for (i = 0; i < module->item_count; i++) {
        struct item *it = module->items[i];
        for (j = 0; j < it->member_count; j++) {
            struct item *m = it->members[j];
            if (m->symbol == NULL || m->kind != ITEM_FN ||
                m->body == NULL) {
                continue;
            }
            check_function(&c, m);
        }
    }
    /* DESIGN: the pass that only reports runs last, over every function
       a worker can reach and over the abstract classes of the program.
       It changes nothing, so a build that skips it still compiles the
       same program. */
    for (i = 0; i < module->item_count; i++) {
        struct item *it = module->items[i];
        struct worker_walk walk;
        if (it->kind != ITEM_FN || !it->worker || it->body == NULL) {
            continue;
        }
        memset(&walk, 0, sizeof walk);
        walk.c = &c;
        walk.worker = it;
        walk_function(&walk, it);
        free(walk.seen);
    }
    /* An abstract class that no class of the program fills has no value
       and no use. Release mode sees every class, so it can say so. */
    if (c.whole_program) {
        for (i = 0; i < module->item_count; i++) {
            struct item *it = module->items[i];
            struct type *t = it->symbol != NULL ? it->symbol->type : NULL;
            if (it->kind != ITEM_CLASS || !it->is_abstract || t == NULL) {
                continue;
            }
            if (!filled_somewhere(&c, t)) {
                char message[96];
                snprintf(message, sizeof message,
                         "`%.*s` is abstract and no class fills it",
                         (int)it->name.length, it->name.text);
                diagnostics_warn(c.diags, it->name_pos.line,
                                 it->name_pos.column, "%s", message);
            }
        }
    }
    for (i = 0; i < module->item_count; i++) {
        if (module->items[i]->symbol != NULL && module->items[i]->exported) {
            check_export(&c, module->items[i]);
        }
    }
    free(c.module_scope.entries);
    return c.ok;
}

static const char *keep_name(struct arena *arena, const char *text,
                             size_t length)
{
    char *copy = arena_alloc(arena, length + 1);

    memcpy(copy, text, length);
    return copy;
}

/* Whether a value of type t has a C representation. That is a scalar
   other than char, a pointer to such a type, an exported struct or union,
   or a function pointer of such types. A struct field may also be a
   fixed-size array of such a type. Sets *hidden to a struct that is not
   exported. */
static bool c_representable(const struct type *t, bool field,
                            const struct type **hidden)
{
    size_t i;

    switch (t->kind) {
    case TYPE_CHAR:
    case TYPE_STR:
    case TYPE_SLICE:
    case TYPE_NULL:
        return false;
    case TYPE_ARRAY:
        return field && t->length_of == NULL &&
               c_representable(t->element, true, hidden);
    case TYPE_POINTER:
        return c_representable(t->element, false, hidden);
    case TYPE_FN:
        for (i = 0; i < t->param_count; i++) {
            if (!c_representable(t->params[i], false, hidden)) {
                return false;
            }
        }
        return t->result->kind == TYPE_VOID ||
               c_representable(t->result, false, hidden);
    case TYPE_STRUCT:
    case TYPE_CLASS:
        if (t->item_exported) {
            return true;
        }
        *hidden = t;
        return false;
    /* An enum is its underlying integer, which the header writes as an
       enum of the same name. */
    case TYPE_ENUM:
        return true;
    default:
        return true;
    }
}

/* Report a type in an export signature or struct that C cannot represent.
   what names the place, as `the parameter `s` of export fn `f``. */
static void check_c_type(struct checker *c, struct pos pos, const char *what,
                         const struct type *t, bool field)
{
    const struct type *hidden = NULL;

    if (is_error((struct type *)t) || c_representable(t, field, &hidden)) {
        return;
    }
    if (hidden != NULL) {
        error_at(c, pos, "%s has type `%s`, and `%s` is not exported", what,
                 tn((struct type *)t), tn((struct type *)hidden));
    } else {
        error_at(c, pos, "%s has type `%s`, which C cannot represent", what,
                 tn((struct type *)t));
    }
}

/* DESIGN: an export item has a C symbol and appears in a C header. Its
   types have a C representation, its name is not main, and no other module
   exports the same name. */
static void check_export(struct checker *c, struct item *it)
{
    const struct type *t = it->symbol->type;
    char what[160];
    size_t i;
    size_t j;

    switch (it->kind) {
    case ITEM_FN:
        if (name_is(&it->name, "main")) {
            error_at(c, it->name_pos, "`main` is the entry of the program and "
                     "cannot be exported");
            return;
        }
        for (i = 0; i < it->param_count && t->kind == TYPE_FN; i++) {
            snprintf(what, sizeof what, "the parameter `%.*s` of export fn "
                     "`%.*s`", (int)it->params[i].name.length,
                     it->params[i].name.text, (int)it->name.length,
                     it->name.text);
            check_c_type(c, it->params[i].pos, what, t->params[i], false);
        }
        if (it->result != NULL && t->kind == TYPE_FN &&
            t->result->kind != TYPE_VOID) {
            snprintf(what, sizeof what, "the result of export fn `%.*s`",
                     (int)it->name.length, it->name.text);
            check_c_type(c, it->result->pos, what, t->result, false);
        }
        for (i = 0; i < c->library_count; i++) {
            const struct interface *lib = c->libraries[i];
            for (j = 0; j < lib->item_count; j++) {
                const struct symbol *other = lib->items[j];
                if (other->exported && other->kind == SYMBOL_FN &&
                    same_name(&other->name, &it->name)) {
                    error_at(c, it->name_pos, "export fn `%.*s` has the symbol "
                             "of export fn `%.*s` in module `%s`",
                             (int)it->name.length, it->name.text,
                             (int)other->name.length, other->name.text,
                             lib->module);
                }
            }
        }
        return;
    /* DESIGN: an export class crosses as its layout, its table type and
       one prototype per public function. Every field and every public
       signature therefore follows the export rule, and the base and the
       table pointer are the compiler's own and always cross. */
    case ITEM_CLASS:
        for (i = 0; i < t->field_count; i++) {
            if (t->fields[i].form == FIELD_BASE ||
                t->fields[i].form == FIELD_TABLE) {
                continue;
            }
            snprintf(what, sizeof what,
                     "the field `%.*s` of export class `%.*s`",
                     (int)t->fields[i].name.length, t->fields[i].name.text,
                     (int)it->name.length, it->name.text);
            check_c_type(c, t->fields[i].pos, what, t->fields[i].type, true);
        }
        for (i = 0; i < it->member_count; i++) {
            const struct item *m = it->members[i];
            const struct type *ft = m->symbol != NULL ? m->symbol->type : NULL;
            if (m->kind != ITEM_FN || !m->pub || ft == NULL ||
                ft->kind != TYPE_FN) {
                continue;
            }
            for (j = m->has_self ? 1 : 0; j < ft->param_count; j++) {
                snprintf(what, sizeof what, "the parameter %zu of `%.*s.%.*s`",
                         j, (int)it->name.length, it->name.text,
                         (int)m->name.length, m->name.text);
                check_c_type(c, m->name_pos, what, ft->params[j], false);
            }
            if (ft->result->kind != TYPE_VOID) {
                snprintf(what, sizeof what, "the result of `%.*s.%.*s`",
                         (int)it->name.length, it->name.text,
                         (int)m->name.length, m->name.text);
                check_c_type(c, m->name_pos, what, ft->result, false);
            }
        }
        return;
    case ITEM_STRUCT:
    case ITEM_UNION:
        for (i = 0; i < t->field_count; i++) {
            snprintf(what, sizeof what, "the field `%.*s` of export %s `%.*s`",
                     (int)t->fields[i].name.length, t->fields[i].name.text,
                     it->kind == ITEM_UNION ? "union" : "struct",
                     (int)it->name.length, it->name.text);
            check_c_type(c, t->fields[i].pos, what, t->fields[i].type, true);
        }
        /* DESIGN: the header writes align(N) as _Alignas on the first
           field, which keeps offset 0 on every target. C refuses
           _Alignas on a bitfield. */
        if (it->align != NULL && t->field_count > 0 &&
            (t->fields[0].bits > 0 || type_field_is_unit_break(&t->fields[0]))) {
            error_at(c, t->fields[0].pos, "the first field `%.*s` of export "
                     "%s `%.*s` is a bitfield, and the C header aligns the "
                     "struct on its first field",
                     (int)t->fields[0].name.length, t->fields[0].name.text,
                     it->kind == ITEM_UNION ? "union" : "struct",
                     (int)it->name.length, it->name.text);
        }
        return;
    case ITEM_CONST:
        if (!is_error((struct type *)t) && !type_is_numeric(t) &&
            t->kind != TYPE_BOOL && t->kind != TYPE_STR) {
            error_at(c, it->type->pos, "export const `%.*s` has type `%s`, and "
                     "an export const is a number, a bool or a str",
                     (int)it->name.length, it->name.text, tn((struct type *)t));
        }
        return;
    default:
        return;
    }
}

/* DESIGN: doc warnings belong to anti check, which passes --doc-warnings.
   Without that option antic says nothing about documentation. */
void sema_doc_warnings(const struct module *module, struct diagnostics *diags)
{
    size_t i;

    for (i = 0; i < module->item_count; i++) {
        const struct item *it = module->items[i];
        if (it->pub && it->note.length > 0 && it->doc.length == 0) {
            diagnostics_warn(diags, it->name_pos.line, it->name_pos.column,
                             "the pub item `%.*s` has a `//#` note and no "
                             "`///` comment", (int)it->name.length,
                             it->name.text);
        }
    }
    for (i = 0; i < module->dropped_count; i++) {
        const struct dropped_doc *d = &module->dropped[i];
        diagnostics_warn(diags, d->pos.line, d->pos.column,
                         d->module_form
                             ? "the `%.*s` comment is dropped, because it "
                               "stands after the first import or item"
                             : "the `%.*s` comment is dropped, because no "
                               "item or field follows it",
                         (int)d->marker.length, d->marker.text);
    }
}

void sema_interface(const struct module *module, const char *module_name,
                    struct arena *arena, struct interface *out)
{
    size_t i;

    memset(out, 0, sizeof *out);
    out->module = keep_name(arena, module_name, strlen(module_name));
    out->doc = keep_name(arena, module->doc.length > 0 ? module->doc.text : "",
                         module->doc.length);
    out->package.name = out->module;
    out->package.version = "0.0.0";
    out->package.license = "";
    out->package.license_text = "";
    out->imports = arena_alloc(arena, (module->import_count + 1) *
                                          sizeof *out->imports);
    for (i = 0; i < module->import_count; i++) {
        out->imports[i] = keep_name(arena, module->imports[i].module.text,
                                    module->imports[i].module.length);
    }
    out->import_count = module->import_count;
    out->items = arena_alloc(arena, (module->item_count + 1) *
                                        sizeof *out->items);
    for (i = 0; i < module->item_count; i++) {
        const struct item *it = module->items[i];
        struct symbol *sym;
        /* DESIGN: an `internal` item goes into the interface marked
           as internal. A module of the same package may then import it,
           and a module of another package may not. */
        if ((!it->pub && it->vis != VIS_INTERNAL) || it->symbol == NULL) {
            continue;
        }
        sym = arena_alloc(arena, sizeof *sym);
        *sym = *it->symbol;
        sym->internal = it->vis == VIS_INTERNAL;
        /* DESIGN: an interface keeps the parameter names of a function,
           which the generated header and anti doc print. */
        if (it->kind == ITEM_FN || it->kind == ITEM_EXTERN_FN) {
            struct name *names =
                arena_alloc(arena, (it->param_count + 1) * sizeof *names);
            size_t j;
            for (j = 0; j < it->param_count; j++) {
                names[j].text = keep_name(arena, it->params[j].name.text,
                                          it->params[j].name.length);
                names[j].length = it->params[j].name.length;
            }
            sym->params = names;
        }
        sym->item = NULL;
        sym->home = out;
        out->items[out->item_count++] = sym;
    }
}
