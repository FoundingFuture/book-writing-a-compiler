---
title: "Structs and arrays"
description: "How antic lays out structs, unions, bitfields and arrays for each target, lowers field access and copies, and passes aggregates by value in calls."
summary: "Layout computed by the back end for its target: structs, unions, bitfields under the System V and MSVC rules, packed and aligned structs. Field access, value semantics, copying, fixed-size arrays and indexing. Struct and union passing by value under all three calling conventions. A small raylib binding and the ABI probe as the tests that the rules are right."
date: 2026-09-13T22:32:00+02:00
lastmod: 2026-09-15T03:57:27+02:00
draft: false
weight: 180
tags: [compilers, assembly]
keywords: [struct layout, bitfield layout, packed structs, aggregate classification, homogeneous floating-point aggregate, value semantics, ABI probe, raylib binding]
---

## Previously

[Chapter 17, Floating point]({{% relref "/programming/writing-a-compiler/17-floating-point" %}}), adds `f32` and `f64` as a second register class. It selects SSE and ARM64 float instructions, converts between integers and floats, compares with NaN semantics and passes float arguments under all three calling conventions. The register allocator chooses each register from the list of its class.

## Aggregates in memory

Chapter 8 stopped at struct and array values with a message that named this chapter. A struct, a union or a fixed-size array is an aggregate. [Chapter 2]({{% relref "/programming/writing-a-compiler/02-the-anti-language" %}}) declares a union like a struct and gives a bitfield its width after the field type. The word `packed` before `struct` or `union` removes padding, and `align(N)` after the type name raises the alignment. [Chapter 6]({{% relref "/programming/writing-a-compiler/06-semantic-analysis" %}}) checks these declarations and computes no layout.

The IR of [chapter 7]({{% relref "/programming/writing-a-compiler/07-intermediate-representation" %}}) keeps an aggregate in memory and handles it through a `ptr`. Lowering therefore turns every aggregate expression into the address of its value. A local gets a slot, a parameter is the pointer that the back end supplies, and a literal gets a slot of its own. A constant is read-only data of the module, which a later section covers. A slot names the type of its value, and the back end gives it a size.

```c
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
```

The operation `memcopy` copies a value of a named type from one address to another. It is the only operation that moves a whole aggregate. A field or an element is read with `load` and written with `store`, one scalar at a time. A bitfield is read with `bitload` and written with `bitstore`.

## Aggregate types in the IR

The IR holds no size and no offset. Each module has a table of aggregate types, and lowering enters a type the first time it needs the type. The function `agg_of` finds the entry by the qualified name of the type or writes a new one. A struct or a union lists its fields with their bit widths, the `packed` flag and the N of `align(N)`. An array lists its element and its length as a symbolic value, together with the length as the source wrote it. A symbolic value is an integer that the back end computes for its target. It is a number, `size_of T`, `offset_of T.f` or an operation on symbolic values. The entries for `str` and slices belong to chapter 19.

```c
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
```

A value in memory has a `struct ir_vtype`: a scalar type, or `IR_AGG` with an index into the table. The function `vtype_of` computes it. The function `size_operand` turns the size of a type into the symbolic operand `size_of T`. The optimizer of [chapter 10]({{% relref "/programming/writing-a-compiler/10-optimizer" %}}) folds no symbolic value, so every size stays in the IR until the back end knows its target.

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

The listing of `--dump-ir` prints the table before the functions. The program `tests/abi/probe.anti` of the section on the ABI probe declares every kind of aggregate that this chapter lays out. The command `build/antic --dump-ir tests/abi/probe.anti | grep "^type"` prints its table.

```text
type str = struct { ptr: ptr, len: i64 }
type probe.Wide = struct { a: i8, b: clong, c: cwchar, d: i16 }
type probe.WideBox = struct { c: i8, t: probe.Wide }
type probe.Mixed = union { a: i8, b: i32, c: f64, d: i64 }
type probe.MixedBox = struct { c: i8, t: probe.Mixed }
type probe.Flags = struct { visible: i32 : 1 zeroext, layer: i32 : 4 zeroext, level: i8 : 3 signext, pad: i16 : 9 zeroext }
type probe.FlagsBox = struct { c: i8, t: probe.Flags }
type probe.Spread = struct { a: i8, b: i32 : 5 zeroext, c: i64 : 40 signext, d: i8 : 7 zeroext }
type probe.SpreadBox = struct { c: i8, t: probe.Spread }
type probe.Tight = packed struct { a: i8, b: i32, c: i16 }
type probe.TightBox = struct { c: i8, t: probe.Tight }
type probe.TightBits = packed struct { a: i8 : 3 zeroext, b: i32 : 7 zeroext }
type probe.TightBitsBox = struct { c: i8, t: probe.TightBits }
type probe.Aligned = struct align(16) { a: i8, b: i32 }
type probe.AlignedBox = struct { c: i8, t: probe.Aligned }
type probe.BitUnion = union { a: i32 : 3 zeroext, b: i8 : 7 zeroext }
type probe.BitUnionBox = struct { c: i8, t: probe.BitUnion }
```

The entry `str` comes from the string literals of the program. A bitfield prints its width and its extension on a read, `zeroext` for an unsigned type and `signext` for a signed one. The IR types `clong` and `cwchar` stand for `c_long` and `c_wchar`, whose widths depend on the target. Each `Box` struct puts a `u8` before its type, so its size minus the size of the type is the alignment of the type.

## Field access and indexing

A field lies at an offset after the address of its aggregate. The function `field_offset` gives the first field of a struct and every field of a union the constant offset 0. Every other field gets the symbolic value `offset_of T.f`, which names the aggregate and the field.

```c
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
```

The function `field_address` adds that offset with `ptradd`, and `offset_address` adds nothing for the offset 0. A base of pointer type is the address itself, so `p.x` on a `*T` needs no dereference of its own.

```c
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
```

An element of an array lies at the index times the element size. The function `element_address` multiplies the index by `size_of` of the element type and adds the product with `ptradd`. The function `first_element` gives the address of element 0. An array starts at its own address, and the value of a pointer is that address. The `str` and the slice of chapter 19 load their `ptr` field. No bounds check happens, as [chapter 2]({{% relref "/programming/writing-a-compiler/02-the-anti-language" %}}) specifies.

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

A literal stores its elements at offsets that `element_offset` computes. Element 0 lies at offset 0, and element `i` at `i` times the element size. Both functions give instructions such as `mul i64 2, size_of i32`, which stay in the IR until the back end folds them.

```c
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
```

## Arrays with symbolic lengths

An array length is a constant expression of type `int`. A length computed from `size_of` stays symbolic, and chapter 6 allows it for struct fields and local variables. The field `.len` of an array is its length, a number or a symbolic operand. Lowering still evaluates the base, so the call in `make().len` runs.

```c
        if (e->as.field.base->type->kind == TYPE_ARRAY) {
            /* .len is the only field of an array. */
            const struct type *array = e->as.field.base->type;
            lower_expr(l, e->as.field.base);
            return array->length_of != NULL
                       ? ir_sym_operand(l->m, sym_of(l, array->length_of))
                       : ir_int_op(IR_I64, array->length);
        }
```

The repeat form `[v; n]` computes `v` once and fills the elements in a loop. The loop compares the index with the length operand, which is symbolic for a count computed from `size_of`. An aggregate element is built at index 0 and copied to the other indices with `memcopy`.

```c
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
```

The test program `tests/programs/sizes.anti` sets the constant `HEADER` to `size_of(Header)`. It declares a local array of `HEADER` bytes and a struct field of `HEADER - 12` bytes.

```anti
extern fn printf(format: *byte, ...) -> i32;

struct Header
{
    tag: u8,
    count: i32,
    next: *Header,
}

const HEADER: int = size_of(Header);

struct Buffer
{
    head: Header,
    pad: [HEADER - 12]byte,
}

fn main() -> int
{
    let b: [HEADER]byte = [1; HEADER];
    let total = 0;
    let i = 0;
    while i < b.len do {
        total = total + b[i] as int;
        i = i + 1;
    }
    printf("%lld %lld %lld\n".ptr, HEADER, total, size_of(Buffer));
    return b.len - 16;
}
```

Its IR keeps every size symbolic. The loop bound is `size_of sizes.Header`, and the stride of each element is `size_of i8`. The type of the field `pad` records the length `sub i64(size_of sizes.Header, 12)`. The global `sizes.0` and the type `str` hold the format string of chapter 19.

```text
type sizes.Header = struct { tag: i8, count: i32, next: ptr }
type [size_of(sizes.Header)]byte = array size_of sizes.Header of i8
type str = struct { ptr: ptr, len: i64 }
type [size_of(sizes.Header) - 12]byte = array sub i64(size_of sizes.Header, 12) of i8
type sizes.Buffer = struct { head: sizes.Header, pad: [size_of(sizes.Header) - 12]byte }
extern fn printf(ptr, ...) -> i32
global sizes.0 size 16 align 1 bytes 25 6c 6c 64 20 25 6c 6c 64 20 25 6c 6c 64 0a 00
fn sizes.main() -> i64 {
b0:
    %0 = slot [size_of(sizes.Header)]byte
    %15 = slot str
    %1 = copy i64 0
    jump b1
b1:
    %2 = slt i8 %1, size_of sizes.Header
    branch %2, b2, b3
b2:
    %3 = mul i64 %1, size_of i8
    %4 = ptradd %0, %3
    store i8 1, %4
    %5 = add i64 %1, 1
    %1 = copy i64 %5
    jump b1
b3:
    %6 = copy i64 0
    %7 = copy i64 0
    jump b4
b4:
    %8 = slt i8 %7, size_of sizes.Header
    branch %8, b5, b6
b5:
    %9 = mul i64 %7, size_of i8
    %10 = ptradd %0, %9
    %11 = load i8 %10
    %12 = zext i64 %11
    %13 = add i64 %6, %12
    %6 = copy i64 %13
    %14 = add i64 %7, 1
    %7 = copy i64 %14
    jump b4
b6:
    %16 = addr @sizes.0
    store ptr %16, %15
    %17 = ptradd %15, offset_of str.len
    store i64 15, %17
    %18 = load ptr %15
    %19 = call i32 @printf(%18, size_of sizes.Header, %6, size_of sizes.Buffer)
    %20 = sub i64 size_of sizes.Header, 16
    ret i64 %20
}
```

The program prints the header size, the sum of the bytes and the size of `Buffer`, and exits with 0 on macos-arm64.

```text
exit 0
16 16 24
```

On linux-x86_64 the back end folds `size_of sizes.Header` to 16 and `size_of sizes.Buffer` to 24. The optimizer then runs again on the function. The stride of 1 byte becomes the scale of the memory operand `(%rax,%rdx,1)`, and `b.len - 16` becomes `movq $0, %rax`.

```text
sizes.main:
b0:
    pushq %rbp
    movq %rsp, %rbp
    subq $32, %rsp
    leaq (%rsp), %rax
    leaq 16(%rsp), %rcx
    movq $0, %rdx
b1:
    cmpq $16, %rdx
    jge b3
b2:
    movb $1, (%rax,%rdx,1)
    addq $1, %rdx
    jmp b1
b3:
    movq $0, %rdx
    movq $0, %rsi
b4:
    cmpq $16, %rsi
    jge b6
b5:
    movb (%rax,%rsi,1), %dil
    movzbq %dil, %rdi
    addq %rdi, %rdx
    addq $1, %rsi
    jmp b4
b6:
    leaq sizes.0(%rip), %rax
    movq %rax, (%rcx)
    movq $15, 8(%rcx)
    movq (%rcx), %rdi
    movq $16, %rsi
    movq $24, %rcx
    movl $0, %eax
    call printf
    movq $0, %rax
    movq %rbp, %rsp
    popq %rbp
    ret
```

A folded length below 1 is an error. The message names the length as the source wrote it and the target. The test `error_array_length` compiles `tests/errors/zero_length.anti`, whose struct `Pair` has 8 bytes on every target.

```anti
struct Pair
{
    a: i32,
    b: i32,
}

fn main() -> int
{
    let x: [size_of(Pair) - 8]byte = [0; size_of(Pair) - 8];
    return x.len;
}
```

```text
antic: the array length `size_of(Pair) - 8` is 0 on linux-x86_64, and an array length is at least 1
```

## Literals and copies

A literal writes its fields or elements directly into the destination. A bitfield of a struct literal is written with `bitstore`. Any other aggregate value already lies at some address, and `build_into` copies it with a `memcopy` of its type. The cases for string literals, byte strings, slice literals and slices belong to chapter 19.

```c
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
```

