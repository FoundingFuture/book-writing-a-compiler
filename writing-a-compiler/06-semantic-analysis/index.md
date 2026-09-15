---
title: "Semantic analysis"
description: "Scopes, symbol tables and a type checker in C for Anti, with literal types, symbolic size_of constants and checks of unions, bitfields and exports."
summary: "Scopes and symbol tables, name resolution and the type checker with no implicit conversions. Mandatory initialisation and the method-call rewrite. Constants computed from `size_of` that stay symbolic, the checks of unions, bitfields and export signatures, and the pointer-free type property used by the threading chapter."
date: 2026-09-13T22:32:00+02:00
lastmod: 2026-09-15T03:49:22+02:00
draft: false
weight: 60
tags: [compilers, programming-languages]
keywords: [semantic analysis, symbol table, scopes and shadowing, type checker, type interning, symbolic size_of, method call rewrite, export signature check]
---

## Previously

[Chapter 5, The parser and the syntax tree]({{% relref "/programming/writing-a-compiler/05-parser-and-syntax-tree" %}}), parses the Anti grammar by recursive descent with precedence climbing. It defines the C structures of the syntax tree and the parsing of each declaration and statement. The declarations include dotted import paths, unions, bitfields, `export` and the contextual words `packed` and `align`. The parser attaches doc comments to items, and its error recovery produces one message for one mistake.

## Checker output

The parser accepts every program that follows the grammar, including `let n: u8 = 300;` and `return x + true;`. Semantic analysis applies the remaining rules of [chapter 2]({{% relref "/programming/writing-a-compiler/02-the-anti-language" %}}). It connects each name to its declaration, gives each expression a type and computes each constant. The checker of antic writes its results into the syntax tree. An expression gets its type, a name gets its symbol, and a call in method syntax becomes a plain call.

```c
/* Check one module against the rules of chapter 2: resolve every name,
   give every expression its type and compute every constant. The method
   syntax v.f(args) and qualified names are rewritten into plain names.
   libraries holds the interface of every loaded library. Returns true
   when no error occurred. */
bool sema_check(struct module *module, const char *module_name,
                const struct interface *const *libraries,
                size_t library_count, struct types *types,
                struct arena *arena, struct diagnostics *diags);
```

The function checks one module. The interfaces of the library files that the module imports arrive in `libraries`, a structure that chapter 9 describes. A name qualified by an imported module also becomes a plain name of the imported item.

## Type table

A checked type is a `struct type`. The built-in types exist once in an array, one for each kind from `TYPE_VOID` to `TYPE_ERROR`. A derived type, such as a pointer or an array, is created on first use and kept in a list. Every constructor searches that list first, so a spelling such as `*[]byte` produces one `struct type` for the whole compilation. Two types are then the same type exactly when their pointers are equal, which is the identity rule of chapter 2.

```c
struct type {
    enum type_kind kind;
    struct type *element;           /* TYPE_POINTER, TYPE_ARRAY, TYPE_SLICE */
    uint64_t length;                /* TYPE_ARRAY, 0 when symbolic */
    const struct symbolic *length_of; /* TYPE_ARRAY, a symbolic length */
    struct type **params;           /* TYPE_FN */
    size_t param_count;
    struct type *result;            /* TYPE_FN, TYPE_VOID without a result */
    struct name module;             /* TYPE_STRUCT */
    struct name name;               /* TYPE_STRUCT */
    struct struct_field *fields;    /* TYPE_STRUCT */
    size_t field_count;

    /* The modifiers of a struct or union: union, packed, the N of
       align(N) or 0, and export. */
    bool is_union;
    bool packed;
    uint64_t align;
    bool item_exported;

    enum layout_state layout;       /* TYPE_STRUCT, for the cycle check */
    struct type *next;              /* the list of derived types */
};
```

```c
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
```

An array type holds its length in `length` when the length is a number, and in `length_of` when it is computed from `size_of`. The section on symbolic sizes explains the second form.

Structs are the exception to interning. Each struct or union declaration creates a new type, so two declarations with identical fields remain two types. A message names a type as a program writes it. The aliased types appear under the names `int`, `float` and `byte`, so `u8` shows as `byte`.

### Numeric and C types

The function `builtin_of_token` maps each type keyword to its built-in type. The keyword `uint` is `u64`. The C types with a fixed size are other spellings of sized types, such as `c_int` for `i32` and `c_size_t` for `u64`. Two spellings of one type give one `struct type`.

