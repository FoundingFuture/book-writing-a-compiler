---
title: "The intermediate representation"
description: "The design of antic's intermediate representation: typed three-address code in basic blocks, types without sizes, a text form and a verifier."
summary: "A target-independent, typed, three-address IR of functions, basic blocks and explicit control flow. Types without sizes: a table of aggregate types and symbolic `size_of` and `offset_of` values that the back end folds. Why it has that shape, and what must never appear in it."
date: 2026-09-13T22:32:00+02:00
lastmod: 2026-09-15T03:55:47+02:00
draft: false
weight: 70
tags: [compilers, programming-languages]
keywords: [intermediate representation, three-address code, basic blocks, control flow graph, symbolic sizes, ir verifier, aggregate types, target independence]
---

## Previously

[Chapter 6, Semantic analysis]({{% relref "/programming/writing-a-compiler/06-semantic-analysis" %}}), builds scopes and symbol tables, name resolution and the type checker without implicit conversions. It enforces mandatory initialisation and applies the method-call rewrite. Constants computed from `size_of` stay symbolic. The chapter checks unions, bitfields and export signatures and computes the pointer-free property of types that the threading chapter uses.

## Role of the IR

The checked syntax tree still has the shape of the source. An expression nests to any depth, a loop is a node with a body, and a field access hides an address calculation. A back end needs short steps that each map to a few processor instructions. The intermediate representation, IR, sits between the two. Lowering, the subject of chapter 8, translates the tree into IR. The optimizer of chapter 10 rewrites IR, and the back ends of chapters 12 to 16 translate IR into assembly.

The IR also travels between machines. [Chapter 2]({{% relref "/programming/writing-a-compiler/02-the-anti-language" %}}) and the design of the book put the unoptimised IR of each library into its `.antl` file. One library file serves all six targets. It must be byte for byte the same whichever host produced it. The test `antl_scale` compares the bytes of one library file with a stored list. The IR therefore holds nothing that depends on a target, and that includes the size of a type.

## Shape of the IR

The IR of antic has four properties.

- It is typed. Every value that an instruction computes has a scalar type such as `i32`, `f64` or `ptr`.
- It is three-address code. An instruction has one operation, at most two operands and at most one result.
- Control flow is explicit. A function is a list of basic blocks, each a sequence of instructions that ends with a jump, a branch or a return.
- It is the same on all six targets. It holds no size, no offset and no register class.

A basic block has one entry, its first instruction, and one exit, its last. No jump lands in the middle of a block, and no instruction before the last leaves it. An optimizer can therefore reason about a block as a unit. The jumps and branches between blocks form the control flow graph of the function.

### Types in the IR

The IR has fewer types than Anti. `bool` becomes `i8` and `char` becomes `i32`. Every pointer and function pointer becomes `ptr`. Signed and unsigned integers of one width share a type, and the operation carries the signedness: `sdiv` and `udiv`, `slt` and `ult`, `sshr` and `ushr`. Two's complement addition, subtraction and multiplication give the same bits for both, so those need no second form.

```c
/* Scalar value types. bool is i8, char is i32, and every pointer and
   function pointer is ptr. Signedness lives in the operations. */
enum ir_type {
    IR_VOID,
    IR_I8,
    IR_I16,
    IR_I32,
    IR_I64,
    IR_F32,
    IR_F64,
    IR_PTR,
    IR_AGG,     /* a struct, union, array, str or slice passed by value */
    IR_CLONG,   /* c_long and c_ulong: 32 bits on Windows, else 64 */
    IR_CWCHAR   /* c_wchar: 16 bits on Windows, else 32 */
};
```

The C types `c_long` and `c_ulong` become `clong`, and `c_wchar` becomes `cwchar`. Their widths differ between targets. A `c_long` has 32 bits on Windows and 64 bits on Linux and macOS, and a `c_wchar` has 16 and 32 bits. The IR keeps each of them as a type of its own, and the back end gives it the width of the target. [Chapter 2]({{% relref "/programming/writing-a-compiler/02-the-anti-language" %}}) makes the other `c_` types aliases of the sized types, so `c_int` is `i32` in the IR.

The type `IR_AGG` marks a value of a struct, a union, an array, a `str` or a slice. Such a value has no scalar type. The IR keeps it in memory and handles it through a `ptr` to that memory.