A `let` builds the value into the slot of its variable. An assignment copies, and a function returns the address of its result with `ret ptr`. The statement `g.cells[1] = g.cells[2]` copies a row of four `i16` elements. After `let row = g.cells[1]`, the variable `row` is a copy that later writes to `g` do not change.

```c
    if (is_aggregate(target->type)) {
        v = lower_address(l, s->as.assign.value);
        if (!l->failed) {
            ir_memcopy(l->f, l->b, p.address, v, vtype_of(l, target->type));
        }
        return;
    }
```

An assignment builds a literal in a slot of its own before the copy. The assignment `a = [a[1], a[0], a[2]]` therefore reads all three elements before it writes any. The back ends copy up to 64 bytes with moves of 8, 4, 2 and 1 bytes. Above that size they call the C function `memcpy`. The size comes from the layout of the type that `memcopy` names.

```c
/* DESIGN: a copy of up to 64 bytes moves 8, 4, 2 and 1 bytes at a time
   through an integer register. A larger copy calls memcpy. */
static void copy_memory(struct selector *s, struct mach_operand dst,
                        struct mach_operand src, uint64_t size)
{
    uint64_t offset = 0;

    if (size > 64) {
        struct mach_operand args[3];
        args[0] = dst;
        args[1] = src;
        args[2] = select_new_vreg(s, 64);
        load(s, args[2], size);
        call_c(s, "memcpy", args, 3);
        return;
    }
    while (offset < size) {
        uint64_t left = size - offset;
        uint8_t bits = left >= 8 ? 64 : left >= 4 ? 32 : left >= 2 ? 16 : 8;
        struct mach_operand t = select_new_vreg(s, bits);
        emit2(s, X64_MOV, t, memory_at(src, (int64_t)offset, bits));
        emit2(s, X64_MOV, memory_at(dst, (int64_t)offset, bits), t);
        offset += bits / 8;
    }
}
```

The pattern of `memcopy` takes the size of the copied type from the layout of the target.

```c
static void emit_memcopy(struct selector *s, const struct ir_inst *inst)
{
    copy_memory(s, select_reg(s, &inst->a), select_reg(s, &inst->b),
                select_size(s, inst->of));
}
```

The ARM64 version has the same shape with `ldr` and `str`.

## Constants as read-only data

An aggregate constant is data of the module, and no instruction builds it. The function `lower_address` gives a use the address of that data, and the use copies from it with one `memcopy`.

Lowering cannot write the bytes. The IR holds no size, so the offset of a field, its padding and the bits of a bitfield belong to the back end. The IR carries the value as a typed tree instead. The back end writes the bytes once it has laid the type out.

```c
enum ir_const_kind {
    IR_CONST_NONE,      /* no value, such as the break of a bitfield unit */
    IR_CONST_INT,       /* an integer, a bitfield among them */
    IR_CONST_FLOAT,
    IR_CONST_SYM,       /* a symbolic value, such as size_of T */
    IR_CONST_ADDR,      /* the address of another global */
    IR_CONST_AGG        /* a struct, a union or an array */
};

/* DESIGN: an aggregate constant reaches the back end as a typed tree and
   not as bytes. The IR holds no sizes. A field's offset, its padding and
   the bits of a bitfield belong to the back end, which lays the type out
   for its target. */
struct ir_const {
    enum ir_const_kind kind;
    struct ir_vtype type;           /* IR_CONST_AGG: the aggregate */
    enum ir_type scalar;            /* the value types */
    uint64_t integer;               /* IR_CONST_INT */
    double floating;                /* IR_CONST_FLOAT */
    uint32_t sym;                   /* IR_CONST_SYM */
    uint32_t global;                /* IR_CONST_ADDR */
    struct ir_const *items;         /* IR_CONST_AGG, one per field */
    size_t item_count;
};
```

The tree keeps one item per field, in the order of the fields. A break of a bitfield unit takes its place among them and carries no value, so an item and a field share an index. The constant `BREAKS` of the ABI probe below has three values in five places, and the dump prints a break as a dash.

```text
global probe.41 probe.Breaks { i8 1, -, i8 2, -, i8 3 }
```

The function `const_tree` builds the tree from the value that chapter 6 computed. A field of a bitfield becomes an integer item, because the aggregate type already records the width.

```c
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
```

The function `const_address` puts the tree in a global of the module and returns its address. It names the global by its index there, as chapter 19 names the global of a string literal, and two constants of one value share the data. A constant that no function uses reaches no global, because lowering builds one at a use.

The back end writes the bytes in `layout_data`, after `layouts_init` and before it selects any instruction.

```c
/* DESIGN: the bytes start as zeros and each field writes its own, so the
   padding of a constant is zero. The data of a constant is then the same
   in every build, and two constants of one value hold equal bytes. */
bool layout_data(struct layouts *l, struct ir_module *m)
{
    size_t i;

    for (i = 0; i < m->global_count && !l->failed; i++) {
        struct ir_global *g = m->globals[i];
        if (g->value == NULL) {
            continue;
        }
        g->size = layout_size(l, const_vtype(g->value));
        g->align = layout_align(l, const_vtype(g->value));
        g->bytes = arena_alloc(m->arena, g->size == 0 ? 1 : g->size);
        write_const(l, m, g, g->value, 0);
    }
    return !l->failed;
}
```

The bytes start as zeros and each field writes its own, so the padding of a constant is zero. A constant therefore holds the same bytes in every build and on every host, and two constants of one value hold equal bytes.

## Bitfield places

A place is where an assignment writes. A bitfield has no address. Its place records the address of the aggregate, the index of the aggregate in the type table and the index of the field. The function `bitfield_of` recognises a field access that names a bitfield, through a struct value or through a pointer.

```c
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
```

The function `read_place` reads a bitfield with `bitload`. The function `lower_assign` writes it with `bitstore`, and a compound assignment such as `f.layer += 3` reads the field before it writes.

```c
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
```

The test program `tests/programs/bitfields.anti` stores four bitfields of different types and reads them back. The fields end with the values 1, 12, -4 and 300, and the exit code is `1 + 12 * 10 + -4 * 100 + 300`, which is 21.

```anti
struct Flags
{
    visible: u32 : 1,
    layer: u32 : 4,
    level: i8 : 3,
    pad: u16 : 9,
}

fn main() -> int
{
    let f = Flags { visible: 1, layer: 9, level: -3, pad: 300 };
    f.layer += 3;
    f.level = f.level - 1;
    return f.visible as int + f.layer as int * 10 + f.level as int * 100 +
        f.pad as int;
}
```

```text
exit 21
```

Lowering names each bitfield by its aggregate and its field. The position of the bits and the integer that holds them depend on the target, so the IR contains neither.

```text
type bitfields.Flags = struct { visible: i32 : 1 zeroext, layer: i32 : 4 zeroext, level: i8 : 3 signext, pad: i16 : 9 zeroext }
fn bitfields.main() -> i64 {
b0:
    %0 = slot bitfields.Flags
    bitstore i32 1, %0, bitfields.Flags.visible
    bitstore i32 9, %0, bitfields.Flags.layer
    bitstore i8 -3, %0, bitfields.Flags.level
    bitstore i16 300, %0, bitfields.Flags.pad
    %1 = bitload i32 %0, bitfields.Flags.layer
    %2 = add i32 %1, 3
    bitstore i32 %2, %0, bitfields.Flags.layer
    %3 = bitload i8 %0, bitfields.Flags.level
    %4 = sub i8 %3, 1
    bitstore i8 %4, %0, bitfields.Flags.level
    %5 = bitload i32 %0, bitfields.Flags.visible
    %6 = zext i64 %5
    %7 = bitload i32 %0, bitfields.Flags.layer
    %8 = zext i64 %7
    %9 = mul i64 %8, 10
    %10 = add i64 %6, %9
    %11 = bitload i8 %0, bitfields.Flags.level
    %12 = sext i64 %11
    %13 = mul i64 %12, 100
    %14 = add i64 %10, %13
    %15 = bitload i16 %0, bitfields.Flags.pad
    %16 = zext i64 %15
    %17 = add i64 %14, %16
    ret i64 %17
}
```

## Value semantics

An aggregate parameter is a copy of the argument, so a function that writes to its parameter leaves the caller's value unchanged. The test program `tests/programs/structs.anti` checks copies of nested structs, of arrays inside structs and of rows of a two-dimensional array.

```anti
extern fn printf(format: *byte, ...) -> i32;

struct V2
{
    x: f32,
    y: f32,
}

struct Rect
{
    corner: V2,
    size: V2,
}

struct Big
{
    id: i32,
    tags: [3]u8,
    weights: [4]f64,
}

struct Grid
{
    cells: [3][4]i16,
}

fn area(r: *Rect) -> f32
{
    return r.size.x * r.size.y;
}

fn grow(r: Rect, by: f32) -> Rect
{
    r.size = V2 { x: r.size.x + by, y: r.size.y + by };
    return r;
}

fn heavier(b: Big, extra: f64) -> Big
{
    let i = 0;
    while i < b.weights.len do {
        b.weights[i] = b.weights[i] + extra;
        i = i + 1;
    }
    b.tags[1] = b.tags[1] + 1;
    return b;
}

fn sum(a: [5]int) -> int
{
    let total = 0;
    let i = 0;
    while i < a.len do {
        total = total + a[i];
        i = i + 1;
    }
    return total;
}

fn line(v: int)
{
    let format = [37 as u8, 108, 100, 10, 0];
    printf(&format[0], v);
}

fn main() -> int
{
    let r = Rect { corner: V2 { x: 1.0, y: 2.0 }, size: V2 { x: 3.0, y: 4.0 } };
    let copy = r;
    copy.size.x = 10.0;
    let big = grow(r, 1.0);
    line(area(&r) as int);
    line(area(&big) as int);
    line(copy.size.x as int);
    line(r.size.x as int);

    let b = Big { id: 7, tags: [1, 2, 3], weights: [0.5; 4] };
    let h = heavier(b, 2.0);
    line(h.tags[1] as int);
    line(b.tags[1] as int);
    line((h.weights[3] * 10.0) as int);
    line((b.weights[3] * 10.0) as int);

    let g = Grid { cells: [[0; 4]; 3] };
    g.cells[2][3] = 9;
    g.cells[1] = g.cells[2];
    let row = g.cells[1];
    row[0] = 5;
    line(g.cells[1][3] as int);
    line(row[0] as int);
    line(g.cells[1][0] as int);
    line(g.cells.len * g.cells[0].len);

    line(sum([1, 2, 3, 4, 5]));
    return 0;
}
```

The program prints 13 values and exits with 0. The fourth line shows that `grow` did not change `r`, and the sixth and eighth lines show the same for `heavier` and `b`. The eleventh line, 0, shows that `row[0] = 5` did not change `g`.

```text
exit 0
12
20
10
3
3
2
25
5
9
5
0
12
15
```

## Layout for a target

The back end lays out every aggregate of the module before selection. The function `select_module` of [chapter 12]({{% relref "/programming/writing-a-compiler/12-instruction-selection" %}}) calls `layouts_init` for its target. It then calls `layout_resolve` on each function and `ir_optimize_function` on each function that `layout_resolve` changed. The file `src/layout.h` declares the result for one aggregate.

```c
/* One scalar inside an aggregate, for the classification of its ABI. */
struct layout_member {
    uint64_t offset;
    enum ir_type type;
};

/* Where a bitfield lies, and the integer that a load or store of it reads
   and writes. */
struct layout_bits {
    uint64_t pos;                   /* the first bit, from bit 0 of byte 0 */
    uint64_t unit_offset;           /* the byte the integer starts at */
    enum ir_type unit_type;
    uint8_t shift;                  /* the first bit inside the integer */
};

/* The layout of an aggregate on the target. */
struct layout {
    uint64_t size;
    uint64_t align;
    uint64_t *offsets;              /* the offset of each field */
    struct layout_bits *bits;       /* for each field, set for bitfields */
    struct layout_member *members;  /* recorded up to LAYOUT_MEMBER_LIMIT */
    size_t member_count;
    bool unaligned;                 /* a field lies off its alignment */
};

/* DESIGN: the members of an aggregate are recorded up to 32 bytes. Every
   convention passes a larger aggregate in memory, and a float aggregate
   of AAPCS64 has at most four members of 8 bytes. */
enum { LAYOUT_MEMBER_LIMIT = 32 };
```

Each scalar type has one size on all six targets, and its alignment equals its size. The C types `c_long` and `c_wchar` are the exception: `c_long` has 4 bytes on Windows and 8 bytes elsewhere, and `c_wchar` has 2 bytes and 4 bytes. The function `target_type` gives both types their fixed IR type.

