---
title: "Lowering the syntax tree to IR"
description: "How antic translates the checked syntax tree into IR: expressions, conditions, loops, calls, returns, symbolic sizes and the home of each local variable."
summary: "Expressions, conditions, both loop forms, calls and returns become blocks and instructions. Where locals live before register allocation. How address-taken variables are marked."
date: 2026-09-13T22:32:00+02:00
lastmod: 2026-09-15T03:53:52+02:00
draft: false
weight: 80
tags: [compilers, programming-languages]
keywords: [lowering, short-circuit evaluation, stack slots, address-taken variables, join block, symbolic sizes, target-sized conversions, ir dump]
---

## Previously

[Chapter 7, The intermediate representation]({{% relref "/programming/writing-a-compiler/07-intermediate-representation" %}}), defines a target-independent, typed, three-address IR of functions, basic blocks and explicit control flow. Its types have no sizes. A table of aggregate types and symbolic `size_of` and `offset_of` values take their place, and the back end folds them. The chapter explains why the IR has that shape, and what must never appear in it.

## Lowering in the pipeline

Lowering reads the syntax tree after semantic analysis and writes one IR module. Semantic analysis has already rejected every program that breaks a rule of [chapter 2, The Anti language]({{% relref "/programming/writing-a-compiler/02-the-anti-language" %}}). Every name in the tree is resolved, and every expression has a type. The entry point is `lower_module` in `src/lower.c`. It records where each symbol lives in the field `ir` of the symbol. For a local or a parameter, that field holds the temporary of its value or of its slot address. For a function it holds the index of the IR function.

```c
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
```

The function makes two passes over the items of the module. The first pass creates an IR function for every `fn` and `extern fn`, so a call can name a function that the file declares further down. The second pass lowers each function body.

The option `--dump-ir` runs lowering, checks the result with the verifier of chapter 7 and prints the IR. For `main.anti` of chapter 1 the output is the listing below. The test `dump_ir_main` compares it byte for byte with `tests/dump/main.ir`.

```text
fn main.scale(%0: i64) -> i64 {
b0:
    %1 = add i64 2, 4
    %2 = copy i64 %1
    %3 = mul i64 %0, %2
    ret i64 %3
}
fn main.main() -> i64 {
b0:
    %0 = call i64 @main.scale(7)
    ret i64 %0
}
```

The instruction `%2 = copy i64 %1` gives the variable `k` a temporary of its own. The section on temporaries for variables gives the reason. The optimizer of chapter 10 removes the copy and folds `add i64 2, 4` into 6.

## Scope of lowering

A `bool`, a `char`, an integer, a float or a pointer is a scalar value, and this chapter explains how scalar values lower. The integers include `c_long`, `c_ulong` and `c_wchar`. The function `ir_type_of` gives the IR type of each Anti type, as chapter 7 defines them.

```c
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
```

A struct, a union, an array, a `str` or a slice has the IR type `IR_AGG`. The function `is_aggregate` selects these types, and lowering keeps their values in memory and passes their addresses. Chapter 18 explains that part of `src/lower.c` for structs, unions and arrays, and chapter 19 for strings and slices. A function pointer is a `ptr`, and chapter 20 lowers the calls through it. The excerpts below are complete functions, and the text names the chapter of each branch that serves one of those types.

## Local variables

A local variable lives in one of two places before register allocation. A variable whose address the program never takes lives in a temporary. A variable whose address is taken lives in a stack slot, a piece of memory in the stack frame of the function. A pointer holds an address, and a temporary has none, so `&x` needs the slot. Register allocation in chapter 13 later places each temporary in a register or in the frame.

### Marking address-taken variables

Semantic analysis marks the variables in [chapter 6, Semantic analysis]({{% relref "/programming/writing-a-compiler/06-semantic-analysis" %}}). Three constructs take an address: `&x`, a method call whose function takes `*T` for a receiver of type `T`, and slicing an array. Each passes its operand to `mark_address_taken`. The function follows index and field expressions down to the variable they start from. It stops at a pointer or a slice, because their elements already live in memory.