### Types without sizes

A comment at the top of `src/ir.h` states which facts the IR leaves to the back end.

```c
/* The intermediate representation of antic: typed three-address code in
   functions of basic blocks. It is the same for all six targets. It holds
   no register, no calling convention and no symbol mangling, because the
   back end decides those per target.

   DESIGN: the IR records types, never sizes, offsets or register classes.
   A size, an offset or an array stride is a symbolic value that names the
   type. The back end folds it after it lays out the types for its target,
   so a library file holds the same bytes on every host. */
```

A size is a fact of the target. A `c_long` takes 4 bytes on Windows and 8 bytes elsewhere. Bitfields follow the System V rule on Linux and macOS and the MSVC rule on Windows, so one struct with bitfields can have two layouts. A library file that held the number 8 for a `c_long` would be wrong on Windows. The IR records the type `clong`, and every target reads the same bytes.

An aggregate therefore appears in the IR as a type, and a size or an offset as a symbolic value that names a type. A module holds a table of aggregate types and a table of symbolic values.

### Aggregate types

The first table has one entry for each aggregate type that the module uses.

```c
#define IR_NO_AGG UINT32_MAX

/* The type of a value in memory: a scalar, or IR_AGG with the index of an
   aggregate in the module's type table. */
struct ir_vtype {
    enum ir_type type;
    uint32_t agg;
};

enum ir_agg_kind { IR_AGG_STRUCT, IR_AGG_UNION, IR_AGG_ARRAY };

/* How a parameter of 8 or 16 bits extends to 32 bits. The IR keeps
   signedness in operations, and a parameter has none, so the signature
   records it for the conventions whose callers extend. */
enum ir_ext { IR_EXT_NONE, IR_EXT_SIGN, IR_EXT_ZERO };

/* A field of a struct or union, or the element of an array. A bitfield
   has a width in bits and extends by its signedness when it is read. */
struct ir_field {
    const char *name;               /* NULL for an array element */
    struct ir_vtype type;
    uint8_t bits;                   /* 0 for a field that is not a bitfield */
    enum ir_ext ext;
};

/* An aggregate type. A struct or union lists its fields, and an array has
   one field for its element and a symbolic length. */
struct ir_aggtype {
    enum ir_agg_kind kind;
    const char *name;               /* main.Vec2, str, []i32 or [4]i32 */
    struct ir_field *fields;
    size_t field_count;
    bool packed;
    uint64_t align;                 /* the N of align(N), or 0 */
    uint32_t length;                /* IR_AGG_ARRAY: a symbolic value */
    const char *length_text;        /* IR_AGG_ARRAY: the length as written */
};
```

The structure `ir_vtype` names the type of a value in memory: a scalar type, or `IR_AGG` with the index of an entry in the table. A struct or union entry lists its fields in declaration order, each with its name and its `ir_vtype`. A bitfield has its width in `bits`, and `ext` records whether a read extends it with its sign or with zeros. A zero-width bitfield has the name `_` and the width 0, and the text form prints it as `_: i32 : 0`. The fields `packed` and `align` carry the modifiers of chapter 2. An array entry has one field for its element and a symbolic value for its length. It also keeps the length as the source wrote it, which the error message of a length below 1 quotes.

An entry is identified by its name. A struct name carries its module, as `main.Vec2`, and an array name its length, as `[4]i32`. The functions `ir_struct_add` and `ir_array_add` return the index of an existing entry with the same name. A `str` and every slice are structs with a pointer field `ptr` and a length field `len`.

The function `aggtype` of `src/ir_print.c` writes one entry of the table as a line of text.

```c
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
```

A union entry prints with the word `union`, as `type main.Value = union { i: i64, f: f64 }`. The modifiers print before and after the word `struct`, as `type main.P = packed struct { a: i8, b: i32 }` and `type main.A = struct align(16) { a: i8 }`.

### Symbolic values

A symbolic value is an integer that depends on the target. An operand refers to an entry of the table of symbolic values by its index.

