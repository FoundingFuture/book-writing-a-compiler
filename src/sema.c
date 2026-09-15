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
    const struct interface *const *libraries;
    size_t library_count;
    struct scope module_scope;
    struct scope *scope;
    struct item *function;      /* the function whose body is checked */
    int loop_depth;
    bool target_sized;          /* a symbolic array length is allowed */
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

static struct symbol *library_item(const struct interface *lib,
                                   const struct name *name)
{
    size_t i;

    for (i = 0; i < lib->item_count; i++) {
        if (same_name(&lib->items[i]->name, name)) {
            return lib->items[i];
        }
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

static bool require(struct checker *c, const struct expr *e, struct type *got,
                    struct type *expected);

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
    sym = library_item(lib, name);
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

/* The type of a function item, fn(params) -> result. */
static struct type *function_type(struct checker *c, struct item *it)
{
    struct type **params =
        arena_alloc(c->arena, (it->param_count + 1) * sizeof *params);
    struct type *result = builtin(c, TYPE_VOID);
    size_t i;

    for (i = 0; i < it->param_count; i++) {
        params[i] = resolve_type(c, it->params[i].type);
        if (is_error(params[i])) {
            return params[i];
        }
    }
    if (it->result != NULL && is_error(result = resolve_type(c, it->result))) {
        return result;
    }
    return types_fn(c->types, params, it->param_count, result);
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
               (base->kind == TYPE_STRUCT && is_place(e->as.field.base));
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
static bool require(struct checker *c, const struct expr *e, struct type *got,
                    struct type *expected)
{
    if (is_error(got) || is_error(expected) || got == expected) {
        return !is_error(got);
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

static struct type *check_unary(struct checker *c, struct expr *e,
                                struct type *expected)
{
    struct expr *operand = e->as.unary.operand;
    struct type *t;

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

static struct type *check_binary(struct checker *c, struct expr *e,
                                 struct type *expected)
{
    enum token_kind op = e->as.binary.op;
    struct type *left;
    struct type *right;
    char spelling[8];
    const char *o = op_text(op, spelling);

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
        if (left != right) {
            error_at(c, e->pos, "the operands of `%s` have the types `%s` and "
                     "`%s`", o, tn(left), tn(right));
            return builtin(c, TYPE_ERROR);
        }
        if (op == TOKEN_EQ || op == TOKEN_NE) {
            if (left->kind == TYPE_STRUCT || left->kind == TYPE_ARRAY ||
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
        if (left != right) {
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

static struct type *check_cast(struct checker *c, struct expr *e)
{
    struct type *from = check_expr(c, e->as.cast.operand, NULL);
    struct type *to = resolve_type(c, e->as.cast.type);

    if (is_error(from) || is_error(to)) {
        return builtin(c, TYPE_ERROR);
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
    if (callee->kind == EXPR_NAME && callee->symbol != NULL &&
        (callee->symbol->kind == SYMBOL_FN ||
         callee->symbol->kind == SYMBOL_EXTERN_FN)) {
        return callee->symbol;
    }
    return NULL;
}

/* The struct behind a value of type T or *T, or NULL. */
static struct type *struct_of(struct type *t)
{
    if (t->kind == TYPE_POINTER) {
        t = t->element;
    }
    return t->kind == TYPE_STRUCT ? t : NULL;
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

/* A function named name in the module that declares struct s. Another
   module sees only its pub functions. */
static struct symbol *method_symbol(const struct checker *c,
                                    const struct type *s,
                                    const struct name *name)
{
    const struct interface *lib;

    if (same_name(&s->module, &c->module_name)) {
        return scope_find_local(&c->module_scope, name);
    }
    lib = find_library(c, &s->module);
    return lib != NULL ? library_item(lib, name) : NULL;
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
    struct symbol *item = library_item(module->home, &name);

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
    struct type *first;
    struct expr **args;
    struct expr *callee;

    if (f == NULL || (f->kind != SYMBOL_FN && f->kind != SYMBOL_EXTERN_FN) ||
        is_error(f->type) || f->type->param_count == 0 ||
        struct_of(f->type->params[0]) != s) {
        error_at(c, field->pos, "`%s` has no field or method `%.*s`", tn(s),
                 (int)field->as.field.name.length, field->as.field.name.text);
        return false;
    }
    first = f->type->params[0];
    if (first->kind == TYPE_POINTER && t->kind == TYPE_STRUCT) {
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
    } else if (first->kind == TYPE_STRUCT && t->kind == TYPE_POINTER) {
        struct expr *deref = new_node(c, EXPR_UNARY, receiver->pos);
        deref->as.unary.op = TOKEN_STAR;
        deref->as.unary.operand = receiver;
        deref->type = first;
        receiver = deref;
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

    if (callee->kind == EXPR_FIELD && (module = qualifier(c, callee)) != NULL) {
        fn = check_qualified(c, callee, module, true);
        callee->type = fn;
        if (!is_error(fn) && callee->symbol->kind == SYMBOL_EXTERN_FN) {
            variadic = callee->symbol->variadic;
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
    if (variadic ? e->as.call.arg_count < fn->param_count
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
    return ok ? fn->result : builtin(c, TYPE_ERROR);
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
    base = check_expr(c, e->as.field.base, NULL);
    if (is_error(base)) {
        return base;
    }
    if ((s = struct_of(base)) != NULL) {
        if ((f = find_field(s, name)) == NULL) {
            error_at(c, e->pos, "`%s` has no field `%.*s`", tn(s),
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
        ok = require(c, inits[i].value,
                     check_expr(c, inits[i].value, f->type), f->type) && ok;
    }
    if (!ok || skip_missing) {
        return ok;
    }
    for (j = 0; j < field_count; j++) {
        if (type_field_is_unit_break(&fields[j])) {
            continue;
        }
        for (i = 0; i < count; i++) {
            if (same_name(&fields[j].name, &inits[i].name)) {
                break;
            }
        }
        if (i == count) {
            error_at(c, e->pos, "the literal of `%s` misses the field `%.*s`",
                     type_name, (int)fields[j].name.length, fields[j].name.text);
            return false;
        }
    }
    return true;
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
        if (t->is_union && e->as.struct_lit.field_count != 1) {
            error_at(c, e->pos, "a literal of union `%s` names exactly one "
                     "field", tn(t));
            return builtin(c, TYPE_ERROR);
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
        t = resolve_type(c, e->as.alloc.type);
        if (!require(c, e->as.alloc.count,
                     check_expr(c, e->as.alloc.count, builtin(c, TYPE_I64)),
                     builtin(c, TYPE_I64)) || is_error(t)) {
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
        struct type *base = e->as.field.base->type;
        if (base->kind == TYPE_STRUCT) {
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
        return fail_const(c, e, "a call");
    case EXPR_ALLOC:
        return fail_const(c, e, "`alloc`");
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
        sym = declare(c, SYMBOL_LOCAL, &s->as.let.name, s->as.let.name_pos,
                      "`%.*s` is already declared in this block");
        if (sym != NULL) {
            sym->type = t;
            s->as.let.symbol = sym;
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
    for (i = 0; i < it->param_count; i++) {
        struct symbol *sym =
            declare(c, SYMBOL_PARAM, &it->params[i].name, it->params[i].pos,
                    "`%.*s` is already declared in this block");
        if (sym != NULL) {
            sym->type = it->symbol->type->params[i];
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
    case ITEM_UNION: return SYMBOL_STRUCT;
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
                const struct interface *const *libraries,
                size_t library_count, struct types *types,
                struct arena *arena, struct diagnostics *diags)
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
    c.libraries = libraries;
    c.library_count = library_count;
    c.scope = &c.module_scope;
    c.ok = true;

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
        it->symbol->exported = it->exported;
        it->symbol->doc = it->doc;
        if (it->kind == ITEM_STRUCT || it->kind == ITEM_UNION) {
            it->symbol->type = types_struct(types, c.module_name, it->name);
            it->symbol->type->is_union = it->kind == ITEM_UNION;
        }
    }

    for (i = 0; i < module->item_count; i++) {
        struct item *it = module->items[i];
        struct struct_field *fields;
        if (it->symbol == NULL ||
            (it->kind != ITEM_STRUCT && it->kind != ITEM_UNION)) {
            continue;
        }
        fields = arena_alloc(arena, it->param_count * sizeof *fields);
        for (j = 0; j < it->param_count; j++) {
            size_t k;
            fields[j].name = it->params[j].name;
            fields[j].pos = it->params[j].pos;
            fields[j].doc = it->params[j].doc;
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
        types_set_fields(types, it->symbol->type, fields, it->param_count);
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
    for (i = 0; i < module->item_count; i++) {
        struct item *it = module->items[i];
        if (it->symbol != NULL &&
            (it->kind == ITEM_STRUCT || it->kind == ITEM_UNION)) {
            it->symbol->type->item_exported = it->exported;
        }
    }
    for (i = 0; i < module->item_count; i++) {
        struct item *it = module->items[i];
        if (it->symbol != NULL && it->kind == ITEM_CONST) {
            const_symbol(&c, it->symbol, it->name_pos);
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
        if (t->item_exported) {
            return true;
        }
        *hidden = t;
        return false;
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
        if (!it->pub || it->symbol == NULL) {
            continue;
        }
        sym = arena_alloc(arena, sizeof *sym);
        *sym = *it->symbol;
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