```c
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
```

Lowering reads the flag `address_taken` of each symbol. It records the home of the variable in the field `ir` of the symbol. That field names the temporary of the value, or the temporary of the slot address.

### Stack slots

The entry block is the first block of a function. Every stack slot is reserved there, before the first statement. The function `reserve_slots` walks the body, nested blocks included, and emits one `slot` instruction per address-taken `let`. A `let` inside a loop body therefore has one slot, and each iteration stores into it again. The condition `is_aggregate` gives every aggregate local a slot too, which chapter 18 describes.

```c
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
```

A slot names the type of its variable, as `slot i64`, and holds no size. The back end computes the size and the alignment of that type for its target. The function `vtype_of` gives the type of a value in memory, a scalar type for every type of this chapter. The unit test below takes the address of a local `x`.

```anti
fn f() -> int
{
    let x = 5;
    let p = &x;
    *p = *p + 1;
    return x;
}
```

```text
fn main.f() -> i64 {
b0:
    %0 = slot i64
    store i64 5, %0
    %1 = copy ptr %0
    %2 = load i64 %1
    %3 = add i64 %2, 1
    store i64 %3, %1
    %4 = load i64 %0
    ret i64 %4
}
```

The temporary `%0` holds the address of the slot of `x`, and `let x = 5;` stores into it. The address of `p` is never taken, so `p` lives in `%1`. The final read of `x` loads from the slot, because the store through `p` changed it.

A parameter arrives in a temporary. An address-taken parameter also gets a slot, and the entry block stores the incoming value into it. The unit test `bump` lowers both kinds of variable.

```anti
const BIAS: i8 = -128;

fn bump(x: i8) -> i16
{
    let p = &x;
    *p -= BIAS;
    return x as i16 + (x as u8 as i16);
}
```

```text
fn main.bump(%0: i8 signext) -> i16 {
b0:
    %1 = slot i8
    store i8 %0, %1
    %2 = copy ptr %1
    %3 = load i8 %2
    %4 = sub i8 %3, -128
    store i8 %4, %2
    %5 = load i8 %1
    %6 = sext i16 %5
    %7 = load i8 %1
    %8 = zext i16 %7
    %9 = add i16 %6, %8
    ret i16 %9
}
```

The temporary `%1` holds the address of the slot of `x`, a slot for one `i8`. The address of `p` is never taken, so `p` lives in `%2`. The constant `BIAS` has no storage and appears as the operand `-128`. Each read of `x` loads from the slot again, because a store through a pointer can change the value between two reads. The parameter prints as `i8 signext`, the extension that chapter 7 records for a signed parameter of 8 bits.

### Temporaries for variables

A `let` for a variable in a temporary copies the value into a new temporary. An assignment writes that temporary again. The copy keeps the variable apart from the temporary its value came from. In `let a = b; b = 5;` the variable `a` keeps the old value of `b`, which would change with `b` if both shared one temporary. The first branch of `lower_let` builds an aggregate value in its slot, the subject of chapter 18.

```c
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
```

## Expressions

The function `lower_expr` returns an operand: a constant, a symbolic value or the temporary that holds the value of the expression. A literal becomes a constant operand without an instruction. A `-` directly before a literal forms one constant, as chapter 2 specifies.

```c
static struct ir_operand lower_expr(struct lowerer *l, const struct expr *e);

static double float_literal(const struct expr *literal, enum ir_type type)
{
    char digits[128];

    snprintf(digits, sizeof digits, "%.*s", (int)literal->as.text.length,
             literal->as.text.bytes);
    return type == IR_F32 ? (double)strtof(digits, NULL)
                          : strtod(digits, NULL);
}
```