```c
enum ir_sym_kind {
    IR_SYM_INT,         /* a number */
    IR_SYM_SIZE_OF,     /* size_of T */
    IR_SYM_OFFSET_OF,   /* offset_of T.f */
    IR_SYM_OP           /* an operation on one or two symbolic values */
};

/* A symbolic value, an integer that depends on the target. */
struct ir_sym {
    enum ir_sym_kind kind;
    enum ir_type type;
    uint64_t value;                 /* IR_SYM_INT */
    struct ir_vtype of;             /* IR_SYM_SIZE_OF, IR_SYM_OFFSET_OF */
    uint32_t field;                 /* IR_SYM_OFFSET_OF */
    int op;                         /* IR_SYM_OP, an enum ir_op */
    uint32_t a;                     /* IR_SYM_OP */
    uint32_t b;                     /* IR_SYM_OP, IR_NO_AGG for one operand */
};
```

The four kinds cover every size the IR needs.

- `IR_SYM_INT` is a number, used as an operand of an operation.
- `IR_SYM_SIZE_OF` is `size_of T`, the size in bytes of the type `T` in `of`.
- `IR_SYM_OFFSET_OF` is `offset_of T.f`, the offset of field number `field` of the aggregate in `of`.
- `IR_SYM_OP` applies an IR operation such as `sub` or `mul` to the entry `a`, or to `a` and `b`.

Sizes and offsets have the type `i64`. An operation has the type of its result, so a comparison of two sizes is an `i8`. The functions `ir_sym_int`, `ir_sym_size_of`, `ir_sym_offset_of` and `ir_sym_op` add an entry or return the index of an equal one. The length `size_of(Foo) - 1` becomes an entry of kind `IR_SYM_OP` with two operand entries, `size_of main.Foo` and the number 1.

The IR writes the stride of an array, the distance between two elements, as `size_of` of the element type. C makes the stride equal to the element size, so indexing through an array, a slice or a pointer multiplies the index by `size_of` of the element. C also places the first field of a struct and every field of a union at offset 0 on every target. The IR writes the number 0 for those offsets.

An instruction uses a symbolic value through an operand of kind `IR_SYM`. The function `ir_sym_operand` builds that operand, and it gives a plain integer operand for a number.

```c
struct ir_operand ir_sym_operand(const struct ir_module *m, uint32_t sym)
{
    struct ir_operand o = {IR_SYM, IR_VOID, {0}};
    const struct ir_sym *s = &m->syms[sym];

    if (s->kind == IR_SYM_INT) {
        return ir_int_op(s->type, s->value);
    }
    o.type = s->type;
    o.as.index = sym;
    return o;
}
```

The function `ir_sym_print` writes a symbolic value in the text form. An operation prints its operands in parentheses after its name and type, as `sub i64(size_of main.Foo, 1)`.

```c
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
```

### Folding of symbolic values

No stage before the back end computes a symbolic value. The optimizer of chapter 10 folds only integer and float constants, so an operand of kind `IR_SYM` stays in the IR. It also leaves every operation on `clong` or `cwchar` unfolded, because their arithmetic wraps at a width that only the back end knows. The back end of chapter 12 lays out every aggregate for its target and then replaces each symbolic operand with a number. It runs the optimizer again on each function that held a symbolic value. Chapter 18 gives the layout rule of every aggregate.

A packed struct and an aligned struct show the fold of an offset. The test source below is lowered by chapter 8, and `antic --dump-ir` prints the table of aggregate types before the functions.

```anti
packed struct P
{
    a: u8,
    b: i32,
}

struct A align(16)
{
    a: u8,
}

fn f(p: *P, a: A) -> i32
{
    return p.b + a.a as i32;
}
```

```text
type main.A = struct align(16) { a: i8 }
type main.P = packed struct { a: i8, b: i32 }
fn main.f(%0: ptr, %1: agg main.A) -> i32 {
b0:
    %2 = ptradd %0, offset_of main.P.b
    %3 = load i32 %2
    %4 = load i8 %1
    %5 = zext i32 %4
    %6 = add i32 %3, %5
    ret i32 %6
}
```

The parameter `a` has the type `agg main.A`, and its temporary `%1` holds the address of the value. The field `b` lies at `offset_of main.P.b`. The back end folds that value to 1 on all six targets, and to 4 for the same struct without `packed`. The field `a` of `A` is its first field and lies at offset 0 on every target, so `load i8 %1` reads it without a `ptradd`.

An array length computed from `size_of` shows why the fold waits for the target. The field `pad` of the struct below has the length `size_of(c_long) - 6`.

