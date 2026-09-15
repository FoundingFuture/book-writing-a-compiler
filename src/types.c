#include "types.h"

#include <string.h>

void types_init(struct types *types, struct arena *arena)
{
    int k;

    memset(types, 0, sizeof *types);
    types->arena = arena;
    for (k = 0; k < TYPE_BUILTIN_COUNT; k++) {
        types->builtins[k].kind = (enum type_kind)k;
    }
}

struct type *types_builtin(struct types *types, enum type_kind kind)
{
    return &types->builtins[kind];
}

/* DESIGN: derived types sit in one linked list and every constructor
   searches it before it creates a type. A program has few distinct
   types, so a linear search is enough and keeps identity a pointer
   comparison. */
static struct type *find_or_add(struct types *types, const struct type *key)
{
    struct type *t;
    size_t i;

    for (t = types->derived; t != NULL; t = t->next) {
        if (t->kind != key->kind || t->element != key->element ||
            t->length != key->length || t->length_of != key->length_of ||
            t->result != key->result ||
            t->param_count != key->param_count) {
            continue;
        }
        for (i = 0; i < t->param_count; i++) {
            if (t->params[i] != key->params[i]) {
                break;
            }
        }
        if (i == t->param_count) {
            return t;
        }
    }
    t = arena_alloc(types->arena, sizeof *t);
    *t = *key;
    if (key->param_count > 0) {
        t->params = arena_alloc(types->arena,
                                key->param_count * sizeof *t->params);
        memcpy(t->params, key->params, key->param_count * sizeof *t->params);
    }
    t->next = types->derived;
    types->derived = t;
    return t;
}

struct type *types_pointer(struct types *types, struct type *element)
{
    struct type key = {0};

    key.kind = TYPE_POINTER;
    key.element = element;
    return find_or_add(types, &key);
}

struct type *types_array(struct types *types, struct type *element,
                         uint64_t length)
{
    struct type key = {0};

    key.kind = TYPE_ARRAY;
    key.element = element;
    key.length = length;
    return find_or_add(types, &key);
}

struct type *types_array_symbolic(struct types *types, struct type *element,
                                  const struct symbolic *length)
{
    struct type key = {0};

    key.kind = TYPE_ARRAY;
    key.element = element;
    key.length_of = length;
    return find_or_add(types, &key);
}

const struct symbolic *types_symbolic(struct types *types,
                                      const struct symbolic *key)
{
    struct symbolic *s;

    for (s = types->symbolics; s != NULL; s = s->next) {
        if (s->kind == key->kind && s->type == key->type &&
            s->value == key->value && s->of == key->of && s->op == key->op &&
            s->a == key->a && s->b == key->b) {
            return s;
        }
    }
    s = arena_alloc(types->arena, sizeof *s);
    *s = *key;
    s->next = types->symbolics;
    types->symbolics = s;
    return s;
}

static const char *operator_spelling(enum token_kind op)
{
    switch (op) {
    case TOKEN_PLUS: return "+";
    case TOKEN_MINUS: return "-";
    case TOKEN_STAR: return "*";
    case TOKEN_SLASH: return "/";
    case TOKEN_PERCENT: return "%";
    case TOKEN_AMP: return "&";
    case TOKEN_PIPE: return "|";
    case TOKEN_CARET: return "^";
    case TOKEN_TILDE: return "~";
    case TOKEN_BANG: return "!";
    case TOKEN_SHL: return "<<";
    case TOKEN_SHR: return ">>";
    case TOKEN_AND_AND: return "&&";
    case TOKEN_OR_OR: return "||";
    case TOKEN_EQ: return "==";
    case TOKEN_NE: return "!=";
    case TOKEN_LT: return "<";
    case TOKEN_LE: return "<=";
    case TOKEN_GT: return ">";
    default: return ">=";
    }
}

static void print_type(struct text *out, const struct type *t, bool qualified);

/* An operand in parentheses when it is itself an operation, so the text
   keeps the grouping of the value. */