Several branches of `lower_expr` belong to later chapters. An aggregate expression returns its address through `lower_address`, the subject of chapters 18 and 19. A field expression with a symbol names a constant or a function through its module, which chapter 9 describes. Chapter 20 lowers such a function to its address. The length of an array and the field of a struct belong to chapter 18. An index expression reads through `element_address`, which the section on memory operations shows.

### Operators

Both operands of a binary operator have the same type, and that type selects the IR operation. A float type selects `fadd` or `flt`. A signed integer type selects `sdiv` or `slt`, and an unsigned one `udiv` or `ult`. Every comparison gives an `i8`. Unary `-` becomes `neg` or `fneg`, and `~` becomes `not`. The operator `!` becomes `xor` with 1, because a `bool` holds 0 or 1.

```c
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
```

### Conversions

A conversion `x as T` becomes one instruction or none. Two types with the same IR type, such as `u32` and `char` or two pointer types, convert without an instruction.

| From | To | Instruction |
|---|---|---|
| Integer, `bool`, `char` or pointer | A type of the same IR type | None |
| Integer | An integer type never wider than the source | `trunc` |
| Signed integer | Any other integer type | `sext` |
| Unsigned integer or `bool` | Any other integer type | `zext` |
| Signed integer | Float | `sitof` |
| Unsigned integer | Float | `uitof` |
| Float | Signed integer | `ftosi` |
| Float | Unsigned integer | `ftoui` |
| `f32` | `f64` | `fext` |
| `f64` | `f32` | `ftrunc` |

Never wider means never wider on any of the six targets, which matters for the target-sized types. A `c_long` has 32 bits on Windows and 64 bits elsewhere, so a conversion from `c_long` to `i32` truncates and one from `c_long` to `i64` extends. The functions `min_bits` and `max_bits` give the narrowest and the widest width of an IR type on the six targets.

```c
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
```

The function `lower_cast` applies the table.

```c
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
```

The unit test below converts between `i32`, `c_long`, `c_wchar` and `i64`.

```anti
extern fn labs(n: c_long) -> c_long;

fn f(a: i32, w: c_wchar) -> i64
{
    let x = labs(a as c_long) + 1;
    return x as i64 + w as i64 + (x as i32) as i64;
}
```

```text
extern fn labs(clong) -> clong
fn main.f(%0: i32, %1: cwchar) -> i64 {
b0:
    %2 = sext clong %0
    %3 = call clong @labs(%2)
    %4 = add clong %3, 1
    %5 = copy clong %4
    %6 = sext i64 %5
    %7 = zext i64 %1
    %8 = add i64 %6, %7
    %9 = trunc i32 %5
    %10 = sext i64 %9
    %11 = add i64 %8, %10
    ret i64 %11
}
```

The conversion `a as c_long` is `sext clong`, which the back end turns into a copy on Windows, where both widths are 32 bits. The conversion `x as i32` is `trunc i32`, a copy on Windows and a truncation elsewhere. Lowering writes the unsigned form of every operation on `c_wchar`, so its conversion to `i64` is `zext`. The back end of chapter 11 turns it into `sext` where `wchar_t` is signed. In the test `bump` above, `x as i16` emits `sext`, and `x as u8 as i16` emits nothing for `as u8` and `zext` for `as i16`.

### Memory operations

The expression `*p` loads through the pointer `p`. The index expression `p[i]` multiplies `i` by `size_of` of the element and adds the product to `p` with `ptradd`. The call `alloc(T, n)` multiplies `n` by `size_of T` and calls the C function `malloc`. The call `free(p)` calls the C function `free`. Both C functions enter the IR module as `extern` functions when lowering first needs them.

The expression `size_of(T)` becomes the symbolic operand `size_of T`. The function `size_operand` builds it from the type in memory that `vtype_of` gives. The branch of `vtype_of` for an aggregate calls `agg_of`, which chapter 18 describes.

```c
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
```