```c
/* The fixed-width IR type of c_long or c_wchar on the target, or type. */
static enum ir_type target_type(const struct layouts *l, enum ir_type type)
{
    bool windows = target_info(l->target)->os == OS_WINDOWS;

    if (type == IR_CLONG) {
        return windows ? IR_I32 : IR_I64;
    }
    if (type == IR_CWCHAR) {
        return windows ? IR_I16 : IR_I32;
    }
    return type;
}

/* Every scalar is as large as it is aligned, on all six targets. */
static uint64_t scalar_size(const struct layouts *l, enum ir_type type)
{
    switch (target_type(l, type)) {
    case IR_I8: return 1;
    case IR_I16: return 2;
    case IR_I32:
    case IR_F32: return 4;
    default: return 8;
    }
}
```

### Structs, unions and arrays

The function `compute` applies the C rules. A struct places each field at the next multiple of its alignment and takes the largest alignment of its fields, as System V states[^1]. A union places every field at offset 0 and takes the size of its largest field, rounded up to its alignment[^3]. An array has the alignment of its element and the size of the element times the length[^3]. A nested aggregate gets its layout from `layout_agg`, which computes it on first use.

```c
/* The C rules of chapter 18. A struct places each field at the next
   multiple of its alignment, and a union places every field at 0. packed
   aligns every field to 1, and align(N) raises the aggregate's alignment.
   The size rounds up to the alignment.

   DESIGN: bitfields follow the rule of the target's C compiler. System V
   on Linux and macOS puts a bitfield at the next free bit, unless it would
   cross a boundary of its type's alignment. MSVC on Windows gives
   consecutive bitfields of one size a shared unit of that size. It opens
   a new unit for another size or when the bits run out. */
static void compute(struct layouts *l, uint32_t agg)
{
    const struct ir_aggtype *t = l->m->aggs[agg];
    struct layout *out = &l->aggs[agg];
    bool msvc = target_info(l->target)->os == OS_WINDOWS;
    bool aapcs64 = target_info(l->target)->os == OS_LINUX &&
                   target_info(l->target)->arch == ARCH_ARM64;
    uint64_t bit = 0;
    uint64_t align = 1;
    uint64_t unit_start = 0;
    uint64_t unit_size = 0;
    uint64_t unit_used = 0;
    uint64_t length;
    struct members members;
    size_t i;

    if (l->agg_state[agg] == BUSY) {
        fail(l, "the layout of `%s` depends on itself", t->name);
        out->size = out->align = 1;
        return;
    }
    l->agg_state[agg] = BUSY;
    out->offsets = allocate(t->field_count, sizeof *out->offsets);
    out->bits = allocate(t->field_count, sizeof *out->bits);
    if (t->kind == IR_AGG_ARRAY) {
        struct ir_vtype element = t->fields[0].type;
        if (!layout_fold(l, t->length, &length)) {
            length = 1;
        } else if ((int64_t)length < 1) {
            fail(l, "the array length `%s` is %" PRId64 " on %s, and an "
                    "array length is at least 1",
                 t->length_text, (int64_t)length, target_name(l->target));
            length = 1;
        }
        bit = length * layout_size(l, element) * 8;
        align = layout_align(l, element);
        out->unaligned = element.type == IR_AGG &&
                         layout_agg(l, element.agg)->unaligned;
    }
    for (i = 0; t->kind != IR_AGG_ARRAY && i < t->field_count; i++) {
        struct ir_vtype type = t->fields[i].type;
        uint64_t width = t->fields[i].bits;
        uint64_t size = layout_size(l, type);
        uint64_t natural = layout_align(l, type);
        uint64_t field_align = t->packed ? 1 : natural;
        uint64_t end = 0;
        if (is_unit_break(&t->fields[i])) {
            /* DESIGN: a zero-width bitfield breaks the unit as C does.
               System V aligns the next field to T, AAPCS64 outside Apple
               also the struct, and MSVC acts only after a bitfield. */
            if (!msvc) {
                bit = round_up(bit, natural * 8);
                align = aapcs64 && natural > align ? natural : align;
            } else if (unit_size != 0) {
                bit = round_up((bit + 7) / 8, field_align) * 8;
                align = field_align > align ? field_align : align;
                unit_size = 0;
            }
            out->offsets[i] = bit / 8;
            continue;
        }
        if (t->kind == IR_AGG_UNION) {
            out->offsets[i] = 0;
            end = width == 0 || msvc ? size * 8 : width;
        } else if (width == 0) {
            unit_size = 0;
            out->offsets[i] = round_up((bit + 7) / 8, field_align);
            bit = (out->offsets[i] + size) * 8;
        } else if (msvc && unit_size == size && unit_used + width <= size * 8) {
            out->bits[i].pos = unit_start + unit_used;
            unit_used += width;
        } else if (msvc) {
            unit_start = round_up((bit + 7) / 8, field_align) * 8;
            unit_size = size;
            unit_used = width;
            out->bits[i].pos = unit_start;
            bit = unit_start + size * 8;
        } else {
            if (!t->packed && bit % (natural * 8) + width > size * 8) {
                bit = round_up(bit, natural * 8);
            }
            out->bits[i].pos = bit;
            bit += width;
        }
        if (width != 0 && t->kind != IR_AGG_UNION) {
            out->offsets[i] = out->bits[i].pos / 8;
        }
        bit = end > bit ? end : bit;
        align = field_align > align ? field_align : align;
        if ((width == 0 && out->offsets[i] % natural != 0) ||
            (type.type == IR_AGG && layout_agg(l, type.agg)->unaligned)) {
            out->unaligned = true;
        }
    }
    /* DESIGN: align(N) raises the alignment, as C's _Alignas does, and a
       value below the alignment the fields give is an error, as in C. */
    if (t->align != 0 && t->align < align) {
        fail(l, "`%s` has align(%" PRIu64 "), below its alignment %" PRIu64
                " on %s", t->name, t->align, align, target_name(l->target));
    } else if (t->align > align) {
        align = t->align;
    }
    out->align = align;
    out->size = round_up((bit + 7) / 8, align);
    for (i = 0; t->kind != IR_AGG_ARRAY && i < t->field_count; i++) {
        if (t->fields[i].bits != 0 && !bit_unit(l, t, out, i)) {
            break;
        }
    }
    l->agg_state[agg] = DONE;
    if (out->size <= LAYOUT_MEMBER_LIMIT) {
        members.count = 0;
        flatten(l, ir_aggregate(agg), 0, &members);
        out->members = allocate(members.count, sizeof *out->members);
        memcpy(out->members, members.items,
               members.count * sizeof *out->members);
        out->member_count = members.count;
    }
}
```

The unit test `structs` in `tests/unit/test_layout.c` lays out the fields `i8`, `i32` and `i16` on linux-x86_64. They lie at offsets 0, 4 and 8, and the struct has 12 bytes with alignment 4. An array of three such structs has 36 bytes.

### Packed and aligned structs

The flag `packed` aligns every field to 1, so no padding lies between the fields, and the struct has alignment 1. A field that lies off its natural alignment marks the layout `unaligned`. An aggregate with such a field inside a field of its own is unaligned as well. The modifier `align(N)` raises the alignment, and the size rounds up to it. An N below the alignment that the fields give is an error that names the type and the target.

The unit test `modifiers` lays out the fields `i8`, `f64` and `i32` on windows-arm64. As a union they take 8 bytes with alignment 8. As a packed struct they take 13 bytes at offsets 0, 1 and 9, with alignment 1. The first field alone with `align(16)` takes 16 bytes with alignment 16. The test `unaligned` expects this message for a struct with `align(4)` and a `c_long` field on linux-arm64.

```text
`main.N` has align(4), below its alignment 8 on linux-arm64
```

### Bitfields under System V and MSVC

A bitfield takes its bits from a storage unit of its declared type, and the placement differs between the C compilers of the targets. System V allocates bit-fields from right to left and requires each one to lie in a storage unit of its type[^1]. The function `compute` puts a bitfield at the next free bit. When the field would cross a boundary of its type's alignment, it starts at that boundary, except in a packed struct. For Windows, `compute` follows the MSVC rule, which gives consecutive bitfields of one size a shared unit of that size. A field of another size opens a new unit at its alignment. So does a field that finds no room in the current unit.

The unit test `bitfields` lays out three structs under both rules, on linux-x86_64 and on windows-arm64. The struct `S1` holds `a: i8 : 3`, `b: i32 : 7` and `c: i16 : 9`. The struct `S3` is a packed struct of the fields `a` and `b`. The struct `S4` holds `a: i8`, `b: i32 : 5`, `c: i64 : 40` and `d: i8 : 7`. The table lists the first bit of each bitfield, the size in bytes and the alignment.

| Struct | Rule | First bits | Size | Alignment |
|---|---|---|---|---|
| `S1` | System V | 0, 3, 16 | 4 | 4 |
| `S1` | MSVC | 0, 32, 64 | 12 | 4 |
| `S3` | System V | 0, 3 | 2 | 1 |
| `S3` | MSVC | 0, 8 | 5 | 1 |
| `S4` | System V | 8, 13, 56 | 8 | 8 |
| `S4` | MSVC | 32, 64, 128 | 24 | 8 |

Apple clang 21.0.0 gives the same sizes, alignments and bit positions for the same structs in C. The check compiled them with `-target x86_64-unknown-linux-gnu` and with `-target aarch64-pc-windows-msvc`, and `S3` with `#pragma pack(push, 1)`. It read the bit positions from the LLVM IR of globals that set one bitfield to 1.

In `S1` under System V, the field `c` would cover bits 10 to 18. That range crosses the 16-bit boundary of `i16`, so `c` starts at bit 16. Under MSVC, `b` has a size other than that of `a` and opens a 4-byte unit at byte 4. The field `c` opens a 2-byte unit at byte 8.

Every bitfield of a union starts at bit 0. Under System V it adds its width to the size of the union, and under MSVC the size of its type. After the size is known, `bit_unit` chooses the integer that a load or a store of each bitfield reads. It is the smallest integer that holds the bits of the field from the byte of its first bit. When that integer would end past the aggregate, it starts earlier. A field that needs more than 8 bytes is an error, and only a packed struct can hold one.

```c
/* The integer a load or store of a bitfield reads. It is the smallest
   integer that holds the field's bits. It starts at the byte of the first
   bit, or earlier when it would end past the aggregate. */
static bool bit_unit(struct layouts *l, const struct ir_aggtype *t,
                     const struct layout *out, size_t i)
{
    struct layout_bits *b = &out->bits[i];
    uint64_t width = t->fields[i].bits;
    uint64_t need = (b->pos % 8 + width + 7) / 8;
    uint64_t bytes = need <= 1 ? 1 : need <= 2 ? 2 : need <= 4 ? 4 : 8;

    if (need > 8) {
        fail(l, "the bitfield `%s` of `%s` spans more than 8 bytes on %s",
             t->fields[i].name, t->name, target_name(l->target));
        return false;
    }
    b->unit_offset = b->pos / 8;
    if (b->unit_offset + bytes > out->size) {
        b->unit_offset = out->size - bytes;
    }
    b->unit_type = bytes == 1 ? IR_I8 : bytes == 2 ? IR_I16
                   : bytes == 4 ? IR_I32 : IR_I64;
    b->shift = (uint8_t)(b->pos - b->unit_offset * 8);
    return true;
}
```

In `S4` under System V, the field `c` covers bits 13 to 52, which need 6 bytes from byte 1. An `i64` from byte 1 would end at byte 9 of the 8-byte struct, so its unit starts at byte 0 with shift 13. The field `d` reads one `i8` at byte 7 with shift 0.

### Zero-width bitfields

A zero-width bitfield `_: T : 0` holds no value and breaks the unit. The function `compute` handles it before any other field and records no bits and no members for it. Clang 21.0.0 follows three rules. The unit test `zero_width` checks them on one target each and on macos-arm64.

| Rule | Next field | Struct alignment |
|---|---|---|
| System V on Linux x86_64 and macOS | at a multiple of T's alignment, also when packed | unchanged |
| AAPCS64 on Linux ARM64 | at a multiple of T's alignment, also when packed | raised to T's alignment |
| MSVC | after a bitfield: at a multiple of T's alignment, limited by `packed` | after a bitfield: raised to that alignment |