```c
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
```

The typed dump of an external function shows both names. The column on the right prints the type, and the `type` lines below each parameter keep the spelling of the source.

```anti
extern fn f(a: c_int, b: c_uchar, c: c_size_t, d: c_double) -> uint;
```

```text
extern_fn f                fn(i32, byte, u64, float) -> u64
  param a
    type c_int
  param b
    type c_uchar
  param c
    type c_size_t
  param d
    type c_double
  result
    type uint
```

The types `c_long`, `c_ulong` and `c_wchar` are kinds of their own, because their width depends on the target. The types `c_long` and `c_ulong` have 32 bits on Windows and 64 bits elsewhere. The type `c_wchar` has 16 bits on Windows and 32 bits elsewhere, with the signedness of C `wchar_t` on the target. The front end compiles for no particular target, so `type_bits` gives each of the three its narrower width. A literal or a constant of such a type must fit that width, so it has one value on every target. Chapter 11 lists the widths per target.

```c
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
```

The function `type_is_target_sized` answers true for the three types. No conversion to or from them is implicit. The function `fn f(a: c_long) -> i64 { return a; }` reports that it expected `int` and found `c_long`. The conversion `a as int` makes the value an `int`.

## Scopes and symbols

A symbol records what a name refers to. Its kind is one of the seven in `enum symbol_kind`, and a union has the kind of a struct. A symbol holds the declaration's position and type, and for a constant its computed value.

```c
struct symbol {
    enum symbol_kind kind;
    struct name name;
    struct pos pos;
    struct type *type;
    struct item *item;              /* a module-level item */
    struct stmt *stmt;              /* a const in a block */
    struct const_value *value;      /* SYMBOL_CONST */
    enum eval_state state;          /* SYMBOL_CONST */
    bool address_taken;             /* SYMBOL_LOCAL, SYMBOL_PARAM */
    bool variadic;                  /* SYMBOL_EXTERN_FN */
    const struct name *params;      /* a function of an interface */

    /* An export item, whose function has the C symbol of its name. */
    bool exported;
    struct doc_text doc;            /* the /// text of a pub item */
    const struct interface *home;   /* the library of an imported item */
    uint32_t ir;                    /* set by lowering, see lower.h */
};
```

The fields from `exported` onwards serve later stages. The flag `exported` marks an `export` item, and `doc` holds the `///` text of the item for its library file. The field `home` names the library of an imported item, and lowering in chapter 8 sets `ir`.

A scope is a list of symbols with a pointer to the enclosing scope. The module scope holds the imports and the items. A function adds a scope for its parameters, and every block adds one more. Lookup walks from the innermost scope outwards and returns the first match. A second declaration of a name in the same scope is an error, and a declaration in an inner scope hides the outer one.

```c
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
```

These scopes implement the shadowing rules of chapter 2. The function body is a block inside the parameter scope, so `let x = x + 1;` at the top of a body declares a new `x`. The checker adds the new symbol after it checks the initialiser, so the initialiser still sees the parameter.

The checker declares every import and every item of the module before it checks any function body. A function may therefore call a function that the file declares further down.

## Types of expressions

### Expected types and literals

The central function is `check_expr`. It takes an expression and the type its context expects, which may be `NULL`, and returns the expression's type. The expected type decides the type of a literal, as chapter 2 specifies. An integer literal takes the expected integer type, or `int` without one. The function then checks that the value fits.

```c
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
```

The function `max_of` derives the largest value from `type_bits`, so a literal of `c_long` fits 32 bits. The message then adds the words `on every target`. The declaration `let b: c_long = 2147483648;` reports that `2147483648` does not fit `c_long` on every target.

A binary operator passes the type of one operand to the other. A literal on the right makes the checker check the left operand first. A literal on the left reverses that order. Two literals both take the type that the context expects of the result.

```c
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
```

For the function `f` below, the option `--dump-types` shows where each literal got its type. The `1` in `a + 1` is a `u32`, and `-128` forms one `i8` constant.

```anti
fn f(a: u32) -> u32
{
    let x: i8 = -128;
    return a + 1;
}
```