The function `element_address` computes the address of an element. For a pointer base, `first_element` returns the pointer itself. For an array, a `str` or a slice it returns the address of element 0, which chapter 19 describes.

```c
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
```

The unit test `k` allocates four `i16` values, writes one and reads it back.

```anti
fn k() -> i16
{
    let p = alloc(i16, 4);
    p[2] = 7;
    let v = p[2];
    free(p);
    return v + size_of(i16) as i16;
}
```

```text
extern fn malloc(i64) -> ptr
extern fn free(ptr)
fn main.k() -> i16 {
b0:
    %0 = mul i64 4, size_of i16
    %1 = call ptr @malloc(%0)
    %2 = copy ptr %1
    %3 = mul i64 2, size_of i16
    %4 = ptradd %2, %3
    store i16 7, %4
    %5 = mul i64 2, size_of i16
    %6 = ptradd %2, %5
    %7 = load i16 %6
    %8 = copy i16 %7
    call void @free(%2)
    %9 = trunc i16 size_of i16
    %10 = add i16 %8, %9
    ret i16 %10
}
```

Lowering computes nothing at compile time, so `mul i64 2, size_of i16` stays in the IR. The optimizer of chapter 10 folds no symbolic value either. The back end of chapter 12 replaces `size_of i16` with 2 and runs the optimizer again, which folds the instructions that use it. The command `antic --dump-select --target macos-arm64` prints `mov x0, #8` for the argument of `malloc`.

### Constants

The name of a constant becomes the value that semantic analysis computed. A constant computed from `size_of` holds a symbolic value of chapter 6, and the function `constant` turns it into a symbolic operand. Every other constant becomes an integer or float operand.

```c
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
```

The function `sym_of` translates a symbolic value of semantic analysis into an entry of the IR table. It chooses each operation with `binary_op` and `narrows`, the functions of the sections on operators and conversions, so a symbolic value and a number take the same operations.

```c
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
```

A constant header size of two `i32` values shows the result.

```anti
const HEADER: int = size_of(i32) * 2;

fn bytes(n: int) -> int
{
    return HEADER + n * size_of(i64);
}
```

```text
fn main.bytes(%0: i64) -> i64 {
b0:
    %1 = mul i64 %0, size_of i64
    %2 = add i64 mul i64(size_of i32, 2), %1
    ret i64 %2
}
```

The constant `HEADER` becomes the operand `mul i64(size_of i32, 2)` of the addition, and `n * size_of(i64)` multiplies by the operand `size_of i64`. Lowering computes no number for either, because a size is a fact of the target.

## Assignment

A place is an expression that denotes a location, as chapter 2 defines. The function `lower_place` turns a place into a temporary, for a variable that lives in one, or into an address. An assignment evaluates the place first and the value second, the order of chapter 2. A compound assignment `x += e` reads the old value of the place before it evaluates `e`, the order of `x = x + e`. A place in a temporary receives the new value with `copy`, and an address receives it with `store`. The branch for an aggregate target and the one for a bitfield place belong to chapter 18.

```c
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
```

## Conditions

A condition in `if`, `while` or `do while` selects the block that runs next. The function `lower_branch` takes a condition and two blocks, one for true and one for false, and ends the current block with branches. For `a && b` it branches on `a` either to a new block that tests `b` or to the false block. For `a || b` it branches on `a` either to the true block or to a new block that tests `b`. The operator `!` swaps the two blocks. The right operand runs only when the result depends on it, which is the short-circuit rule of chapter 2.

```c
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
```

An `&&` or `||` outside a condition, as in `return a && b;`, produces a `bool` value. The function `short_circuit` copies the left operand into a result temporary. When the left operand decides the result, the branch goes straight to the join block, the block where both paths continue. Otherwise a block evaluates the right operand and assigns it to the result.

```c
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
```

The unit test `h` returns `a / b > 1 && a != b` for two `u32` parameters.