MSVC ignores a zero-width bitfield that follows a field without a width or opens the struct. For the struct `S3` with `a: i8 : 3`, `_: i64 : 0` and `b: i8 : 3`, the field `b` starts at bit 64 under every rule. The struct has 9 bytes with alignment 1 under System V and 16 bytes with alignment 8 under AAPCS64 and MSVC. The facts come from C structs of the same fields that clang compiled for each triple. A global union of such a struct and a byte array with one field set to 1 shows the bit positions in the object file.

### Members for classification

A calling convention decides where an aggregate goes from its size, its alignment and the types of its scalars. The function `compute` records the scalars of an aggregate of up to 32 bytes as its members, through every level of nested aggregates. The function `flatten` gives every field of a union the offset of the union. Two equal scalars at one offset count as one member.

```c
/* The scalars of a value of type v at offset. Every field of a union
   starts at the union's own offset. Two equal scalars at one offset are
   one member. */
static void flatten(struct layouts *l, struct ir_vtype v, uint64_t offset,
                    struct members *out)
{
    const struct ir_aggtype *t;
    const struct layout *layout;
    uint64_t step;
    uint64_t i;

    if (v.type != IR_AGG) {
        for (i = 0; i < out->count; i++) {
            if (out->items[i].offset == offset &&
                out->items[i].type == target_type(l, v.type)) {
                return;
            }
        }
        if (out->count < LAYOUT_MEMBER_LIMIT) {
            out->items[out->count].offset = offset;
            out->items[out->count].type = target_type(l, v.type);
            out->count++;
        }
        return;
    }
    t = l->m->aggs[v.agg];
    layout = layout_agg(l, v.agg);
    if (t->kind == IR_AGG_ARRAY) {
        step = layout_size(l, t->fields[0].type);
        for (i = 0; step > 0 && i < layout->size / step; i++) {
            flatten(l, t->fields[0].type, offset + i * step, out);
        }
        return;
    }
    for (i = 0; i < t->field_count; i++) {
        if (!is_unit_break(&t->fields[i])) {
            flatten(l, t->fields[i].type, offset + layout->offsets[i], out);
        }
    }
}
```

AAPCS64 counts the members of a homogeneous aggregate as uniquely addressable members[^3], so two `f32` fields of a union at offset 0 form one member. The unit test `union_members` expects one member for a union of two `f32` fields. It expects two members for a union of an `f32` and an `i32`.

### Folding symbolic values

The function `layout_fold` computes the bits of a symbolic value on the target from the layouts and keeps the result. A `size_of` or an `offset_of` reads the layout, and an operation folds its operands first. An operation wraps at the width of its type, and a division by zero is an error that names the target.

```c
bool layout_fold(struct layouts *l, uint32_t sym, uint64_t *out)
{
    const struct ir_sym *s = &l->m->syms[sym];
    uint64_t a = 0;
    uint64_t b = 0;
    bool ok = true;

    if (l->sym_state[sym] == DONE) {
        *out = l->values[sym];
        return true;
    }
    switch (s->kind) {
    case IR_SYM_INT:
        *out = s->value;
        break;
    case IR_SYM_SIZE_OF:
        *out = layout_size(l, s->of);
        break;
    case IR_SYM_OFFSET_OF:
        *out = layout_agg(l, s->of.agg)->offsets[s->field];
        break;
    case IR_SYM_OP:
        ok = layout_fold(l, s->a, &a) &&
             (s->b == IR_NO_AGG || layout_fold(l, s->b, &b));
        /* The operands of a comparison or conversion have their own type,
           and the value of the operation has the result type. */
        ok = ok && fold_op(l, s, target_type(l, l->m->syms[s->a].type), a, b,
                           out);
        break;
    }
    if (ok && !l->failed) {
        l->values[sym] = *out;
        l->sym_state[sym] = DONE;
    }
    return ok && !l->failed;
}
```

The unit test `structs` folds `size_of main.S` times `offset_of main.S.c` for the struct of `i8`, `i32` and `i16`. On linux-x86_64 the struct has 12 bytes and `c` lies at offset 8, so the product is 96.

The function `layout_resolve` replaces every symbolic operand of a function with a constant. It gives `clong` and `cwchar` their widths on the target, after `wchar_signedness` of chapter 11 picks the signed operations on `cwchar` where `wchar_t` is signed. A conversion between two types of one width becomes a `copy`. A block that holds a bitfield load or store goes through `lower_block_bits`. The flag `resolved` tells `select_module` to run the optimizer on the function again.

```c
bool layout_resolve(struct layouts *l, struct ir_function *f, bool *resolved)
{
    bool has_bits = false;
    size_t b;
    size_t i;
    size_t k;

    resolve_type(l, &f->result, resolved);
    for (i = 0; i < f->param_count; i++) {
        resolve_type(l, &f->params[i].type, resolved);
    }
    for (i = 0; i < f->temp_count; i++) {
        resolve_type(l, &f->temps[i], resolved);
    }
    for (b = 0; b < f->block_count; b++) {
        for (i = 0; i < f->blocks[b]->count; i++) {
            struct ir_inst *inst = &f->blocks[b]->insts[i];
            wchar_signedness(l, inst);
            resolve_type(l, &inst->type, resolved);
            if (!resolve(l, &inst->a, resolved) ||
                !resolve(l, &inst->b, resolved) ||
                !resolve(l, &inst->c, resolved)) {
                return false;
            }
            for (k = 0; k < inst->arg_count; k++) {
                if (!resolve(l, &inst->args[k], resolved)) {
                    return false;
                }
            }
            /* A conversion between c_long and a type of its width on
               this target changes nothing. */
            if (is_conversion(inst->op) && inst->a.type == inst->type) {
                inst->op = IR_COPY;
            }
            if (inst->op == IR_BITLOAD || inst->op == IR_BITSTORE) {
                has_bits = true;
            }
        }
        if (has_bits) {
            lower_block_bits(l, f, f->blocks[b]);
            *resolved = true;
            has_bits = false;
        }
    }
    return true;
}
```

### Bitfield loads and stores

The function `lower_bits` replaces `bitload` and `bitstore` with operations on the unit integer. A load shifts the field down and masks it. A signed field shifts to the top of the unit and back with an arithmetic shift. A store reads the unit, clears the bits of the field, puts in the new bits and writes the unit back.

```c
/* DESIGN: a bitfield load reads the integer that holds the field. It
   shifts the field down and masks it. A signed field shifts to the top and
   back with an arithmetic shift. A store reads the integer, clears the
   field's bits, puts the new bits in and writes the integer back. It is
   the sequence a C compiler emits. */
static void lower_bits(struct layouts *l, struct ir_function *f,
                       struct ir_block *out, const struct ir_inst *inst)
{
    const struct ir_field *field = &l->m->aggs[inst->of.agg]->fields[inst->field];
    const struct layout_bits *where =
        &layout_agg(l, inst->of.agg)->bits[inst->field];
    enum ir_type unit = where->unit_type;
    int unit_bits = bits(unit);
    uint64_t width = field->bits;
    const struct ir_operand *base = inst->op == IR_BITLOAD ? &inst->a
                                                          : &inst->b;
    struct ir_operand pointer = *base;
    struct ir_operand word;
    struct ir_operand v;

    if (where->unit_offset != 0) {
        pointer = temp_of(f, ir_ptradd(f, out, pointer,
                                       ir_int_op(IR_I64, where->unit_offset)));
    }
    word = temp_of(f, ir_load(f, out, unit, pointer));
    if (inst->op == IR_BITLOAD && field->ext == IR_EXT_SIGN) {
        int up = unit_bits - where->shift - (int)width;
        v = up == 0 ? word
                    : temp_of(f, ir_binary(f, out, IR_SHL, unit, word,
                                           ir_int_op(unit, (uint64_t)up)));
        if (width < (uint64_t)unit_bits) {
            v = temp_of(f, ir_binary(f, out, IR_SHR_S, unit, v,
                                     ir_int_op(unit, (uint64_t)unit_bits - width)));
        }
        v = convert(f, out, v, unit, inst->type, IR_EXT_SIGN);
        ir_assign(f, out, inst->result, v);
        return;
    }
    if (inst->op == IR_BITLOAD) {
        v = where->shift == 0
                ? word
                : temp_of(f, ir_binary(f, out, IR_SHR_U, unit, word,
                                       ir_int_op(unit, where->shift)));
        if (width < (uint64_t)unit_bits) {
            v = temp_of(f, ir_binary(f, out, IR_AND, unit, v,
                                     ir_int_op(unit, low_bits(width))));
        }
        v = convert(f, out, v, unit, inst->type, IR_EXT_ZERO);
        ir_assign(f, out, inst->result, v);
        return;
    }
    v = convert(f, out, inst->a, inst->type, unit, IR_EXT_ZERO);
    if (width < (uint64_t)unit_bits) {
        v = temp_of(f, ir_binary(f, out, IR_AND, unit, v,
                                 ir_int_op(unit, low_bits(width))));
    }
    if (where->shift != 0) {
        v = temp_of(f, ir_binary(f, out, IR_SHL, unit, v,
                                 ir_int_op(unit, where->shift)));
    }
    word = temp_of(f, ir_binary(f, out, IR_AND, unit, word,
                                ir_int_op(unit,
                                          ~(low_bits(width) << where->shift))));
    v = temp_of(f, ir_binary(f, out, IR_OR, unit, word, v));
    ir_store(f, out, unit, v, pointer);
}
```

On linux-x86_64, `f.layer += 3` and `f.level = f.level - 1` of `bitfields.anti` become the lines below. The field `layer` covers bits 1 to 4, so its unit is the `i8` at byte 0 with shift 1. The mask `-31` is the complement of `15 << 1` and clears bits 1 to 4. The field `level` covers bits 5 to 7 and reaches the top of its unit, so its load needs only the arithmetic shift `sarb $5`.

```text
    movb (%rax), %cl
    shrb $1, %cl
    andb $15, %cl
    movzbl %cl, %ecx
    addl $3, %ecx
    movb (%rax), %dl
    andb $15, %cl
    shlb $1, %cl
    andb $-31, %dl
    orb %cl, %dl
    movb %dl, (%rax)
    movb (%rax), %cl
    sarb $5, %cl
    subb $1, %cl
    movb (%rax), %dl
    andb $7, %cl
    shlb $5, %cl
    andb $31, %dl
    orb %cl, %dl
    movb %dl, (%rax)
```

The field `pad` has 9 bits of an `i16`. System V places it at byte 2. MSVC places it at byte 6, after the 4-byte unit of `visible` and `layer` and the 1-byte unit of `level`. The store of 300 differs only in that offset, first on linux-x86_64 and then on windows-x86_64.

```text
    movq %rax, %rcx
    addq $2, %rcx
    movw (%rcx), %dx
    andw $-512, %dx
    orw $300, %dx
    movw %dx, (%rcx)
```

```text
    movq %rax, %rcx
    addq $6, %rcx
    movw (%rcx), %dx
    andw $-512, %dx
    orw $300, %dx
    movw %dx, (%rcx)
```

## Structs in calls

The test program `tests/dump/structs.anti` exercises four kinds of struct. The struct `Pair` holds an integer and a float in 16 bytes. The struct `Big` has 24 bytes, and `Rgb` has 3 bytes. The struct `V2` holds two floats in 8 bytes.

```anti
struct Pair
{
    a: int,
    b: f64,
}

struct Big
{
    a: int,
    b: int,
    c: int,
}

struct Rgb
{
    r: u8,
    g: u8,
    b: u8,
}

struct V2
{
    x: f32,
    y: f32,
}

extern fn take(p: Pair, big: Big, c: Rgb, v: V2) -> Pair;
extern fn make(n: int) -> Big;

fn call(p: Pair, big: Big, c: Rgb, v: V2) -> Pair
{
    let m = make(p.a);
    let q = take(p, m, c, v);
    q.a = q.a + big.c;
    return q;
}
```

The IR is the same on all six targets, and `--dump-ir` and `--dump-opt` print the same listing for this file. The call of `make` gives the address of its result, and `memcopy` copies a `structs.Big` into the slot of `m`. The field `big.c` lies at `offset_of structs.Big.c`. The return `ret ptr %5` names the address of `q` and copies nothing.

```text
type structs.Pair = struct { a: i64, b: f64 }
type structs.Big = struct { a: i64, b: i64, c: i64 }
type structs.Rgb = struct { r: i8, g: i8, b: i8 }
type structs.V2 = struct { x: f32, y: f32 }
extern fn take(agg structs.Pair, agg structs.Big, agg structs.Rgb, agg structs.V2) -> agg structs.Pair
extern fn make(i64) -> agg structs.Big
fn structs.call(%0: agg structs.Pair, %1: agg structs.Big, %2: agg structs.Rgb, %3: agg structs.V2) -> agg structs.Pair {
b0:
    %4 = slot structs.Big
    %5 = slot structs.Pair
    %6 = load i64 %0
    %7 = call agg @make(%6)
    memcopy %4, %7, structs.Big
    %8 = call agg @take(%0, %4, %2, %3)
    memcopy %5, %8, structs.Pair
    %9 = load i64 %5
    %10 = ptradd %1, offset_of structs.Big.c
    %11 = load i64 %10
    %12 = add i64 %9, %11
    store i64 %12, %5
    ret ptr %5
}
```