```text
function f                 fn(u32) -> u32
  param a                  u32
    type u32
  result
    type u32
  block
    let_stmt x             i8
      type i8
      unary -              i8
        int_lit 128        i8
    return_stmt
      additive +           u32
        ident a            u32
        int_lit 1          u32
```

### Operators and conversions

Both operands of a binary operator have the same type. The operators `+`, `-`, `*` and `/` need numeric operands. The operator `%` and the bit operators need integers, and `&&` and `||` need `bool`. The operators `==` and `!=` accept every type except structs, unions, arrays, slices and `str`. The ordering operators accept numeric types and `char`. A mismatch reports both types, as in the operands of `+` having the types `int` and `byte`.

An explicit conversion with `as` follows the table of chapter 2. The function `can_convert` encodes it, and any other pair of types is a compile error.

```c
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
```

The target-sized types are numeric, so `as` converts between them and every other numeric type. The back end turns a conversion between two types of equal width on its target into a copy.

### Places

Assignment and `&` need a place, an expression that denotes a location in memory. The function `is_place` follows the list in chapter 2. A constant is no place, so `&X` on a constant reports that a constant has no address. An element of a `str` is no place either, because a `str` never changes. The checker also marks each local variable whose address the program takes. Chapter 8 keeps such variables in the stack frame.

```c
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
```

A bitfield is a struct field with a width in bits, such as `layer: u32 : 4`. It is a place for assignment and has no address. The function `is_bitfield` recognises a field access whose field has a width, and `&` on such a field reports that a bitfield has no address.

```c
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
```

## Method-call rewrite

A call `v.f(args)` has two readings. When the struct of `v` has a field `f`, the call calls the function pointer in that field. Otherwise it is method syntax, and the checker rewrites it into a plain call. The function `method_symbol` looks for a function `f` in the module that declares the struct, and another module offers only its `pub` functions. The first parameter of `f` must be the struct type or a pointer to it. The checker then builds the first argument: `&v`, `v`, `p` or `*p`, following the table of chapter 2.

```c
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
```

A union has no methods. The function `check_call` rewrites `v.f(args)` only for a struct, so on a union the call always reads the field `f`. A call `v.get()` on the union `Value` without a field `get` reports that `Value` has no field `get`.

```c
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
```

After the rewrite, every later stage sees an ordinary call. The struct `Shape` of chapter 2 shows both readings in one function. The call `sq.scale(1.5)` becomes `scale(&sq, 1.5)`, and `sq.area(&sq)` calls the function pointer in the field `area`.

## Constants

A constant's value is computed at compile time. The function `eval_const` evaluates a checked expression and accepts only what chapter 2 allows in a constant. That covers literals, other constants, operators, conversions, struct and array literals, `size_of`, field access and constant indexing. The evaluator refuses a union literal, because a constant struct holds a value for every field and a union literal names one. A call reports that a call is not a constant expression.

The result is a `struct const_value`. Its kind says which member of `as` holds the value.

```c
enum const_kind {
    CONST_INT,
    CONST_FLOAT,
    CONST_BOOL,
    CONST_CHAR,
    CONST_NULL,
    CONST_TEXT,     /* a string or byte string literal */
    CONST_ARRAY,
    CONST_STRUCT,
    CONST_SYMBOLIC  /* an integer or bool computed from size_of */
};

/* The value of a constant, computed at compile time. */
struct const_value {
    enum const_kind kind;
    struct type *type;
    union {
        uint64_t integer;           /* the bits of the value in its type */
        double floating;
        bool boolean;
        uint32_t character;
        struct token_text text;
        struct {
            struct const_value *items;  /* elements, or fields in order */
            size_t count;
        } aggregate;
        const struct symbolic *symbolic;
    } as;
};
```

Integer results wrap to the bit width of their type, which `wrap` does. For a target-sized type the function keeps all 64 bits.

```c
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
```

The function `fits_every_target` then checks that the result fits the narrower width. The constant `const BIG: c_ulong = 65536 as c_ulong * 65536 as c_ulong;` reports that the value does not fit `c_ulong` on every target.

```c
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
```

Constants may refer to each other in any order. A constant gets its value on first use. It is marked busy while its initialiser is evaluated, so a cycle such as `const A: int = B;` and `const B: int = A;` reports that `A` depends on itself.

```c
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
```

The same evaluator computes array lengths. `[N]T` and `[e; N]` need a constant `int` of at least 1, so `[0; 0]` reports that an array length is at least 1. A length computed from `size_of` is the exception that the next section describes.