```anti
struct Tail
{
    pad: [size_of(c_long) - 6]byte,
}

fn main() -> int
{
    let t = Tail { pad: [0; size_of(c_long) - 6] };
    return t.pad[0] as int;
}
```

The IR of this program is the same for all six targets. Its table holds the entry `type [size_of(c_long) - 6]byte = array sub i64(size_of clong, 6) of i8`. With `--target` set to one of the four Linux and macOS targets, `antic -S main.anti` succeeds, and the length is 2. On both Windows targets the length is -2, and `antic --target windows-x86_64 -S main.anti` stops with one message.

```text
antic: the array length `size_of(c_long) - 6` is -2 on windows-x86_64, and an array length is at least 1
```

### Temporaries

A temporary, written `%n`, holds one scalar value. The IR of antic is not in static single assignment form, SSA, where each temporary receives its value in exactly one instruction. A temporary here may receive a value in several instructions, which is how a loop variable changes. Chapter 27 lists SSA among the extensions. The optimizer of chapter 10 works without it.

## Instructions

The operations fall into six groups, from arithmetic and comparisons to the terminators that end a block.

```c
enum ir_op {
    /* result = op a, b: both operands and the result have the same type */
    IR_ADD, IR_SUB, IR_MUL, IR_SDIV, IR_UDIV, IR_SREM, IR_UREM,
    IR_AND, IR_OR, IR_XOR, IR_SHL, IR_SHR_S, IR_SHR_U,
    IR_FADD, IR_FSUB, IR_FMUL, IR_FDIV,
    /* result = op a */
    IR_NEG, IR_NOT, IR_FNEG, IR_COPY,
    /* result i8 = op a, b: 1 when the comparison holds, else 0 */
    IR_EQ, IR_NE, IR_SLT, IR_SLE, IR_SGT, IR_SGE,
    IR_ULT, IR_ULE, IR_UGT, IR_UGE,
    IR_FEQ, IR_FNE, IR_FLT, IR_FLE, IR_FGT, IR_FGE,
    /* result = op a, with the result type named by the instruction */
    IR_TRUNC, IR_SEXT, IR_ZEXT, IR_SITOF, IR_UITOF, IR_FTOSI, IR_FTOUI,
    IR_FEXT, IR_FTRUNC,
    /* Memory. */
    IR_SLOT,        /* Result ptr: a stack slot for a value of type of. */
    IR_LOAD,        /* Result: load type from a. */
    IR_STORE,       /* Store a of type into b. */
    IR_PTRADD,      /* Result ptr: a plus b bytes. */
    IR_MEMCOPY,     /* Copy a value of type of from b to a. */
    IR_ADDR,        /* Result ptr: address of a. */
    IR_BITLOAD,     /* Result: load bitfield field of aggregate of from a. */
    IR_BITSTORE,    /* Store a into bitfield field of aggregate of at b. */
    /* Calls. */
    IR_CALL,        /* Result: call a with args, as signature b if set. */
    /* Terminators, the last instruction of a block. */
    IR_JUMP,        /* Go to block a. */
    IR_BRANCH,      /* Go to b when a is nonzero, else c. */
    IR_RET          /* Return a, or nothing. */
};
```

An operand is a temporary, an integer or float constant, a global, a function, a basic block or a symbolic value.

```c
enum ir_operand_kind {
    IR_NONE,
    IR_TEMP,        /* A temporary, %n. */
    IR_INT,         /* An integer constant. */
    IR_FLOAT,       /* A float constant. */
    IR_GLOBAL,      /* a global data item, by index */
    IR_FUNC,        /* a function, by index */
    IR_BLOCK,       /* a basic block, by index */
    IR_SYM          /* a symbolic value, by index */
};

struct ir_operand {
    enum ir_operand_kind kind;
    enum ir_type type;
    union {
        uint32_t temp;
        uint64_t integer;
        double floating;
        uint32_t index;
    } as;
};
```

An instruction stores its operation, its type, its result temporary and up to three operands. A call adds an argument list. A slot, a memory copy and the two bitfield operations name a type in `of`, and the bitfield operations also name a field.