Each parameter of aggregate type names its aggregate, and so does an aggregate result. The back end classifies the parameter with `select_layout`, which returns the layout of that aggregate on the target.

## Structs under System V

System V splits an aggregate of up to 16 bytes into eightbytes of 8 bytes each. Each eightbyte starts with the class NO_CLASS and takes the classes of its fields[^1]. An eightbyte whose fields are all floats has class SSE, and one with any integer field has class INTEGER[^1]. An eightbyte without fields keeps NO_CLASS and takes no register. A larger aggregate, or one with unaligned fields, has class MEMORY and goes to the stack[^1]. The psABI allows larger register aggregates only for `__m256` and `__m512` vector types, which Anti lacks[^1].

```c
/* The eightbytes of a System V aggregate of at most 16 bytes. Each one
   goes to an integer register unless all of its members are floats. An
   eightbyte without a member is NO_CLASS and takes no register. Returns
   false for a larger aggregate or one with a field off its alignment,
   whose class is MEMORY. */
static bool sysv_classify(const struct layout *agg, struct arg_location *loc,
                          bool fp[2])
{
    size_t count = (size_t)(agg->size + 7) / 8;
    size_t parts = 0;
    size_t k;
    size_t i;

    if (agg->size > 16 || agg->size == 0 || agg->unaligned) {
        return false;
    }
    for (k = 0; k < count; k++) {
        bool used = false;
        fp[parts] = true;
        for (i = 0; i < agg->member_count; i++) {
            if (agg->members[i].offset / 8 == k) {
                used = true;
                fp[parts] = fp[parts] && select_is_float(agg->members[i].type);
            }
        }
        if (!used) {
            continue;
        }
        loc->parts[parts].offset = (uint8_t)(8 * k);
        loc->parts[parts].bytes = (uint8_t)(agg->size - 8 * k < 8
                                                ? agg->size - 8 * k
                                                : 8);
        parts++;
    }
    loc->part_count = parts;
    return true;
}
```

A union classifies like a struct whose fields all lie at offset 0, because its members come from `flatten`. The union `Num` of an `i64` and an `f64` has one INTEGER eightbyte, and the union `Mix` of an `f32` and an `i32` as well.

An INTEGER eightbyte takes the next free register of `rdi` to `r9`, and an SSE eightbyte the next of `xmm0` to `xmm7`[^1]. If no register is available for any eightbyte, the whole argument goes to the stack and earlier assignments of its eightbytes are reverted[^1]. A MEMORY result goes to storage whose address the caller passes in `rdi`, and the callee returns that address in `rax`[^1]. The result of class INTEGER or SSE returns in `rax` and `rdx` or `xmm0` and `xmm1`[^1].

```c
static void locate(const struct selector *s, const struct ir_function *callee,
                   const enum ir_type *types, size_t count,
                   struct arg_location *out)
{
    const struct abi *a = s->abi;
    struct arg_location result;
    size_t ints = 0;
    size_t floats = 0;
    int64_t offset = a->shadow_space;
    size_t i;
    size_t k;

    locate_result(s, callee, &result);
    if (result.indirect) {
        ints = 1;
    }
    for (i = 0; i < count; i++) {
        bool fp = select_is_float(types[i]);
        size_t position = i + ints * (a == &windows);
        const struct layout *agg =
            i < callee->param_count ? select_layout(s, callee->params[i].agg) : NULL;
        bool reg_fp[2];
        memset(&out[i], 0, sizeof out[i]);
        out[i].copy = -1;
        if (types[i] == IR_AGG && a == &sysv &&
            sysv_classify(agg, &out[i], reg_fp)) {
            size_t need_fp = 0;
            for (k = 0; k < out[i].part_count; k++) {
                need_fp += reg_fp[k];
            }
            if (ints + out[i].part_count - need_fp <= a->int_arg_count &&
                floats + need_fp <= a->fp_arg_count) {
                for (k = 0; k < out[i].part_count; k++) {
                    out[i].parts[k].reg = reg_fp[k] ? a->fp_args[floats++]
                                                    : a->int_args[ints++];
                }
                continue;
            }
            out[i].part_count = 0;
        }
        if (types[i] == IR_AGG && a == &sysv) {
            /* DESIGN: System V gives a stack argument eightbytes, and 16
               bytes of alignment when the type needs more than 8. clang
               passes a 16-aligned aggregate at the next multiple of 16. */
            int64_t align = agg->align > 8 ? 16 : 8;
            out[i].stack = true;
            out[i].size = agg->size;
            offset = (offset + align - 1) / align * align;
            out[i].offset = offset;
            offset += (int64_t)(agg->size + 7) / 8 * 8;
        } else if (types[i] == IR_AGG && position < a->int_arg_count &&
                   windows_by_value(agg)) {
            out[i].part_count = 1;
            out[i].parts[0].reg = a->int_args[position];
            out[i].parts[0].bytes = (uint8_t)agg->size;
        } else if (types[i] == IR_AGG && windows_by_value(agg)) {
            out[i].stack = true;
            out[i].size = agg->size;
            out[i].offset = offset;
            offset += 8;
        } else if (types[i] == IR_AGG && position < a->int_arg_count) {
            out[i].indirect = true;
            out[i].reg = a->int_args[position];
        } else if (types[i] == IR_AGG) {
            out[i].indirect = true;
            out[i].stack = true;
            out[i].offset = offset;
            offset += 8;
        } else if (a == &windows && position < a->int_arg_count) {
            out[i].reg = fp ? a->fp_args[position] : a->int_args[position];
            if (fp && i >= callee->param_count) {
                out[i].copy = a->int_args[position];
            }
        } else if (a == &sysv && fp && floats < a->fp_arg_count) {
            out[i].reg = a->fp_args[floats++];
        } else if (a == &sysv && !fp && ints < a->int_arg_count) {
            out[i].reg = a->int_args[ints++];
        } else {
            out[i].stack = true;
            out[i].offset = offset;
            offset += 8;
        }
    }
}
```

A stack argument takes whole eightbytes, and one whose type aligns to 16 starts at the next multiple of 16. The program `abi_structs.anti` passes two 16-aligned structs to `abi_wide`, one in a register and one on the stack, and clang reads the second one at `16(%rsp)`. The listing of `call` on linux-x86_64 shows each rule. The parameter `p` arrives in `rdi` and `xmm0`, and the prologue stores both into a slot at `24(%rsp)`. The parameter `big` is MEMORY and lies above the return address, so its address is `16(%rbp)`. The parameter `c` arrives as 3 bytes in `rsi` and is stored with `movw` and `movb`, and `v` is one SSE eightbyte in `xmm1`.

```text
structs.call:
b0:
    pushq %rbp
    movq %rsp, %rbp
    subq $192, %rsp
    movq %rbx, 184(%rsp)
    movq %r12, 176(%rsp)
    movq %r13, 168(%rsp)
    movq %r14, 160(%rsp)
    movq %r15, 152(%rsp)
    leaq 24(%rsp), %rbx
    movq %rdi, (%rbx)
    movsd %xmm0, 8(%rbx)
    leaq 16(%rbp), %r10
    movq %r10, 144(%rsp)
    leaq 40(%rsp), %r13
    movw %si, (%r13)
    shrq $16, %rsi
    movb %sil, 2(%r13)
    leaq 48(%rsp), %r14
    movsd %xmm1, (%r14)
    leaq 56(%rsp), %r15
    leaq 80(%rsp), %r10
    movq %r10, 136(%rsp)
    movq (%rbx), %rsi
    leaq 96(%rsp), %r12
    movq %r12, %rdi
    call make
    movq (%r12), %rax
    movq %rax, (%r15)
    movq 8(%r12), %rax
    movq %rax, 8(%r15)
    movq 16(%r12), %rax
    movq %rax, 16(%r15)
    leaq (%rsp), %rax
    movq (%r15), %rcx
    movq %rcx, (%rax)
    movq 8(%r15), %rcx
    movq %rcx, 8(%rax)
    movq 16(%r15), %rcx
    movq %rcx, 16(%rax)
    movq (%rbx), %rdi
    movsd 8(%rbx), %xmm0
    movzwl (%r13), %esi
    movzbl 2(%r13), %eax
    shlq $16, %rax
    orq %rax, %rsi
    movsd (%r14), %xmm1
    call take
    leaq 120(%rsp), %rcx
    movq %rax, (%rcx)
    movsd %xmm0, 8(%rcx)
    movq (%rcx), %rax
    movq 136(%rsp), %r10
    movq %rax, (%r10)
    movq 8(%rcx), %rax
    movq 136(%rsp), %r10
    movq %rax, 8(%r10)
    movq 136(%rsp), %r10
    movq (%r10), %rax
    movq 144(%rsp), %r10
    movq 16(%r10), %rcx
    addq %rcx, %rax
    movq 136(%rsp), %r10
    movq %rax, (%r10)
    movq 136(%rsp), %r10
    movq (%r10), %rax
    movq 136(%rsp), %r10
    movsd 8(%r10), %xmm0
    movq 184(%rsp), %rbx
    movq 176(%rsp), %r12
    movq 168(%rsp), %r13
    movq 160(%rsp), %r14
    movq 152(%rsp), %r15
    movq %rbp, %rsp
    popq %rbp
    ret
```

The call of `make` passes the address `96(%rsp)` in `rdi` and loads `p.a` into `rsi`. The call of `take` copies `m` to the bottom of the stack and joins the 3 bytes of `c` back into `rsi` with `shlq` and `orq`. The result arrives in `rax` and `xmm0`, and the caller stores both into a slot at `120(%rsp)`. The address of the slot of `q` stays live across both calls, spills to `136(%rsp)` and reloads into `r10` before each use.

## Structs under Windows x64

Windows passes a struct of 8, 16, 32 or 64 bits as an integer of that size[^2]. Any other struct is passed as a pointer to memory that the caller allocates, aligned to 16 bytes[^2]. A single argument is never spread across registers[^2]. Arrays are never passed by immediate value[^2]. An Anti array parameter follows the rule of a struct with the same size, because C has no array parameters to agree with. A union follows the rule of its size, so the 8 bytes of `Num` pass as an integer.

```c
/* Windows passes an aggregate of 1, 2, 4 or 8 bytes like an integer of
   that size, and any other aggregate as a pointer to a copy. */
static bool windows_by_value(const struct layout *agg)
{
    return agg->size == 1 || agg->size == 2 || agg->size == 4 ||
           agg->size == 8;
}
```

A struct result of 8, 16, 32 or 64 bits returns in `rax`[^2]. Any other result goes to memory whose address the caller passes as the first argument, and the other arguments shift one position to the right[^2]. The callee returns the same address in `rax`[^2].

On windows-x86_64 the address of the result of `call` arrives in `rcx`, so `p`, `big` and `c` arrive as pointers in `rdx`, `r8` and `r9`. None of the three has 1, 2, 4 or 8 bytes. The 8 bytes of `v` are the fifth argument, at `48(%rbp)` above the shadow store of the caller. The call of `take` makes three copies at multiples of 16 bytes and passes `v` by value at `32(%rsp)`.

```text
    leaq 104(%rsp), %rbx
    leaq 128(%rsp), %rdx
    movq (%rsi), %rax
    movq %rax, (%rdx)
    movq 8(%rsi), %rax
    movq %rax, 8(%rdx)
    leaq 144(%rsp), %r8
    movq (%r14), %rax
    movq %rax, (%r8)
    movq 8(%r14), %rax
    movq %rax, 8(%r8)
    movq 16(%r14), %rax
    movq %rax, 16(%r8)
    leaq 176(%rsp), %r9
    movw (%r12), %ax
    movw %ax, (%r9)
    movb 2(%r12), %al
    movb %al, 2(%r9)
    leaq 32(%rsp), %rax
    movq (%r13), %rcx
    movq %rcx, (%rax)
    movq %rbx, %rcx
    call take
```

The function returns by copying `q` to the address that arrived in `rcx`, saved at `184(%rsp)`, and loading that address into `rax`.

## Structs under AAPCS64