```text
fn main.h(%0: i32, %1: i32) -> i8 {
b0:
    %2 = udiv i32 %0, %1
    %3 = ugt i8 %2, 1
    %4 = copy i8 %3
    branch %3, b1, b2
b1:
    %5 = ne i8 %0, %1
    %4 = copy i8 %5
    jump b2
b2:
    ret i8 %4
}
```

## If chains

The function `lower_if` lowers an `if`, each `else if` and the `else`. Every condition gets a block for its body and a block for the false case. The false block holds the next condition or the `else` body. After a last condition without `else`, the false block is the join block after the chain. A body that reaches its end jumps to the join block. The join block is created only when some path needs it, so a chain whose every body returns has none.

```c
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
```

After `return`, `break` or `continue` the function has no current block. Anti has no labels, so no jump can reach a statement after one. The loop of `lower_block` stops on a null block, which skips such statements. The closing brace is an exit of the block, so the deferred statements of the scope run there.

```c
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
```

The unit test `pick` uses `&&`, `||` and `!` in conditions. Blocks are numbered in the order lowering creates them. The function `lower_if` creates `b1` for the first body and `b2` for the test of `!a` before it lowers the first condition. The blocks `b3` and `b4` that test `b` and `c > 0` therefore come after them.

```anti
fn pick(a: bool, b: bool, c: int) -> int
{
    if a && (b || c > 0) {
        return 1;
    } else if !a {
        return 2;
    }
    return 3;
}
```

```text
fn main.pick(%0: i8 zeroext, %1: i8 zeroext, %2: i64) -> i64 {
b0:
    branch %0, b3, b2
b1:
    ret i64 1
b2:
    branch %0, b6, b5
b3:
    branch %1, b1, b4
b4:
    %3 = sgt i8 %2, 0
    branch %3, b1, b2
b5:
    ret i64 2
b6:
    ret i64 3
}
```

The test of `!a` in `b2` branches to `b6` when `a` is true, which skips the body of `else if`. The join block `b6` exists because the last condition can be false, and the final `return 3` runs there. Both `bool` parameters print as `i8 zeroext`, because a `bool` holds 0 or 1 and extends with zeros.

## Loops

Both loop forms use three blocks: a test block for the condition, a body block and an exit block. A `while` loop jumps to its test block first, and a `do while` loop jumps to its body block first. The statement `continue` jumps to the test block, and `break` jumps to the exit block. The structure `loop` holds both targets, and its field `outer` links a nested loop to the loop around it.

```c
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
```

The unit test `count` has a `do while` loop with `continue` and `break`.

```anti
fn count(n: int) -> int
{
    let i = 0;
    do {
        i += 1;
        if i == 5 {
            continue;
        }
        if i > 8 {
            break;
        }
    } while i < n
    return i;
}
```

```text
fn main.count(%0: i64) -> i64 {
b0:
    %1 = copy i64 0
    jump b1
b1:
    %2 = add i64 %1, 1
    %1 = copy i64 %2
    %3 = eq i8 %1, 5
    branch %3, b4, b5
b2:
    %5 = slt i8 %1, %0
    branch %5, b1, b3
b3:
    ret i64 %1
b4:
    jump b2
b5:
    %4 = sgt i8 %1, 8
    branch %4, b6, b7
b6:
    jump b3
b7:
    jump b2
}
```

The body is `b1`, the test `b2` and the exit `b3`. The `continue` in `b4` jumps to the test, and the `break` in `b6` jumps to the exit. The temporary `%1` of `i` receives a new value in `b1` on every iteration.

### The for loop

A `for` builds four blocks rather than three: a test, a body, a step and an exit. The step holds the increment, and it is the target of `continue`.

`for` therefore has a lowering of its own. A rewrite into `while` puts the increment at the end of the body, where `continue` jumps over it, and the loop never ends.

```c
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
```