```c
#define IR_NO_RESULT UINT32_MAX

struct ir_inst {
    enum ir_op op;
    enum ir_type type;              /* the result type, or the stored type */
    uint32_t result;                /* a temporary, or IR_NO_RESULT */
    struct ir_operand a;
    struct ir_operand b;
    struct ir_operand c;
    struct ir_vtype of;             /* IR_SLOT, IR_MEMCOPY, the bitfield ops */
    uint32_t field;                 /* IR_BITLOAD, IR_BITSTORE */
    struct ir_operand *args;        /* IR_CALL */
    size_t arg_count;
};
```

### Memory operations

Every address calculation of a program appears in the IR as an instruction.

- `slot T` reserves a stack slot for a value of type `T` and gives its address.
- `load` and `store` read and write one scalar.
- `ptradd` adds a byte offset to an address. The offset of a field is `offset_of`, and the offset of an element is its index times `size_of` of the element.
- `memcopy dst, src, T` copies a value of the aggregate type `T`.
- `addr` gives the address of a global or a function.
- `bitload` and `bitstore` read and write a bitfield, named by its aggregate and its field.

A field access `p.y` of chapter 2 becomes a `ptradd` of `offset_of` for the field and a `load`. A bitfield has no byte offset of its own, because its storage unit, bit position and mask depend on the layout rule of the target. The back end of chapter 18 replaces `bitload` with a load of the integer that holds the field, a shift and a mask. It replaces `bitstore` with a load of that integer, a mask that clears the field, the new bits and a store. The test source below writes and reads two bitfields through a pointer.

```anti
struct Flags
{
    visible: u32 : 1,
    level: i8 : 3,
}

fn f(p: *Flags) -> i8
{
    p.visible = 1;
    p.level -= 2;
    return p.level;
}
```

```text
type main.Flags = struct { visible: i32 : 1 zeroext, level: i8 : 3 signext }
fn main.f(%0: ptr) -> i8 {
b0:
    bitstore i32 1, %0, main.Flags.visible
    %1 = bitload i8 %0, main.Flags.level
    %2 = sub i8 %1, 2
    bitstore i8 %2, %0, main.Flags.level
    %3 = bitload i8 %0, main.Flags.level
    ret i8 %3
}
```

The entry of `main.Flags` records both widths. The field `level` has the signed type `i8`, and `signext` in the table marks it. A read of `level` extends its 3 bits with the sign, and a read of `visible` with zeros.

## Functions, globals and modules

A function has parameters, a result type, basic blocks and the type of every temporary. An `extern` function has parameters and a result but no blocks. A parameter or result of aggregate type names its entry in the table, and the text form prints it as `agg main.A`. A parameter of 8 or 16 bits records its signedness in `ext`, because the callers of some conventions extend it, as chapter 15 describes.

```c
/* A parameter. An aggregate parameter's temporary holds a pointer to the
   value, and the back end copies it in as its ABI requires. */
struct ir_param {
    enum ir_type type;
    enum ir_ext ext;
    uint32_t agg;                   /* IR_AGG: the aggregate, else IR_NO_AGG */
    uint32_t temp;
};

struct ir_function {
    uint32_t index;
    const char *module;             /* NULL for a C function */
    const char *name;
    struct ir_param *params;
    size_t param_count;
    size_t param_capacity;
    enum ir_type result;
    uint32_t result_agg;            /* IR_AGG: the aggregate, else IR_NO_AGG */
    bool is_extern;                 /* no body: C or another module */
    bool variadic;
    bool exported;                  /* an export fn, with a C symbol */
    struct ir_block **blocks;
    size_t block_count;
    size_t block_capacity;
    enum ir_type *temps;            /* the type of each temporary */
    uint32_t temp_count;
    uint32_t temp_capacity;
};
```

A function is named by its module and its name, as `main.scale`. The IR does not mangle names. Chapter 9 turns the pair into a symbol for each object file format.

A global is a block of constant bytes, such as the bytes of a string literal, with relocations for the addresses it contains. A module holds the aggregate types, the symbolic values, the functions and the globals of one source file.

```c
struct ir_module {
    struct arena *arena;
    const char *name;
    struct ir_aggtype **aggs;
    size_t agg_count;
    size_t agg_capacity;
    struct ir_sym *syms;
    size_t sym_count;
    size_t sym_capacity;
    struct ir_function **functions;
    size_t function_count;
    size_t function_capacity;
    struct ir_global **globals;
    size_t global_count;
    size_t global_capacity;
};
```

## Builder