AAPCS64 defines a homogeneous floating-point aggregate, or HFA, as an aggregate whose members all have the same floating-point type, with at most four members[^3]. An HFA takes one float register per member when enough remain[^3]. Otherwise it goes to the stack, and no later argument gets a float register[^3]. Any other aggregate above 16 bytes is copied, and the argument becomes a pointer to the copy[^3]. A smaller one takes consecutive x registers when they suffice[^3]. Otherwise it goes to the stack, and no later argument gets an x register[^3].

```c
/* A homogeneous floating-point aggregate: one to four members, all f32 or
   all f64. Returns the number of members, or 0. */
static size_t hfa_members(const struct layout *agg)
{
    size_t i;

    if (agg->member_count == 0 || agg->member_count > 4 ||
        !select_is_float(agg->members[0].type)) {
        return 0;
    }
    for (i = 1; i < agg->member_count; i++) {
        if (agg->members[i].type != agg->members[0].type) {
            return 0;
        }
    }
    return agg->member_count;
}
```

The members of a union come from `flatten`, so the union `FF` of two `f32` fields is an HFA of one member. The union `Mix` has an `f32` and an `i32` at offset 0, two members of different types, and passes in an x register. The function `aggregate_parts` gives an HFA one float register per member and any other aggregate one x register per 8 bytes.

```c
/* The register parts of an aggregate, starting at register first. A
   homogeneous float aggregate takes one register per member. Another
   aggregate of at most 16 bytes takes 8-byte parts. */
static void aggregate_parts(const struct layout *agg, uint8_t first,
                            struct arg_location *out)
{
    size_t n = hfa_members(agg);
    size_t k;

    if (n > 0) {
        for (k = 0; k < n; k++) {
            out->parts[k].reg = (uint8_t)(first + k);
            out->parts[k].bytes = agg->members[0].type == IR_F32 ? 4 : 8;
            out->parts[k].offset = (uint8_t)agg->members[k].offset;
        }
        out->part_count = n;
        return;
    }
    n = (size_t)(agg->size + 7) / 8;
    for (k = 0; k < n; k++) {
        out->parts[k].reg = (uint8_t)(first + k);
        out->parts[k].bytes =
            (uint8_t)(agg->size - 8 * k < 8 ? agg->size - 8 * k : 8);
        out->parts[k].offset = (uint8_t)(8 * k);
    }
    out->part_count = n;
}
```

A result returns in the registers that the same type would take as the only argument. Any other result goes to memory whose address the caller passes in `x8`[^3]. Windows ARM64 applies the same rules to named arguments and results[^5]. Its list of definitions gives an HFA 2 to 4 members, while its section on return values requires only a non-empty type of identical float members[^5]. The compiler antic follows AAPCS64 on all three ARM64 conventions, which a Windows host has yet to confirm.

An argument with alignment 16 starts at an even x register, by rule C.10 of AAPCS64[^3]. On the stack it starts at a multiple of 16, because rule C.14 rounds the stack address up to its natural alignment[^3]. The function `locate` skips one register for such an aggregate when the number of used x registers is odd, except on Apple. Apple clang 21.0.0 compiles `int64_t wide(int64_t a, Wide w, int64_t b)` with `-O2`, for a 16-aligned `Wide` of one `int8_t`. With `-target aarch64-unknown-linux-gnu` and with `-target aarch64-pc-windows-msvc` it reads `w.a` from `x2` and `b` from `x4`. With `-target arm64-apple-macos` it reads `w.a` from `x1` and `b` from `x3`.

```c
/* DESIGN: AAPCS64 counts integer and float arguments separately, and an
   argument after its eight registers goes to the stack in 8 bytes. Apple
   gives a named stack argument its own size at its own alignment and
   passes every variadic argument on the stack in 8 bytes. Windows passes
   a variadic float in an integer register. A homogeneous float aggregate
   takes float registers and any other aggregate of up to 16 bytes integer
   registers, all or none. A larger aggregate is a pointer to a copy. On
   the stack, Apple aligns a float aggregate to its members, and every
   other aggregate takes 8-byte slots. */
static void locate(const struct selector *s, const struct ir_function *callee,
                   const enum ir_type *types, size_t count,
                   struct arg_location *out)
{
    size_t ints = 0;
    size_t floats = 0;
    int64_t next = 0;
    int64_t align;
    size_t i;

    for (i = 0; i < count; i++) {
        bool variadic = i >= callee->param_count;
        bool fp = select_is_float(types[i]) &&
                  !(s->abi == &windows && variadic);
        int64_t size = s->abi == &apple && !variadic ? bits(types[i]) / 8 : 8;
        memset(&out[i], 0, sizeof out[i]);
        out[i].copy = -1;
        if (types[i] == IR_AGG) {
            const struct layout *agg = select_layout(s, callee->params[i].agg);
            size_t hfa = hfa_members(agg);
            size_t parts = (size_t)(agg->size + 7) / 8;
            size = 8;
            if (hfa > 0 && floats + hfa <= s->abi->fp_arg_count) {
                aggregate_parts(agg, s->abi->fp_args[floats], &out[i]);
                floats += hfa;
                continue;
            }
            /* An aggregate aligned to 16 starts at an even register,
               except on Apple. */
            if (hfa == 0 && agg->size <= 16 && agg->align == 16 &&
                s->abi != &apple && ints % 2 == 1 &&
                ints + 1 + parts <= s->abi->int_arg_count) {
                ints++;
            }
            if (hfa == 0 && agg->size <= 16 &&
                ints + parts <= s->abi->int_arg_count) {
                aggregate_parts(agg, s->abi->int_args[ints], &out[i]);
                ints += parts;
                continue;
            }
            if (hfa == 0 && agg->size > 16 && ints < s->abi->int_arg_count) {
                out[i].indirect = true;
                out[i].reg = s->abi->int_args[ints++];
                continue;
            }
            if (hfa > 0) {
                floats = s->abi->fp_arg_count;
            } else if (agg->size <= 16) {
                ints = s->abi->int_arg_count;
            }
            out[i].stack = true;
            out[i].indirect = agg->size > 16 && hfa == 0;
            align = 8;
            if (!out[i].indirect) {
                out[i].size = agg->size;
                size = (int64_t)(agg->size + 7) / 8 * 8;
            }
            if (hfa > 0 && s->abi == &apple) {
                align = (int64_t)agg->align;
                size = (int64_t)agg->size;
            } else if (agg->align == 16 && !out[i].indirect) {
                align = 16;
            }
            out[i].offset = (next + align - 1) / align * align;
            next = out[i].offset + size;
            continue;
        }
        if (s->abi == &apple && variadic) {
            out[i].stack = true;
        } else if (fp && floats < s->abi->fp_arg_count) {
            out[i].reg = s->abi->fp_args[floats++];
        } else if (!fp && ints < s->abi->int_arg_count) {
            out[i].reg = s->abi->int_args[ints++];
        } else {
            out[i].stack = true;
        }
        if (out[i].stack) {
            out[i].offset = (next + size - 1) / size * size;
            next = out[i].offset + size;
        }
    }
}
```

The three ARM64 targets produce the same listing for `structs.anti`, which the test `dump_alloc_structs.macos-arm64` and its two siblings pin. The parameter `p` arrives in `x0` and `x1`, and `big` as a pointer in `x2`. The parameter `c` arrives in `x3`, and `v` arrives as an HFA in `s0` and `s1`.

```text
    add x19, sp, #0
    str x0, [x19]
    str x1, [x19, #8]
    mov x20, x2
    add x21, sp, #16
    strh w3, [x21]
    lsr x9, x3, #16
    strb w9, [x21, #2]
    add x22, sp, #24
    str s0, [x22]
    str s1, [x22, #4]
```

The call of `make` passes the address of its result in `x8`. The call of `take` copies `m` to `sp+96` and passes that address in `x2`.

```text
    add x2, sp, #96
    ldr x9, [x23]
    str x9, [x2]
    ldr x9, [x23, #8]
    str x9, [x2, #8]
    ldr x9, [x23, #16]
    str x9, [x2, #16]
    ldr x0, [x19]
    ldr x1, [x19, #8]
    ldrh w3, [x21]
    ldrb w9, [x21, #2]
    lsl x9, x9, #16
    orr x3, x3, x9
    ldr s0, [x22]
    ldr s1, [x22, #4]
    bl take
```

Apple lets a named stack argument take fewer than 8 bytes[^4]. Its document does not state where an aggregate goes on the stack. The test `program_abi_structs` below calls a C function whose last `V2` argument goes to the stack after a 2-byte `i16` argument. A callee compiled by Apple clang 21.0.0 reads that HFA at the next multiple of 4, the alignment of `f32`. It reads an aggregate that is no HFA at the next multiple of 8. Under the AAPCS64 rule, a multiple of 8 for both, a caller writes `v2` 4 bytes above the place where that callee reads it. The function `locate` therefore gives an HFA on the Apple stack the alignment of its members.

## Moving parts in and out of registers

A register part of an aggregate may hold 1 to 8 bytes. A part of 1, 2, 4 or 8 bytes is one load or store of that width. A part of 3, 5, 6 or 7 bytes moves in pieces of 4, 2 and 1 bytes joined with shifts. No load then reads beyond the aggregate, and no store writes beyond it.

```c
/* Load bytes of an aggregate at offset after base into register dst. An
   integer part of 3, 5, 6 or 7 bytes combines its pieces with shifts. */
static void load_bytes(struct selector *s, struct mach_operand dst,
                       struct mach_operand base, int64_t offset,
                       unsigned bytes)
{
    unsigned low = bytes >= 4 ? 4 : 2;
    struct mach_operand rest;

    if (is_float_register(s, dst)) {
        move_float(s, widened(dst, (uint8_t)(bytes * 8)),
                   memory_at(base, offset, (uint8_t)(bytes * 8)));
    } else if (bytes == 8 || bytes == 4) {
        emit2(s, X64_MOV, widened(dst, (uint8_t)(bytes * 8)),
              memory_at(base, offset, (uint8_t)(bytes * 8)));
    } else if (bytes == 2 || bytes == 1) {
        emit2(s, X64_MOVZX, widened(dst, 32),
              memory_at(base, offset, (uint8_t)(bytes * 8)));
    } else {
        load_bytes(s, dst, base, offset, low);
        rest = select_new_vreg(s, 64);
        load_bytes(s, rest, base, offset + low, bytes - low);
        emit2(s, X64_SHL, rest, mach_imm(8 * low));
        emit2(s, X64_OR, widened(dst, 64), rest);
    }
}
```

A slot that receives register parts is a multiple of 8 bytes, so that a part of 8 bytes stays inside it.

```c
/* DESIGN: a slot for an aggregate that register parts fill is a multiple
   of 8 bytes, so a part of 8 bytes never writes past its end. */
static uint32_t aggregate_slot(struct selector *s, const struct layout *agg)
{
    return mach_slot_add(s->out, (agg->size + 7) / 8 * 8,
                         agg->align < 8 ? 8 : agg->align);
}
```

On entry, `select_params` stores the parts of a register aggregate into such a slot. A pointer argument, in a register or on the stack, becomes the address of the parameter. An aggregate on the stack becomes its address above the frame.

```c
        if (types[i] == IR_AGG) {
            dst = mach_vreg(f->params[i].temp, 64);
            if (locations[i].part_count > 0) {
                s->target->slot_address(s, dst,
                                        aggregate_slot(s, select_layout(
                                                              s, f->params[i].agg)));
                select_store_parts(s, &locations[i], dst);
            } else if (locations[i].indirect && locations[i].stack) {
                s->target->stack_param(s, locations[i].offset, dst);
            } else if (locations[i].indirect) {
                s->target->move(s, dst, mach_preg(locations[i].reg, 64));
            } else {
                s->target->incoming_address(s, dst, locations[i].offset);
            }
            continue;
        }
```

A return copies the result to the address that arrived for it, or loads its parts into the result registers.

```c
static void emit_ret(struct selector *s, const struct ir_inst *inst)
{
    struct mach_inst *ret;

    uint8_t result = select_is_float(inst->type) ? s->abi->fp_result
                                                  : s->abi->int_result;
    struct arg_location loc;
    uint64_t uses = 0;

    if (s->f->result == IR_AGG) {
        locate_result(s, s->f, &loc);
        if (loc.indirect) {
            copy_memory(s, s->result_address, select_reg(s, &inst->a),
                        select_layout(s, s->f->result_agg)->size);
            move(s, mach_preg((uint32_t)loc.copy, 64), s->result_address);
            uses = BIT(loc.copy);
        } else {
            uses = select_load_parts(s, &loc, select_reg(s, &inst->a));
        }
    } else if (inst->type != IR_VOID) {
        load_value(s, mach_preg(result, width(inst->type)), &inst->a);
        uses = BIT(result);
    }
    ret = select_emit(s, X64_RET, 0, NULL);
    ret->uses = uses;
}
```