static void print_operand(struct text *out, const struct symbolic *s,
                          bool qualified)
{
    bool group = s->kind == SYMBOLIC_BINARY || s->kind == SYMBOLIC_CAST;

    text_append(out, group ? "(" : "");
    symbolic_print(out, s, qualified);
    text_append(out, group ? ")" : "");
}

void symbolic_print(struct text *out, const struct symbolic *s,
                    bool qualified)
{
    switch (s->kind) {
    case SYMBOLIC_INT:
        if (s->type->kind == TYPE_BOOL) {
            text_append(out, s->value != 0 ? "true" : "false");
        } else if (type_is_signed(s->type)) {
            text_appendf(out, "%lld", (long long)s->value);
        } else {
            text_appendf(out, "%llu", (unsigned long long)s->value);
        }
        break;
    case SYMBOLIC_SIZE_OF:
        text_append(out, "size_of(");
        print_type(out, s->of, qualified);
        text_append(out, ")");
        break;
    case SYMBOLIC_UNARY:
        text_append(out, operator_spelling(s->op));
        print_operand(out, s->a, qualified);
        break;
    case SYMBOLIC_BINARY:
        print_operand(out, s->a, qualified);
        text_appendf(out, " %s ", operator_spelling(s->op));
        print_operand(out, s->b, qualified);
        break;
    case SYMBOLIC_CAST:
        print_operand(out, s->a, qualified);
        text_append(out, " as ");
        print_type(out, s->type, qualified);
        break;
    }
}

struct type *types_slice(struct types *types, struct type *element)
{
    struct type key = {0};

    key.kind = TYPE_SLICE;
    key.element = element;
    return find_or_add(types, &key);
}

struct type *types_fn(struct types *types, struct type **params,
                      size_t param_count, struct type *result)
{
    struct type key = {0};

    key.kind = TYPE_FN;
    key.params = params;
    key.param_count = param_count;
    key.result = result;
    return find_or_add(types, &key);
}

struct type *types_struct(struct types *types, struct name module,
                          struct name name)
{
    struct type *t = arena_alloc(types->arena, sizeof *t);

    t->kind = TYPE_STRUCT;
    t->module = module;
    t->name = name;
    return t;
}

void types_set_fields(struct types *types, struct type *s,
                      const struct struct_field *fields, size_t count)
{
    s->fields = arena_alloc(types->arena, count * sizeof *s->fields);
    memcpy(s->fields, fields, count * sizeof *s->fields);
    s->field_count = count;
}

static struct type *cycle_in(struct type *t);

/* The struct that a symbolic value measures with size_of and that holds
   the struct being checked. */
static struct type *cycle_in_symbolic(const struct symbolic *s)
{
    struct type *cycle = NULL;

    if (s == NULL) {
        return NULL;
    }
    if (s->kind == SYMBOLIC_SIZE_OF) {
        return cycle_in(s->of);
    }
    cycle = cycle_in_symbolic(s->a);
    return cycle != NULL ? cycle : cycle_in_symbolic(s->b);
}

/* A struct inside a value of type t that contains itself. Pointers,
   slices and function pointers hold no value of their element. */
static struct type *cycle_in(struct type *t)
{
    struct type *cycle;

    while (t->kind == TYPE_ARRAY) {
        if ((cycle = cycle_in_symbolic(t->length_of)) != NULL) {
            return cycle;
        }
        t = t->element;
    }
    return t->kind == TYPE_STRUCT ? types_find_cycle(t) : NULL;
}

struct type *types_find_cycle(struct type *s)
{
    struct type *cycle;
    size_t i;

    if (s->layout == LAYOUT_DONE) {
        return NULL;
    }
    if (s->layout == LAYOUT_BUSY) {
        return s;
    }
    s->layout = LAYOUT_BUSY;
    for (i = 0; i < s->field_count; i++) {
        if ((cycle = cycle_in(s->fields[i].type)) != NULL) {
            s->layout = LAYOUT_NONE;
            return cycle;
        }
    }
    s->layout = LAYOUT_DONE;
    return NULL;
}

void type_name(struct text *out, const struct type *t)
{
    print_type(out, t, false);
}

void type_name_qualified(struct text *out, const struct type *t)
{
    print_type(out, t, true);
}