Lowering creates IR through a small set of functions. Each emitter appends one instruction to a block. An emitter with a result creates a new temporary and returns its number. The function `ir_assign` writes to an existing temporary, which a variable in a loop needs.

The unit test for a loop builds a function that adds the numbers from 0 to `n - 1`. It uses four blocks: the entry, the loop test, the loop body and the exit. The constant `IR_NO_AGG` says that the result and the parameter are scalars.

```c
static void loop(void)
{
    struct arena arena = {0};
    struct ir_module m;
    struct ir_function *f;
    struct ir_block *entry, *test, *body, *done;
    uint32_t n, total, i, less;

    ir_module_init(&m, &arena, "main");
    f = ir_function_add(&m, "main", "sum", IR_I64, IR_NO_AGG);
    n = ir_param_add(f, IR_I64, IR_NO_AGG);
    entry = ir_block_add(f);
    test = ir_block_add(f);
    body = ir_block_add(f);
    done = ir_block_add(f);
    total = ir_unary(f, entry, IR_COPY, IR_I64, ir_int_op(IR_I64, 0));
    i = ir_unary(f, entry, IR_COPY, IR_I64, ir_int_op(IR_I64, 0));
    ir_jump(f, entry, test);
    less = ir_binary(f, test, IR_SLT, IR_I8, ir_temp_op(f, i),
                     ir_temp_op(f, n));
    ir_branch(f, test, ir_temp_op(f, less), body, done);
    ir_assign(f, body, total,
              ir_temp_op(f, ir_binary(f, body, IR_ADD, IR_I64,
                                      ir_temp_op(f, total), ir_temp_op(f, i))));
    ir_assign(f, body, i,
              ir_temp_op(f, ir_binary(f, body, IR_ADD, IR_I64,
                                      ir_temp_op(f, i), ir_int_op(IR_I64, 1))));
    ir_jump(f, body, test);
    ir_ret(f, done, IR_I64, ir_temp_op(f, total));
```

The unit test `symbolic` adds a struct, a symbolic size, a symbolic length and an array of that length. Adding the same struct and the same size a second time returns the same indexes.

```c
    foo = ir_struct_add(&m, IR_AGG_STRUCT, "main.Foo", fields, 2, false, 0);
    CHECK(ir_struct_add(&m, IR_AGG_STRUCT, "main.Foo", fields, 2, false, 0) ==
          foo);
    size = ir_sym_size_of(&m, ir_aggregate(foo));
    CHECK(ir_sym_size_of(&m, ir_aggregate(foo)) == size);
    length = ir_sym_op(&m, IR_SUB, IR_I64, size, ir_sym_int(&m, IR_I64, 1));
    bytes = ir_array_add(&m, "[size_of(main.Foo) - 1]byte", ir_scalar(IR_I8),
                         length, "size_of(Foo) - 1");
    CHECK(ir_sym_operand(&m, ir_sym_int(&m, IR_I64, 7)).kind == IR_INT);
    f = ir_function_add(&m, "main", "f", IR_I64, IR_NO_AGG);
    x = ir_param_add(f, IR_I64, IR_NO_AGG);
    b0 = ir_block_add(f);
    copy = ir_slot(f, b0, ir_aggregate(bytes));
    scaled = ir_binary(f, b0, IR_MUL, IR_I64, ir_temp_op(f, x),
                       ir_sym_operand(&m, size));
    sum = ir_binary(f, b0, IR_ADD, IR_I64, ir_temp_op(f, scaled),
                    ir_sym_operand(&m, length));
    ir_memcopy(f, b0, ir_temp_op(f, copy), ir_temp_op(f, copy),
               ir_aggregate(bytes));
    ir_ret(f, b0, IR_I64, ir_temp_op(f, sum));
```

## Text form

The function `ir_print` writes a module as text: the table of aggregate types, the `extern` functions, the globals and the functions with their blocks. The unit tests compare that text with expected strings, and the dump tests of the later chapters with expected files. The loop above prints as follows.

```text
fn main.sum(%0: i64) -> i64 {
b0:
    %1 = copy i64 0
    %2 = copy i64 0
    jump b1
b1:
    %3 = slt i8 %2, %0
    branch %3, b2, b3
b2:
    %4 = add i64 %1, %2
    %1 = copy i64 %4
    %5 = add i64 %2, 1
    %2 = copy i64 %5
    jump b1
b3:
    ret i64 %1
}
```