## Packed and aligned structs in calls

Since the host runs macos-x86_64 programs, `program_abi_structs_macos-x86_64` compares antic with clang for that target as well, and it found the missing 16-byte alignment of a stack argument. The unit test `test_select` in `tests/unit/test_select.c` selects code for a packed struct and a 16-aligned struct. Its expectations for linux-x86_64, linux-arm64 and macos-arm64 follow the source below.

```c
static const char aligned[] = "packed struct Tight { a: i8, b: i32 }\n"
                              "struct Wide align(16) { a: i8 }\n"
                              "fn tight(t: Tight, b: int) -> int {\n"
                              "    return t.b as int + b;\n"
                              "}\n"
                              "fn wide(a: int, w: Wide, b: int) -> int {\n"
                              "    return a + w.a as int + b;\n"
                              "}\n";

void test_select(void)
{
    /* System V passes a struct with a field off its alignment in memory,
       and the empty second eightbyte of Wide takes no register. */
    selects(aligned, TARGET_LINUX_X86_64,
            "main.tight:\n"
            "b0:\n"
            "    leaq 16(%rbp), %t0\n"
            "    movq %rdi, %t1\n"
            "    movl 1(%t0), %t3\n"
            "    movslq %t3, %t4\n"
            "    movq %t4, %t5\n"
            "    addq %t1, %t5\n"
            "    movq %t5, %rax\n"
            "    ret\n"
            "main.wide:\n"
            "b0:\n"
            "    movq %rdi, %t0\n"
            "    leaq slot0, %t1\n"
            "    movq %rsi, (%t1)\n"
            "    movq %rdx, %t2\n"
            "    movb (%t1), %t3\n"
            "    movsbq %t3, %t4\n"
            "    movq %t0, %t5\n"
            "    addq %t4, %t5\n"
            "    movq %t5, %t6\n"
            "    addq %t2, %t6\n"
            "    movq %t6, %rax\n"
            "    ret\n");
```

The struct `Tight` has 5 bytes, and its field `b` lies at offset 1, off its alignment. System V passes it in memory, so `tight` takes the address `16(%rbp)` and reads `t.b` at offset 1. The struct `Wide` has 16 bytes and one member at offset 0. Its first eightbyte takes `rsi`, and its second eightbyte takes no register, so `b` arrives in `rdx`. Apple clang 21.0.0 compiles both functions in C with `-O2 -target x86_64-unknown-linux-gnu`. Its `tight` reads `t.b` at `9(%rsp)`, and its `wide` reads `w.a` from `sil`.

On linux-arm64 the 5 bytes of `Tight` arrive in `x0`. The struct `Wide` skips `x1` and arrives in `x2` and `x3`, and `b` in `x4`. On macos-arm64 `Wide` arrives in `x1` and `x2`, and `b` in `x3`.

## Narrow arguments to C

System V leaves the bits of an INTEGER value above its memory width unspecified and expects the consumer to extend it[^1]. Apple clang 21.0.0 compiles `int widen(short s, unsigned char u) { return s + u; }` for `x86_64-unknown-linux-gnu` to `leal (%rdi,%rsi), %eax`, which reads 32 bits of each argument register. The same compiler extends both arguments with `movswl` and `movzbl` in the caller. For a stack argument, and for `x86_64-pc-windows-msvc`, the callee extends. The compiler antic extends register arguments of 8 and 16 bits on System V, from the `signext` or `zeroext` of chapter 15.

```c
        /* DESIGN: clang callees on System V read an 8-bit or 16-bit
           register argument as 32 bits, so the caller extends it. */
        if (s->abi == &sysv && i < callee->param_count &&
            callee->params[i].ext != IR_EXT_NONE) {
            extend_into(s, mach_preg(locations[i].reg, 32), arg,
                        callee->params[i].ext == IR_EXT_SIGN);
        } else {
            load_value(s, reg, arg);
        }
```

The function `emit_call` of chapter 14 holds this branch with the reason in its DESIGN comment. Apple callers on ARM64 extend as well, as chapter 15 describes.

## Frame size on x86_64

Arrays make frames of any size possible. The x86_64 instruction `sub` and the displacement of a memory operand hold signed 32-bit values. A frame of 2^31 bytes or more on x86_64 is therefore a compile error. The unit test in `tests/unit/test_emit.c` compiles the array `[7 as u8; 3000000000]` in the function `main` of module `main`. It expects ``the stack frame of `main.main` needs 3000000000 bytes, and an x86_64 frame holds at most 2147483647``. ARM64 frames have no limit, because chapter 15 moves large offsets through a register.

## Checking against C

The file `tests/abi/structs.c` defines C functions that take and return structs and unions by value. The CMake build compiles it with the C compiler of the build, and the test `program_abi_structs` links the object with `tests/abi/abi_structs.anti`. The driver passes an `.o`, `.a`, `.obj` or `.lib` from the command line to the linker after the object of the program. The function `abi_stack` gives every field of its 22 arguments its own weight, so a misplaced argument changes the result.

```c
/* Seven doubles leave one float register, so t, v and v2 go to the stack.
   Each argument has its own weight, so a misplaced one changes the sum. */
double abi_stack(double d0, double d1, double d2, double d3, double d4,
                 double d5, double d6, struct Tri t, struct V2 v, float f,
                 struct Rgb c, uint8_t u, struct Pair p, int64_t i0,
                 int64_t i1, int64_t i2, int64_t i3, int64_t i4, int64_t i5,
                 struct Rgb c2, int16_t s, struct V2 v2)
{
    return d0 + 2 * d1 + 3 * d2 + 4 * d3 + 5 * d4 + 6 * d5 + 7 * d6 +
           8 * t.a + 9 * t.b + 10 * t.c + 11 * v.x + 12 * v.y + 13 * f +
           14 * (c.r + 2 * c.g + 3 * c.b) + 15 * u + 16 * (double)p.a +
           17 * p.b + 18 * (double)i0 + 19 * (double)i1 + 20 * (double)i2 +
           21 * (double)i3 + 22 * (double)i4 + 23 * (double)i5 +
           24 * (c2.r + 2 * c2.g + 3 * c2.b) + 25 * s + 26 * v2.x +
           27 * v2.y;
}
```

Three unions and a struct with a union field test the classification of fields at offset 0. The union `Num` is an integer, `FF` a float aggregate of one member and `Mix` an integer. The struct `Holder` places `Num` after an `int8_t`.

```c
union Num { int64_t i; double d; };
union FF { float a; float b; };
union Mix { float f; int32_t i; };
struct Holder { int8_t tag; union Num n; };
```

A packed struct, a 16-aligned struct and a struct of bitfields test these layouts in calls. The last function calls `anti_twice`, an `export fn` of the Anti program, from C. An `export fn` has the C symbol of its name, as [chapter 16]({{% relref "/programming/writing-a-compiler/16-assembly-emission" %}}) describes.

```c
struct __attribute__((packed)) Tight { int8_t a; int32_t b; };
typedef struct { _Alignas(16) int8_t a; } Wide;

/* A packed struct with a field away from its alignment passes in memory
   on System V. A 16-aligned struct starts at an even register on AAPCS64
   outside Apple, and on the stack at a multiple of 16. */
struct Tight abi_tight(struct Tight t, int64_t k)
{
    struct Tight r;

    r.a = (int8_t)(t.a + 1);
    r.b = t.b + (int32_t)k;
    return r;
}

int64_t abi_wide(int64_t a, Wide w, int64_t b, int64_t c, int64_t d,
                 int64_t e, int64_t f, Wide w2, int64_t g)
{
    return a + 2 * w.a + 3 * b + 4 * c + 5 * d + 6 * e + 7 * f + 8 * w2.a +
           9 * g;
}

struct Bits { uint32_t visible : 1; uint32_t layer : 4; int8_t level : 3; uint16_t pad : 9; };

/* Bitfields follow the System V rule on Linux and macOS and the MSVC rule
   on Windows. */
struct Bits abi_bits(struct Bits b)
{
    struct Bits r = b;

    r.layer = b.layer + 1;
    /* gcc warns for every assignment to a narrow signed bitfield that it
       cannot range-check, and the field holds three bits. The test passes
       -2, and -4 is the smallest value the field holds. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#endif
    r.level = (int8_t)(b.level * 2);
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
    r.pad = (uint16_t)(b.pad + 7);
    return r;
}

int64_t anti_twice(int64_t x);

/* A C caller of an export fn of the Anti program. */
int64_t abi_calls_anti(int64_t x)
{
    return anti_twice(x) + 1;
}
```

The Anti program declares the same types with `union`, `packed`, `align(16)` and bitfield widths, and defines `anti_twice`.

```anti
union Num
{
    i: int,
    d: f64,
}

union FF
{
    a: f32,
    b: f32,
}

union Mix
{
    f: f32,
    i: i32,
}

struct Holder
{
    tag: i8,
    n: Num,
}

packed struct Tight
{
    a: i8,
    b: i32,
}

struct Wide align(16)
{
    a: i8,
}

struct Bits
{
    visible: u32 : 1,
    layer: u32 : 4,
    level: i8 : 3,
    pad: u16 : 9,
}

export fn anti_twice(x: int) -> int
{
    return 2 * x;
}
```

A C program with the same calls, compiled with Apple clang 21.0.0 together with `structs.c`, prints the lines of `abi_structs.expected`. The Anti program prints the same lines on macos-arm64.

```text
exit 0
42
2.5
12003
1966090
1.75
6
-21
1.5
1734
999
-6
321
1145.75
7261.875
8417
904
4.25
3
3.75
42
742
51023
285
1066407
41
```

The three lines after 7261.875 come from `abi_huge`. The compiler antic copies its 104-byte struct with `memcpy`, and its result arrives through the address in `x8`. The four lines from 3 to 742 come from the unions and `Holder`. The value 51023 encodes the result of `abi_tight`, with `a` 5 and `b` 1023. The value 285 is the sum of the nine weighted arguments of `abi_wide`. The value 1066407 encodes the four fields 1, 7, -4 and 407 of the result of `abi_bits`. The last line, 41, is `anti_twice(20) + 1`. The x86_64 code of the same program assembles for all three x86_64 triples and does not run on the development Mac.

## ABI probe

The ABI probe compares the layouts of C and Anti. The program `tests/abi/probe.c` prints the size and the alignment of each struct and union. For each field it prints the bytes of an object that it cleared first and then gave that one field. That image carries the offset and the value together, and needs no `offsetof`. The types cover `c_long`, `c_wchar`, a union, bitfields, packed structs, an aligned struct, a union of bitfields and a struct with zero-width bitfields.

```c
struct Wide
{
    int8_t a;
    long b;
    wchar_t c;
    int16_t d;
};

union Mixed
{
    int8_t a;
    int32_t b;
    double c;
    int64_t d;
};

struct Flags
{
    uint32_t visible : 1;
    uint32_t layer : 4;
    int8_t level : 3;
    uint16_t pad : 9;
};

struct Spread
{
    int8_t a;
    uint32_t b : 5;
    int64_t c : 40;
    uint8_t d : 7;
};

#pragma pack(push, 1)
struct Tight
{
    int8_t a;
    int32_t b;
    int16_t c;
};

struct TightBits
{
    uint8_t a : 3;
    uint32_t b : 7;
};
#pragma pack(pop)

struct Aligned
{
    _Alignas(16) int8_t a;
    int32_t b;
};

union BitUnion
{
    uint32_t a : 3;
    uint8_t b : 7;
};

struct Breaks
{
    uint8_t a;
    uint32_t : 0;
    uint8_t b : 3;
    uint64_t : 0;
    uint8_t c : 5;
};

static void dump(const char *name, const void *value, size_t size)
{
    const unsigned char *p = value;
    size_t i;

    printf("%s", name);
    for (i = 0; i < size; i++) {
        printf(" %02x", p[i]);
    }
    printf("\n");
}

#define TYPE(T, name)                                                     \
    printf("%s size %d align %d\n", name, (int)sizeof(T),                  \
           (int)_Alignof(T))

#define FIELD(T, name, f)                                                 \
    do {                                                                  \
        T v;                                                              \
        memset(&v, 0, sizeof v);                                          \
        v.f = 1;                                                          \
        dump(name "." #f, &v, sizeof v);                                  \
    } while (0)
```