static void print_type(struct text *out, const struct type *t, bool qualified)
{
    static const char *const builtin_names[TYPE_BUILTIN_COUNT] = {
        [TYPE_VOID] = "void", [TYPE_BOOL] = "bool", [TYPE_CHAR] = "char",
        [TYPE_I8] = "i8", [TYPE_I16] = "i16", [TYPE_I32] = "i32",
        [TYPE_I64] = "int", [TYPE_U8] = "byte", [TYPE_U16] = "u16",
        [TYPE_U32] = "u32", [TYPE_U64] = "u64", [TYPE_F32] = "f32",
        [TYPE_CLONG] = "c_long", [TYPE_CULONG] = "c_ulong",
        [TYPE_CWCHAR] = "c_wchar",
        [TYPE_F64] = "float", [TYPE_STR] = "str", [TYPE_NULL] = "null",
        [TYPE_ERROR] = "<error>",
    };
    size_t i;

    switch (t->kind) {
    case TYPE_POINTER:
        text_append(out, "*");
        print_type(out, t->element, qualified);
        return;
    case TYPE_SLICE:
        text_append(out, "[]");
        print_type(out, t->element, qualified);
        return;
    case TYPE_ARRAY:
        if (t->length_of != NULL) {
            text_append(out, "[");
            symbolic_print(out, t->length_of, qualified);
            text_append(out, "]");
        } else {
            text_appendf(out, "[%llu]", (unsigned long long)t->length);
        }
        print_type(out, t->element, qualified);
        return;
    case TYPE_FN:
        text_append(out, "fn(");
        for (i = 0; i < t->param_count; i++) {
            if (i > 0) {
                text_append(out, ", ");
            }
            print_type(out, t->params[i], qualified);
        }
        text_append(out, ")");
        if (t->result->kind != TYPE_VOID) {
            text_append(out, " -> ");
            print_type(out, t->result, qualified);
        }
        return;
    case TYPE_STRUCT:
        if (qualified) {
            text_appendf(out, "%.*s.", (int)t->module.length, t->module.text);
        }
        text_appendf(out, "%.*s", (int)t->name.length, t->name.text);
        return;
    default:
        text_append(out, builtin_names[t->kind]);
        return;
    }
}

bool type_is_integer(const struct type *t)
{
    return t->kind >= TYPE_I8 && t->kind <= TYPE_CWCHAR;
}

bool type_field_is_unit_break(const struct struct_field *f)
{
    return f->name.length == 1 && f->name.text[0] == '_';
}

bool type_is_target_sized(const struct type *t)
{
    return t->kind == TYPE_CLONG || t->kind == TYPE_CULONG ||
           t->kind == TYPE_CWCHAR;
}

bool type_is_signed(const struct type *t)
{
    return t->kind >= TYPE_I8 && t->kind <= TYPE_CLONG;
}

bool type_is_float(const struct type *t)
{
    return t->kind == TYPE_F32 || t->kind == TYPE_F64;
}

bool type_is_numeric(const struct type *t)
{
    return type_is_integer(t) || type_is_float(t);
}

int type_bits(const struct type *t)
{
    switch (t->kind) {
    case TYPE_I8:
    case TYPE_U8: return 8;
    case TYPE_I16:
    case TYPE_U16:
    case TYPE_CWCHAR: return 16;
    case TYPE_I32:
    case TYPE_U32:
    case TYPE_CLONG:
    case TYPE_CULONG:
    case TYPE_F32: return 32;
    case TYPE_I64:
    case TYPE_U64:
    case TYPE_F64: return 64;
    default: return 0;
    }
}

bool type_pointer_free(const struct type *t)
{
    size_t i;

    switch (t->kind) {
    case TYPE_POINTER:
    case TYPE_SLICE:
    case TYPE_FN:
        return false;
    case TYPE_ARRAY:
        return type_pointer_free(t->element);
    case TYPE_STRUCT:
        for (i = 0; i < t->field_count; i++) {
            if (!type_pointer_free(t->fields[i].type)) {
                return false;
            }
        }
        return true;
    default:
        return true;
    }
}