### Undefined operations

Chapter 2 lists four operations without a value. They are an integer division or remainder by zero and the minimum value divided by `-1`. The other two are a shift count outside the bits of the type and a float conversion outside the integer range. With constant operands each is a compile error, in a constant and in a function body. The checks `check_binary` and `check_cast` call `undefined_on_constants` after the operand types are known.

```c
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
```

The function `reports_undefined` tests the four cases and writes the message by one rule. It names the operation and the operand values in the form the reader wrote them, and says what is wrong in three or four words. A literal operand keeps its spelling, as `0x10` in `` `0x10 / 0` divides by zero ``. Any other operand prints as the literal of its value, with the fewest digits that read back, and NaN prints as `nan`. The program `fn f() -> i32 { return 30000000000.0 as i32; }` reports `` `30000000000.0 as i32` does not fit `i32` ``. The evaluator calls `reports_undefined` too, so antic itself never divides by zero while it computes a constant. A variable operand leaves the case to run time, so `x / 0` compiles.

## Symbolic sizes

The value of `size_of(T)` depends on the target, and the front end compiles for no particular target. The evaluator therefore gives `size_of` a symbolic value, an expression tree that the back end folds for its target. A constant computed from `size_of` stays symbolic too. For a struct `H`, the constant `const N: int = size_of(H) * 2;` holds the tree of `size_of(H) * 2`, and chapter 7 takes such values into the IR.

```c
enum symbolic_kind {
    SYMBOLIC_INT,
    SYMBOLIC_SIZE_OF,
    SYMBOLIC_UNARY,
    SYMBOLIC_BINARY,
    SYMBOLIC_CAST
};

/* A constant integer or bool whose value depends on the target, because
   it is computed from size_of. Nodes are interned, so two equal values
   are one pointer. */
struct symbolic {
    enum symbolic_kind kind;
    struct type *type;
    uint64_t value;                 /* SYMBOLIC_INT */
    struct type *of;                /* SYMBOLIC_SIZE_OF */
    enum token_kind op;             /* SYMBOLIC_UNARY, SYMBOLIC_BINARY */
    const struct symbolic *a;       /* the operand of an operation or cast */
    const struct symbolic *b;       /* SYMBOLIC_BINARY */
    struct symbolic *next;          /* the list of interned nodes */
};
```

Symbolic nodes are interned like derived types. The function `types_symbolic` returns the existing node equal to a key, so two equal values are one pointer.

```c
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
```

A constant value of the kind `CONST_SYMBOLIC` holds such a node. The case `EXPR_SIZE_OF` of `eval_const` creates the leaf. The case `EXPR_CAST` follows it and shows the one rule for conversions. The back end folds symbolic values as integers, so a value computed from `size_of` converts only to an integer type.

```c
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
```

An operator or a conversion with a symbolic operand builds a new node. The function `as_symbolic` turns a number, a bool or a character operand into a node of the kind `SYMBOLIC_INT`. The function `symbolic_value` interns the operation.

```c
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
```

The checks that need no target stay in the front end. A division by a constant zero and a shift count outside the bits of the type are errors, even with a symbolic left operand.

### Array lengths

An array length may be symbolic. The function `array_of` checks a length expression and returns an array type with a number or with a symbolic length. The flag `target_sized` of the checker allows a symbolic length where chapter 2 allows it. The checker sets the flag while it resolves the type of a struct field and while it checks a `let` statement.

```c
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
```

Two array types with the same interned length are one type, so `[N]byte` and `[size_of(H) * 2]byte` name the same type. Such a length has no value in the front end, so a constant expression that reads `.len` of such an array reports an error. The back end checks that each folded length is at least 1 on its target.

The typed dump prints a symbolic length as a program writes it. An operand that is itself an operation gets parentheses.

```anti
struct H
{
    tag: u8,
    n: i32
}

const N: int = size_of(H) * 2;

struct B
{
    bytes: [N]byte,
    more: [size_of(H) * 2]byte
}

fn f(b: *B) -> int
{
    let copy: [N]byte = b.more;
    let fill = [0; N - 1];
    return copy.len + fill.len;
}
```