The program `tests/abi/probe.anti` prints the same lines for the same types. The function `size` prints the size and computes the alignment from the `Box` struct of the type. For each field, `main` clears the value, sets the field to 1 and calls `dump`.

```anti
fn clear(p: *byte, n: int)
{
    let i = 0;
    while i < n do {
        p[i] = 0;
        i += 1;
    }
}

fn dump(name: str, p: *byte, n: int)
{
    printf("%s".ptr, name.ptr);
    let i = 0;
    while i < n do {
        printf(" %02x".ptr, p[i] as c_int);
        i += 1;
    }
    printf("\n".ptr);
}

fn size(name: str, n: int, box: int)
{
    printf("%s size %d align %d\n".ptr, name.ptr, n as c_int,
        (box - n) as c_int);
}
```

The script `tests/run_probe.cmake` compiles both programs, runs both and compares their output. The test `abi_probe` runs it on every host except Windows.

```cmake
# The ABI probe on the host: compile probe.c with the C compiler and
# probe.anti with antic, run both and compare their output byte for byte.
# Run with cmake -P and these values:
#   ANTIC     the antic executable
#   LLVM_MC   the llvm-mc executable
#   RUNTIME   the runtime directory
#   SOURCES   tests/abi
#   WORK      a directory for the executables

file(MAKE_DIRECTORY "${WORK}")
execute_process(COMMAND cc -std=c11 -o "${WORK}/probe_c" "${SOURCES}/probe.c"
    RESULT_VARIABLE status ERROR_VARIABLE err)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "cc probe.c failed\n${err}")
endif()
execute_process(
    COMMAND "${ANTIC}" --llvm-mc "${LLVM_MC}" --runtime "${RUNTIME}"
            -o "${WORK}/probe_anti" "${SOURCES}/probe.anti"
    RESULT_VARIABLE status ERROR_VARIABLE err)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "antic probe.anti failed\n${err}")
endif()
execute_process(COMMAND "${WORK}/probe_c" OUTPUT_VARIABLE from_c)
execute_process(COMMAND "${WORK}/probe_anti" OUTPUT_VARIABLE from_anti)
if(NOT from_c STREQUAL from_anti)
    message(FATAL_ERROR "the layouts differ\nC:\n${from_c}\nAnti:\n${from_anti}")
endif()
```

On the development Mac both programs print the lines below. The field `Wide.b` starts at byte 8, so `c_long` has 8 bytes on macOS, and `Wide.c` is a `c_wchar` of 4 bytes at byte 16. The byte `20` of `Flags.level` is bit 5, and `Flags.pad` starts at byte 2. The field `Spread.c` starts at bit 13, the byte `20` of byte 1. Both fields of `BitUnion` start at bit 0. In `Breaks` the zero-width `u32` moves `b` to byte 4, and the zero-width `u64` moves `c` to byte 8. The last line reads the fields of the constant `BREAKS` back. A struct assignment leaves the padding of its destination unspecified in C. The probe therefore compares a whole byte image only where both languages cleared the object first.

```text
Wide size 24 align 8
Wide.a 01 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
Wide.b 00 00 00 00 00 00 00 00 01 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
Wide.c 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 01 00 00 00 00 00 00 00
Wide.d 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 01 00 00 00
Mixed size 8 align 8
Mixed.a 01 00 00 00 00 00 00 00
Mixed.b 01 00 00 00 00 00 00 00
Mixed.c 00 00 00 00 00 00 f0 3f
Mixed.d 01 00 00 00 00 00 00 00
Flags size 4 align 4
Flags.visible 01 00 00 00
Flags.layer 02 00 00 00
Flags.level 20 00 00 00
Flags.pad 00 00 01 00
Spread size 8 align 8
Spread.a 01 00 00 00 00 00 00 00
Spread.b 00 01 00 00 00 00 00 00
Spread.c 00 20 00 00 00 00 00 00
Spread.d 00 00 00 00 00 00 00 01
Tight size 7 align 1
Tight.a 01 00 00 00 00 00 00
Tight.b 00 01 00 00 00 00 00
Tight.c 00 00 00 00 00 01 00
TightBits size 2 align 1
TightBits.a 01 00
TightBits.b 08 00
Aligned size 16 align 16
Aligned.a 01 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
Aligned.b 00 00 00 00 01 00 00 00 00 00 00 00 00 00 00 00
BitUnion size 4 align 4
BitUnion.a 01 00 00 00
BitUnion.b 01 00 00 00
Breaks size 9 align 1
Breaks.a 01 00 00 00 00 00 00 00 00
Breaks.b 00 00 00 00 01 00 00 00 00
Breaks.c 00 00 00 00 00 00 00 00 01
Breaks.const a 1 b 2 c 3
```

## Binding to raylib

The library raylib is written in C for programming games[^6]. Its header `raymath.h` declares vector, matrix and quaternion functions that take their parameters by value and return structs[^7]. The struct `Vector2` holds two `float` fields, and `Matrix` holds sixteen[^7]. The header defines every function inline, and `RAYMATH_IMPLEMENTATION` makes one file emit external definitions[^7]. The file `tests/abi/raymath.c` is that one file.

```c
/* The raymath functions of raylib as out-of-line C functions. The program
   test abi_raymath calls them from Anti through a small binding. */
#define RAYMATH_IMPLEMENTATION
#include "raymath.h"
```

The script `tools/get-raylib.cmake` downloads the source archive of raylib 6.0 and checks its SHA-256 digest. The GitHub API lists digests for the ten binary assets of the release and none for the source archive[^8]. The file `tools/raylib-pin` therefore holds the digest of the archive at its first download, with the version. CMake downloads and unpacks the archive itself, so the one script runs on every host that builds antic. The CMake cache variable `ANTIC_RAYLIB_DIR` names the extracted release. The test `program_abi_raymath` fails with `raylib not found` when the header is missing.

The binding declares four structs with the field names of raylib and ten functions. The type `Quaternion` is a `Vector4` in raylib[^7], so the binding uses `Vector4` for both. A `Matrix` of 64 bytes is not an HFA, because it has sixteen members. It passes as a pointer to a copy on ARM64 and on the stack under System V.

```anti
extern fn printf(format: *byte, ...) -> i32;

struct Vector2
{
    x: f32,
    y: f32,
}

struct Vector3
{
    x: f32,
    y: f32,
    z: f32,
}

struct Vector4
{
    x: f32,
    y: f32,
    z: f32,
    w: f32,
}

struct Matrix
{
    m0: f32,
    m4: f32,
    m8: f32,
    m12: f32,
    m1: f32,
    m5: f32,
    m9: f32,
    m13: f32,
    m2: f32,
    m6: f32,
    m10: f32,
    m14: f32,
    m3: f32,
    m7: f32,
    m11: f32,
    m15: f32,
}

extern fn Vector2Add(v1: Vector2, v2: Vector2) -> Vector2;
extern fn Vector2Rotate(v: Vector2, angle: f32) -> Vector2;
extern fn Vector3CrossProduct(v1: Vector3, v2: Vector3) -> Vector3;
extern fn Vector3Transform(v: Vector3, mat: Matrix) -> Vector3;
extern fn Vector3RotateByQuaternion(v: Vector3, q: Vector4) -> Vector3;
extern fn MatrixMultiply(left: Matrix, right: Matrix) -> Matrix;
extern fn MatrixRotateZ(angle: f32) -> Matrix;
extern fn MatrixTranslate(x: f32, y: f32, z: f32) -> Matrix;
extern fn QuaternionFromEuler(pitch: f32, yaw: f32, roll: f32) -> Vector4;
extern fn QuaternionMultiply(q1: Vector4, q2: Vector4) -> Vector4;

fn line(v: f32)
{
    let format = alloc(byte, 6);
    format[0] = 37;
    format[1] = 46;
    format[2] = 57;
    format[3] = 103;
    format[4] = 10;
    format[5] = 0;
    printf(format, v as f64);
    free(format);
}

fn main() -> int
{
    let a = Vector2Add(Vector2 { x: 1.5, y: -2.0 }, Vector2 { x: 0.25, y: 8.0 });
    line(a.x);
    line(a.y);
    let r = Vector2Rotate(a, 0.5);
    line(r.x);
    line(r.y);
    let c = Vector3CrossProduct(Vector3 { x: 1.0, y: 2.0, z: 3.0 },
        Vector3 { x: -4.0, y: 0.5, z: 2.0 });
    line(c.x);
    line(c.y);
    line(c.z);
    let m = MatrixMultiply(MatrixRotateZ(0.25), MatrixTranslate(3.0, -1.0, 2.0));
    line(m.m0);
    line(m.m4);
    line(m.m12);
    line(m.m13);
    let t = Vector3Transform(c, m);
    line(t.x);
    line(t.y);
    line(t.z);
    let q = QuaternionMultiply(QuaternionFromEuler(0.1, 0.2, 0.3),
        QuaternionFromEuler(-0.4, 0.5, 0.6));
    line(q.x);
    line(q.y);
    line(q.z);
    line(q.w);
    let v = Vector3RotateByQuaternion(Vector3 { x: 1.0, y: 0.0, z: 0.0 }, q);
    line(v.x);
    line(v.y);
    line(v.z);
    return 0;
}
```

A C program that makes the same calls and prints the same fields, compiled with Apple clang 21.0.0 and linked with the same `raymath.o`, prints the 21 lines of `abi_raymath.expected`. The Anti program prints the same 21 lines on macos-arm64.

```text
exit 0
1.75
6
-1.34078383
6.1044898
2.5
-14
8.5
0.968912423
-0.247403964
3
-1
8.88593674
-13.9462633
10.5
-0.21106334
0.218570754
0.4833709
0.820994437
0.437158972
0.701425076
-0.562934518
```

## Tests

The unit tests in `tests/unit/test_layout.c` check the layouts of structs, unions, packed and aligned structs and the target-sized C types. They also check the members of unions, both bitfield rules and the errors for array lengths and alignments. The tests in `tests/unit/test_lower.c` check aggregate parameters, slots, literals, copies, array indexing, `.len`, symbolic lengths, bitfield loads and stores, unions and the type table entries of `packed` and `align(16)`. The tests in `tests/unit/test_struct.c` check both copy strategies and the stack HFA on linux-arm64 and macos-arm64. The file `tests/unit/test_select.c` checks the packed and aligned placement, `tests/unit/test_x86_64.c` the extension of narrow arguments and `tests/unit/test_emit.c` the frame limit.

The dump tests pin the IR of `structs.anti` and its listings for System V, Windows x64 and the three ARM64 targets. The asm tests assemble `structs.anti`, `sizes.anti`, `bitfields.anti`, `abi_structs.anti` and `probe.anti` for all six triples. The tests `program_sizes`, `program_bitfields`, `program_structs`, `program_abi_structs`, `program_abi_raymath` and `abi_probe` run programs on the development Mac.

## Next

[Chapter 19, Strings]({{% relref "/programming/writing-a-compiler/19-strings-slices-and-bytes" %}}), slices and bytes, adds `str` and `[]T` as pointer-plus-length values and string literals in read-only data. It covers `char` and UTF-8 decoding, byte strings, and calling libc with `s.ptr`.

## References

[^1]: H.J. Lu and others, *System V Application Binary Interface, AMD64 Architecture Processor Supplement*, version 1.0, March 12, 2025, sections 3.1.2 and 3.2.3, https://gitlab.com/x86-psABIs/x86-64-ABI

[^2]: Microsoft, *x64 calling convention*, sections "Calling convention defaults", "Parameter passing" and "Return values", https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention?view=msvc-170

[^3]: Arm Limited, *Procedure Call Standard for the Arm 64-bit Architecture*, release 2025Q4, sections 5.10, 6.8.2 and 6.9, https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst

[^4]: Apple, *Writing ARM64 code for Apple platforms*, sections on arguments and variadic functions, https://developer.apple.com/documentation/xcode/writing-arm64-code-for-apple-platforms

[^5]: Microsoft, *Overview of ARM64 ABI conventions*, sections "Definitions", "Parameter passing" and "Return values", https://learn.microsoft.com/en-us/cpp/build/arm64-windows-abi-conventions?view=msvc-170

[^6]: R. Santamaria, *raylib*, release 6.0, file `README.md`, https://github.com/raysan5/raylib/blob/6.0/README.md

[^7]: R. Santamaria, *raylib*, release 6.0, file `src/raymath.h`, https://github.com/raysan5/raylib/blob/6.0/src/raymath.h

[^8]: GitHub REST API, `GET /repos/raysan5/raylib/releases/tags/6.0`, queried on September 14, 2026, fields `assets[].digest` and `tarball_url`, https://docs.github.com/en/rest/releases/releases