The bound is read before the test, so a `for` over a slice reads its length once. A range keeps the counter in a temporary of the type its ends have. A sequence counts in `i64` and reaches each element through a `ptradd` of the index times the element size.

### Deferred statements

The lowerer keeps one list per block, and a block runs its list in reverse at every exit. An entry of the list is a deferred statement or a local to tear down. A local of a class whose chain declares `destruct` or owns memory ends with its block. A `let` of such a class therefore adds an entry where the program would have written `defer destroy(&c)`.

```c
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
```

A `return` runs every scope out to the function, so `run_defers_to` walks to a stop of `NULL`. A `break` or a `continue` runs only the scopes the loop encloses, and each loop records the scope it began in for that reason.

```c
/* Run every scope from the innermost out to stop, which is not run. */
static void run_defers_to(struct lowerer *l, const struct defers *stop)
{
    const struct defers *scope;

    for (scope = l->defers; scope != stop; scope = scope->outer) {
        run_defers(l, scope);
    }
}
```

A returned value is copied into a temporary before the deferred statements run. Without the copy a deferred statement that writes the variable would change what the function returns.

A `return` of a local by name hands that local to the caller. Its owned memory belongs to the returned value now, so the exit action of that one local is skipped. The field `moved` of the lowerer names it, and every other local of the scope ends as usual. Without the rule a function that builds a value and returns it would free what it just gave away.

## Calls and returns

A call evaluates its arguments from left to right and emits `call` with the IR function of the callee. A call of a function without a result has no result temporary. The function `callee_function` finds the IR function, and chapter 9 describes it for imported functions. A call through a function pointer takes the other branch, `ir_call_indirect`, which chapter 20 describes.

```c
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
```

The statement `return e;` emits `ret` with the value of `e`, and `return;` emits `ret` without one. A function without a result can reach the end of its body, and lowering adds a `ret` there. Semantic analysis has already rejected a function with a result that can reach its end. The function `lower_function` sets up the entry block with the slots and then lowers the body. The test `!is_aggregate` leaves aggregate parameters to chapter 18.

```c
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
```

## Objects

A class carries its base as field 0, so lowering needs no step per level to reach an inherited field. The function `field_owner` finds the class of the chain that declares a name. The address of the field is the object plus the offset of that field in that class.

### Tables and descriptors

Every class a module declares builds five globals: its table, its descriptor, its ancestors, its field list and its function list. Each interface it implements adds a table and a descriptor of its own. They are built for the class and not for a literal of it. A library that declares a class it never constructs therefore still carries the data its consumers need. An abstract class has no complete value and therefore no table.

The table holds the descriptor at entry 0, then one entry per public function of the chain. The function `table_of` walks the chain from the root down, appending each public function. A function that repeats a name replaces the entry rather than adding one. The entries of a base therefore keep their indices in every class below it.

The table of an interface repeats the interface's own chain, and its entries are thunks. A thunk subtracts the offset of the sub-object from `self` and calls the function of the class with the object. Every function is therefore written once and serves the direct call and the interface call alike. Where the class fills nothing, the interface's own body takes the entry. That body already takes the sub-object.

```text
global classes.Circle.table [11]ptr { @classes.Circle.descriptor,
  @anti_rt_Object_type_name, ..., @classes.Shape.move,
  @classes.Shape.span, @classes.Circle.area }
```

The descriptor names the class and points at the descriptor of its base. The record holds the size of the class as a symbolic value and its depth in the chain. Then come its ancestors, its field list and its function list. It also holds the `destruct` body the class declares, and not the one it inherits, because `delete` runs one body per level. Last comes the offset of the sub-object, which is zero for a class and the distance to the object for an interface table.

The ancestors are the descriptors from the root down, one per level. The entry at a level is the same address in every class below it, which is what makes `is` one comparison after the depth check.

### Literals and construction