```text
struct_decl H
  field tag                byte
    type u8
  field n                  i32
    type i32
const_decl N               int
  type int
  multiplicative *         int
    size_of                int
      type H
    int_lit 2              int
struct_decl B
  field bytes              [size_of(H) * 2]byte
    type [N]
      ident N              int
      type byte
  field more               [size_of(H) * 2]byte
    type [N]
      multiplicative *     int
        size_of            int
          type H
        int_lit 2          int
      type byte
function f                 fn(*B) -> int
  param b                  *B
    type *
      type B
  result
    type int
  block
    let_stmt copy          [size_of(H) * 2]byte
      type [N]
        ident N            int
        type byte
      field more           [size_of(H) * 2]byte
        ident b            *B
    let_stmt fill          [(size_of(H) * 2) - 1]int
      array_lit ;          [(size_of(H) * 2) - 1]int
        int_lit 0          int
        additive -         int
          ident N          int
          int_lit 1        int
    return_stmt
      additive +           int
        field len          int
          ident copy       [size_of(H) * 2]byte
        field len          int
          ident fill       [(size_of(H) * 2) - 1]int
```

A length from `size_of` in a parameter, a cast of such a value to `f64` and a struct that measures itself are all errors.

```anti
struct H
{
    n: i32
}

const F: f64 = size_of(H) as f64;

fn f(a: [size_of(H)]byte)
{
}

struct S
{
    a: [size_of(S)]byte
}
```

```text
sizeof.anti:12:8: error: struct `S` contains itself
sizeof.anti:8:10: error: a length computed from `size_of` is allowed only in a struct field or a local variable
sizeof.anti:6:16: error: a value computed from `size_of` converts only to an integer type in a constant expression
```

## Struct and union declarations

The fields get their types in a second pass over the items, after every item name is declared. A field records its name, position, type and bitfield width, and the `///` text for its library file.

```c
struct struct_field {
    struct name name;
    struct pos pos;
    struct type *type;
    uint8_t bits;                   /* the width of a bitfield, or 0 */
    struct doc_text doc;            /* the /// text */
};
```

```c
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
```

The loop after the fields runs the cycle check of `types_find_cycle`, which the last subsection of this section describes.

A union is a `TYPE_STRUCT` with `is_union` set, and every rule of structs applies to it unless the checker tests the flag. The flag changes three rules. A union literal names exactly one field, `v.f(args)` on a union never becomes a method call, and a union literal is not a constant.

A field with a width is a bitfield. The function `bitfield_width` checks that the type is a sized integer and that the width is a constant from 1 to the bits of the type. The types `c_long`, `c_ulong` and `c_wchar` are refused as bitfield types. The field named `_` is the zero-width bitfield of C and must have the width 0. It may appear more than once, a union refuses it, and `find_field` and the check of struct literals skip it, so no access or literal names it.

```c
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
```

The modifier `packed` passes to the type unchanged. The N of `align(N)` must be a constant power of two, which `alignment` checks. The back end reports an alignment below the one that the fields give, because only the back end knows that alignment.

```c
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
```

The files that antic compiled for these listings indent with one tab per level, the formatter style. The page shows each tab as four spaces. A column counts bytes, so the column after one tab is 2.

```anti
union Value
{
    i: int,
    f: f64
}

struct Flags
{
    visible: u32 : 1,
    layer: u32 : 4,
    level: u8 : 9
}

struct A align(3)
{
    a: u8
}

fn f(flags: Flags) -> int
{
    let v = Value { i: 1, f: 2.0 };
    let p = &flags.layer;
    return 0;
}
```

```text
modifiers.anti:11:14: error: a bitfield of `byte` has 1 to 8 bits
modifiers.anti:14:16: error: an alignment is a power of two
modifiers.anti:21:10: error: a literal of union `Value` names exactly one field
modifiers.anti:22:11: error: a bitfield has no address
```

### Structs that contain themselves

The front end computes no layout. Chapter 18 lays out every struct and union in the back end, for the target that it compiles for. Semantic analysis checks only that no struct contains itself by value, a rule that needs no target. The size of such a struct depends on itself, whether it holds itself directly, in an array or through `size_of` in an array length.

```c
/* DESIGN: the front end computes no layout, because the back end lays out
   types for its target. The front end checks only that no struct contains
   itself by value, directly or through size_of in an array length.
   Returns NULL, or the struct that contains itself. */
struct type *types_find_cycle(struct type *s);
```

```c
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
```