The comparison `slt` gives an `i8` of 0 or 1, and `branch` goes to `b2` when it is not zero. The temporaries `%1` and `%2` receive new values in `b2`, which SSA would not allow.

A `str` shows the memory operations. The test `memory` builds a `str` in a stack slot and passes its pointer to the C function `puts`.

```text
type str = struct { ptr: ptr, len: i64 }
extern fn puts(ptr) -> i32
global main.str.0 size 3 align 1 bytes 68 69 00
fn main.main() -> i64 {
b0:
    %0 = slot str
    %1 = addr @main.str.0
    store ptr %1, %0
    %2 = ptradd %0, offset_of str.len
    store i64 2, %2
    %3 = load ptr %0
    %4 = call i32 @puts(%3)
    %5 = sext i64 %4
    ret i64 %5
}
```

The slot names its type `str`, and the back end gives it 16 bytes on every target. The pointer field `ptr` is the first field, so `store ptr %1, %0` writes it at the address of the slot. The length field lies at `offset_of str.len`.

The test `symbolic` prints the struct, the array and the symbolic values of the builder code above.

```text
type main.Foo = struct { a: i8, b: i32 }
type [size_of(main.Foo) - 1]byte = array sub i64(size_of main.Foo, 1) of i8
fn main.f(%0: i64) -> i64 {
b0:
    %1 = slot [size_of(main.Foo) - 1]byte
    %2 = mul i64 %0, size_of main.Foo
    %3 = add i64 %2, sub i64(size_of main.Foo, 1)
    memcopy %1, %1, [size_of(main.Foo) - 1]byte
    ret i64 %3
}
```

The operand `size_of main.Foo` in `%2` scales `%0` by the size of the struct. The length of the array appears twice, in the table and as the operand of `add`. Both refer to one entry of the table of symbolic values.

## Excluded from the IR

Six kinds of information never appear in the IR, because each depends on the target.

- Layouts. A type or a symbolic value stands for each size, offset and alignment, and the back end computes the layout for its target.
- Registers. A temporary names a value, and register allocation in chapter 13 decides where the value lives.
- Register classes. Whether an aggregate travels in integer registers, float registers or memory follows from its layout, and chapter 18 classifies it for each convention.
- Calling conventions. A call lists its arguments, and an aggregate argument is a pointer to its value. The back end moves arguments into registers or onto the stack as the ABI of the target requires.
- Symbol names. A function is a module and a name, and the back end derives the symbol.
- Instruction selection. An operation such as `mul` says what to compute, and chapter 12 decides which instructions compute it.

A `.antl` file built on a Mac and one built on Linux therefore hold the same IR.

## Verifier

Every stage that writes IR can make a mistake that shows up only much later as wrong assembly. The verifier checks the rules of the IR right after a stage runs. Every block ends with exactly one terminator. Every operand refers to a temporary, a block, a function, a global or a symbolic value that exists. The operand types agree with the instruction.

```c
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
```

The function `check_definitions` checks that every path from the entry to a use of a temporary passes a definition of it. Chapter 10 relies on that property and describes the check. The function `check_inst` also checks the types that instructions name. A `slot` or `memcopy` whose type has no entry in the table reports `names a type that does not exist`. A `bitload` or `bitstore` of a field without a width reports `names a field that is not a bitfield`.

Each violation becomes one line of text with the function and the block. The unit test `verifier` builds a function with three mistakes: an addition of `i64` and `i32`, a block without a terminator and a return of the wrong type.

```text
main.f b0: add i64 has an operand of type i32
main.f b0: the block does not end with a terminator
main.f b1: ret i32 in a function that returns i64
```

## Tests

The unit tests in `tests/unit/test_ir.c` build modules with the builder. Four of them print their expected text and pass the verifier: the function `scale` of chapter 1 after constant folding, the loop, the `str` example and the symbolic values. The test `verifier` fails with the three messages above. The ctest test `unit` runs them with the unit tests of the other chapters, and it passes.

## Next

[Chapter 8, Lowering the syntax tree to IR]({{% relref "/programming/writing-a-compiler/08-lowering-to-ir" %}}), turns expressions, conditions, both loop forms, calls and returns into blocks and instructions. It decides where local variables live before register allocation, and it marks the variables whose address the program takes.