A literal of a class stores the address of the table into the first word of the object. That word is the table field of the root, and it sits at offset 0. The table pointer of every interface sub-object follows, each at the offset of its field. The literal then writes each named field at the offset of the class that declares it, and the defaults of every level it left out. Last it runs `construct`, base first down the chain. A base therefore sees its own fields before the class below adds to them.

The heap form calls `malloc` with the size of the literal's type. It writes the literal into the result and runs the same chain. `alloc(T, n)` multiplies the count by the size, as before.

`Circle(2.0)` and `alloc Circle(2.0)` write the defaults and then run the `construct` that takes arguments. That one may fail, so it gives a pointer back, and the branch after it is the error branch of chapter 7. On a failure the heap form frees the object before the handler runs.

### Dispatch and bound functions

`self.super.f(args)` is a direct call. The base part of the object is a value, its class is the base, and the entry is found at compile time.

A call the checker marked reads the table from the first word of the object. It reads the entry at the index of the function and calls it indirectly. The function `table_index` computes the index from the static class. The function `entry_offset` multiplies it by the symbolic size of a pointer.

A bound function is an aggregate of two words. Lowering stores the object in the first word and the entry in the second. A call loads both and calls the entry with the object as its first argument. A `final` function and a `final` class store the address of the body instead.

### Lifetime

`dup`, `delete` and `destroy` each become one call of the runtime with the pointer. The runtime reads the descriptor of the object, so the concrete class decides what happens, whatever the static type of the pointer says. It also moves back by the offset the descriptor holds. Each of the three therefore works through an interface pointer.

A `delete` runs the `destruct` body of the concrete class, then the body of each class up the chain. It then frees the memory behind every `own` field of each level, and the object. A local of a class whose chain declares `destruct` or owns memory is torn down at the end of its block, which the exit actions below carry.

The test `is` loads the descriptor through the table. It compares the depth of the object with the depth of the target. It then compares the ancestor at that depth with the descriptor of the target. A cast with `as` adds a call of the runtime on the failing path, and `as?` writes null there. A cast that leaves an interface sub-object moves the pointer back by the offset the descriptor holds.

### Handling an error

A call that can fail gives a pointer, and lowering follows it with the compare and the branch of chapter 7. The success arm carries on, and the failure arm is the handler.

A `catch` block binds the pointer to the name the program wrote and lowers the block. The handler ends by deleting the error, because the error belongs to it, unless it hands the error to the caller with `return e`. A `yield` writes its value where the call would have written its result and jumps past the handler.

A `try` runs the deferred statements of the function and returns the pointer. `catch fatal` reads the entry of `fatal` from the table of the error's class and calls it. A `try` block gives one handler to every failing call inside it, so each of them branches to the same block, and the handler runs once.

The out pointer of a failing call is the address of what the `let` declares. That local therefore gets a place of its own, which the entry block reserves like any address-taken local.

### Static and atomic fields

A static field is a global of the module that declares its class, named `Class.field`, with the declared value written into it and the `mutable` flag set. Every mention of the field reaches the same global.

An atomic operation becomes a call of the runtime with the address, the symbolic width of the value and the operands. A narrower value extends to 64 bits on the way in and truncates on the way out, so one runtime symbol serves every width.

## Tests

Every construct of this chapter is lowered, printed and verified against an expected text, and the listing of `main.anti` is pinned. The build has zero warnings, and every `ctest` test passes.

## Next

[Chapter 9, Modules and library files]({{% relref "/programming/writing-a-compiler/09-modules-and-library-files" %}}), names each module by a path that mirrors a directory tree under the search roots. It adds `import` with `as`, `pub`, `export` and name mangling with length-prefixed segments on COFF. The `anti.` root is reserved for the language, reverse-domain roots serve third-party libraries, and single-segment names serve a program's own files. The `.antl` file holds a package header with licence fields, the public interface with its doc text and the unoptimised IR. The serialiser and the deserialiser write and check a version stamp. The tests check that library files are byte-identical on every host and that both doc comment forms write the same file.