The function `types_find_cycle` marks a struct as busy while it visits the fields. Pointers, slices and function pointers hold no value of their element, so `cycle_in` stops at them. An array passes on to its element and to every `size_of` operand in its length. A struct that is still busy when a field reaches it contains itself. For `struct Node { next: *Node, value: Node }` the checker reports that struct `Node` contains itself. The struct `S` in the section on symbolic sizes contains itself through `size_of(S)`.

## Statements

A condition has type `bool`. `break` and `continue` need an enclosing loop. A `return` with a value needs a function with a result of that type, and a `return` without a value needs a function without a result.

A function with a result must not reach the end of its body. The rule of chapter 2 decides from the form of the body alone. The last statement is a `return`, or an `if` chain with a final `else` whose branches all end that way.

```c
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
```

A function that ends with `while true do { return 1; }` therefore reports that it can reach its end without `return`, because a loop never counts.

Chapter 2 also requires every variable to have an initial value. The grammar enforces that already, because `let_stmt` has no form without `=`. The checker gives the variable the declared type, or the type of the initialiser when the declaration names none.

## Export checks

An `export` item has a C symbol and appears in the C header of a library for C, which chapter 25 builds. Its types must have a C spelling. The function `c_representable` decides that for one type, with `field` set for the type of a struct field.

```c
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
```

A pointer passes its element on with `field` cleared, so `*[4]int` has no C representation in a signature. A fixed-size array with a symbolic length has none either. The function `check_c_type` turns a refusal into a message that names the place. A struct without `export` gets its own message.

```c
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
```

The function `check_export` runs after every function body is checked. An export fn checks each parameter and its result, and it may not be `main`. No library that the module loads may export a function of the same name, because both would have one C symbol. An exported struct or union checks every field. With `align(N)` its first field is no bitfield, because the header of chapter 25 writes the alignment on the first field and C allows none on a bitfield. An export const has a numeric type, `bool` or `str`.

```c
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
```

The unit test `unique_exports` in `tests/unit/test_modules.c` builds a library `geo` with an export fn `dot` and checks a module that exports `dot` as well. It expects the message that export fn `dot` has the symbol of export fn `dot` in module `geo`. The other messages come from one file.

```anti
struct V
{
    x: int
}

export struct S
{
    name: str
}

export fn f(v: *V, s: str) -> c_int
{
    return 0;
}

export fn main() -> int
{
    return 0;
}
```

```text
geo.anti:8:2: error: the field `name` of export struct `S` has type `str`, which C cannot represent
geo.anti:11:13: error: the parameter `v` of export fn `f` has type `*V`, and `V` is not exported
geo.anti:11:20: error: the parameter `s` of export fn `f` has type `str`, which C cannot represent
geo.anti:16:11: error: `main` is the entry of the program and cannot be exported
```

## Doc comment warning

A `//#` comment holds a note for the developers of a library, and a `///` comment documents the item for its users. A `pub` item with a note and no `///` comment gets a warning on request. A warning does not stop the compilation. A `struct diagnostic` has a `warning` flag, which `diagnostics_warn` sets.

```c
/* A compile error or warning at a position in the source. Lines and
   columns count from 1, and a column counts bytes. A warning does not
   stop the compilation. */
struct diagnostic {
    int line;
    int column;
    bool warning;
    char message[160];
};
```

Doc warnings belong to `anti check`, the check command of the build tool in chapter 24, The build tool. It passes the option `--doc-warnings`, and without that option antic says nothing about documentation. The function `sema_doc_warnings` walks the items of a checked module and leaves the result of the check unchanged.

```c
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
```

The driver calls it after `sema_check` succeeds, and only with the option.

```c
    if (!sema_check(tree, text_cstr(module), libraries, paths.count,
                    &types, &arena, &diags)) {
        print_diagnostics(o->input, &diags);
```

The driver prints the diagnostics after the check, whether it failed or not. Each line names its kind, `error` or `warning`.

```c
static void print_diagnostics(const char *input,
                              const struct diagnostics *diags)
{
    size_t i;

    for (i = 0; i < diags->count; i++) {
        fprintf(stderr, "%s:%d:%d: %s: %s\n", input, diags->items[i].line,
                diags->items[i].column,
                diags->items[i].warning ? "warning" : "error",
                diags->items[i].message);
    }
}
```

The file `tests/errors/note.anti` compiles with one warning under `--doc-warnings`, which the test `warning_note` expects. Without the option the test `silent_note` expects no warning.

```anti
//# Keeps the cache warm.
pub fn warm() -> int
{
    return 1;
}

fn main() -> int
{
    return warm() - 1;
}
```

```text
tests/errors/note.anti:2:8: warning: the pub item `warm` has a `//#` note and no `///` comment
```

## Pointer-free property

Chapter 22 lets a worker function run on another thread and receive only data without pointers. The function `type_pointer_free` computes that property in the type checker. Pointers, slices and function pointers contain an address. A struct, a union or an array is pointer-free when its fields or its elements are. A `str` counts as pointer-free, because its bytes never change.

```c
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
```

## Error output

The checker reports every error it finds and does not stop at the first. An expression that already produced an error has the type `TYPE_ERROR`, and every check that sees this type stays silent. One mistake therefore produces one message. The file `tests/errors/types.anti` has two mistakes.

```anti
fn main() -> int
{
    let flag: bool = 1;
    return flag;
}
```

```text
tests/errors/types.anti:3:19: error: expected `bool`, found an integer literal
tests/errors/types.anti:4:9: error: expected `int`, found `bool`
```

## Typed tree dump

The option `--dump-types` prints the syntax tree with the type of each expression, parameter, variable and function at column 28. For the function `scale` in `tests/dump/scale.anti` it prints the listing that chapter 1 shows. The test `dump_types_scale` compares the two byte for byte.

```anti
fn scale(x: int) -> int
{
    let k = 2 + 4;
    return x * k;
}
```

```text
function scale             fn(int) -> int
  param x                  int
    type int
  result
    type int
  block
    let_stmt k             int
      additive +           int
        int_lit 2          int
        int_lit 4          int
    return_stmt
      multiplicative *     int
        ident x            int
        ident k            int
```

## Tests

The unit tests for the checker run 33 programs that must pass and 100 that must fail with an exact position and message. The groups cover the rules of this chapter, from literal types and the C types to constants and `main`. Four more tests compare typed tree dumps, and one expects the note warning. The types layer has its own tests in `tests/unit/test_types.c`, which include the cycle check of `Node` and the symbolic length `[size_of(Pixel) - 4]byte`.

```c
    /* A length computed from size_of stays symbolic until the back end
       folds it. Equal lengths make one array type. */
    accepts("struct H { tag: u8, n: i32 }\n"
            "const N: int = size_of(H) * 2;\n"
            "struct B { bytes: [N]byte, more: [size_of(H) * 2]byte }\n"
            "fn f(b: *B) -> int {\n"
            "    let copy: [N]byte = b.more;\n"
            "    let fill = [0; N - 1];\n"
            "    return copy.len + fill.len;\n"
            "}\n");
    rejects("struct H { n: i32 }\nfn f(a: [size_of(H)]byte) {}", 2, 10,
            "a length computed from `size_of` is allowed only in a struct "
            "field or a local variable");
    rejects("struct H { n: i32 }\nconst Z: [size_of(H)]byte = [0; 4];", 2, 11,
            "a length computed from `size_of` is allowed only in a struct "
            "field or a local variable");
    rejects("struct H { n: i32 }\nconst F: f64 = size_of(H) as f64;", 2, 16,
            "a value computed from `size_of` converts only to an integer type "
            "in a constant expression");
    rejects("struct H { n: i32 }\nconst D: int = size_of(H) / 0;", 2, 16,
            "`size_of(H) / 0` divides by zero");
    rejects("struct H { n: i32 }\nconst A: [2]int = [1, 2];\n"
            "const X: int = A[size_of(H) - 4];", 3, 18,
            "an index computed from `size_of` is not a constant expression");
    rejects("struct S { a: [size_of(S)]byte }", 1, 8,
            "struct `S` contains itself");
```

The ctest tests `unit`, `dump_types_scale`, `error_types` and `warning_note` pass on the development Mac.

## Next

[Chapter 7, The intermediate representation]({{% relref "/programming/writing-a-compiler/07-intermediate-representation" %}}), defines a target-independent, typed, three-address IR of functions, basic blocks and explicit control flow. Its types have no sizes. The IR holds a table of aggregate types and symbolic `size_of` and `offset_of` values, which the back end folds. The chapter explains why the IR has that shape, and what must never appear in it.
