---
title: "Strings, slices and bytes"
description: "How antic compiles str and slices as pointer-plus-length aggregates, puts literals into read-only data and hands UTF-8 arguments to main."
summary: "`str` and `[]T` as pointer-plus-length values, string literals in read-only data, `char` and UTF-8 decoding, byte strings, and calling libc with `s.ptr`."
date: 2026-09-13T22:32:00+02:00
lastmod: 2026-09-15T03:52:27+02:00
draft: false
weight: 190
tags: [compilers, assembly]
keywords: [string literals, slices, read-only data section, UTF-8 decoding, U+FFFD substitution, command-line arguments, symbolic field offsets, pointer plus length]
---

## Previously

[Chapter 18, Structs and arrays]({{% relref "/programming/writing-a-compiler/18-structs-and-arrays" %}}), lays out structs, unions, bitfields and packed and aligned structs in the back end, for each target. Bitfields follow the System V rule on Linux and macOS and the MSVC rule on Windows. The chapter adds field access, value semantics, copies, fixed-size arrays and indexing. It passes structs and unions by value under all three calling conventions and checks the rules with a raylib binding and the ABI probe.

## Aggregate types

[Chapter 2]({{% relref "/programming/writing-a-compiler/02-the-anti-language" %}}) defines `str` and the slice `[]T` as a pointer `ptr` followed by a length `len` of type `int`. A `str` holds valid UTF-8 without a NUL byte, and a NUL follows its last byte. Lowering treats both types as aggregates of chapter 18, so their copies, parameters and results use the code of that chapter.

```c
/* A str, a slice and a bound function are aggregates of two words. */
static bool is_aggregate(const struct type *t)
{
    return type_has_fields(t) || t->kind == TYPE_ARRAY ||
           t->kind == TYPE_STR || t->kind == TYPE_SLICE ||
           (t->kind == TYPE_FN && t->bound);
}
```

The type table of [chapter 7]({{% relref "/programming/writing-a-compiler/07-intermediate-representation" %}}) holds one entry for each aggregate type that a module uses. The function `agg_of` enters a `str` or a slice as a struct of two fields: `ptr` of IR type `ptr` and `len` of IR type `i64`. Its branches for structs and arrays belong to chapter 18.

```c
    } else {
        fields[0].name = "ptr";
        fields[0].type = ir_scalar(IR_PTR);
        fields[1].name = "len";
        fields[1].type = ir_scalar(IR_I64);
        agg = ir_struct_add(l->m, IR_AGG_STRUCT, name, fields, 2, false, 0);
    }
```

An entry records the name of its type, and `agg_of` returns the existing entry when it finds that name in the table. The types `str` and `[]byte` are therefore two entries with equal fields. The IR prints each entry as a `type` line before the declarations, as in the IR of `tests/dump/strings.anti` further below.

```text
type []byte = struct { ptr: ptr, len: i64 }
type str = struct { ptr: ptr, len: i64 }
```

## Layout and argument passing

A type table entry holds no size and no offset. The back end lays out every entry for its target by the C rules of chapter 18, before instruction selection. The function `scalar_size` in `src/layout.c` gives a pointer and an `i64` 8 bytes and alignment 8 on all six targets. On every target `ptr` therefore lies at offset 0 and `len` at offset 8, and a `str` or a slice has 16 bytes.

A convention sees a `str` as a struct of a pointer and a 64-bit integer. System V passes it in two integer registers, because both eightbytes have class INTEGER[^1]. AAPCS64 passes it in two x registers[^2]. Windows x64 passes a pointer to a copy, because 16 bytes is none of the sizes 1, 2, 4 and 8[^3].

## Fields, indexing and slicing

The field `.ptr` is the first field of the aggregate and `.len` the second. Their access reuses `field_address` of chapter 18 with the offset from `field_offset`. C places the first field of a struct at offset 0 on every target, as the comment of `field_offset` states, so `.ptr` needs no offset. The offset of `.len` is the symbolic value `offset_of str.len`, or `offset_of []byte.len` for a byte slice.

```c
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
```

An element of a `str` or a slice lies after the pointer that the value holds. An element of an array lies after the array's own address. The function `first_element` returns the address of element 0 for all four kinds of base. Indexing and slicing both start from it.

```c
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
```

A slice `a[lo..hi]` points `lo` elements after element 0 and holds `hi - lo` elements. The base, `lo` and `hi` are evaluated in that order. The pointer moves by `lo` times the stride, the symbolic value `size_of` of the element type. A slice of a `str` has type `[]byte`, and `byte` has the IR type `i8`, so its stride is `size_of i8`. No bounds check happens, as chapter 2 specifies.

```c
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
```

The test program `tests/dump/strings.anti` slices a `str` in `tail` and indexes the result in `main`.

```anti
extern fn puts(s: *byte) -> i32;

fn tail(s: str, from: int) -> []byte
{
    return s[from..s.len];
}

fn main() -> int
{
    let t = tail("hello", 1);
    puts("hi".ptr);
    return t[0] as int;
}
```

After optimization, `tail` loads `ptr` from offset 0 of its parameter `%0` and `len` from `offset_of str.len`. The pointer of the result is `ptr + from * size_of i8`, and its length is `len - from`. The optimizer of chapter 10 keeps the multiplication, because a symbolic value is not a constant for any of its passes. In `main` the index of `t[0]` is 0, so the product is 0 for every size, and `%11` adds 0.

```text
type []byte = struct { ptr: ptr, len: i64 }
type str = struct { ptr: ptr, len: i64 }
extern fn puts(ptr) -> i32
global strings.0 size 6 align 1 bytes 68 65 6c 6c 6f 00
global strings.1 size 3 align 1 bytes 68 69 00
fn strings.tail(%0: agg str, %1: i64) -> agg []byte {
b0:
    %2 = slot []byte
    %3 = load ptr %0
    %4 = ptradd %0, offset_of str.len
    %5 = load i64 %4
    %6 = mul i64 %1, size_of i8
    %7 = ptradd %3, %6
    store ptr %7, %2
    %8 = sub i64 %5, %1
    %9 = ptradd %2, offset_of []byte.len
    store i64 %8, %9
    ret ptr %2
}
fn strings.main() -> i64 {
b0:
    %0 = slot []byte
    %1 = slot str
    %2 = slot str
    %3 = addr @strings.0
    store ptr %3, %1
    %4 = ptradd %1, offset_of str.len
    store i64 5, %4
    %5 = call agg @strings.tail(%1, 1)
    memcopy %0, %5, []byte
    %6 = addr @strings.1
    store ptr %6, %2
    %7 = ptradd %2, offset_of str.len
    store i64 2, %7
    %8 = load ptr %2
    %9 = call i32 @puts(%8)
    %10 = load ptr %0
    %11 = ptradd %10, 0
    %12 = load i8 %11
    %13 = zext i64 %12
    ret i64 %13
}
```

A slice literal `[]T { ptr: p, len: n }` stores its two fields in source order. The element `t[0]` in `main` loads the pointer from the slot `%0` of `t`, adds the offset 0 with `ptradd` and loads the byte.

## Literals in read-only data

A string literal needs bytes that live as long as the program. Lowering puts the bytes of each literal and one NUL byte into a global of the IR of chapter 7. A `str` literal then becomes its address and its length, which does not count the NUL. The literal `"hello"` is the global `strings.0` of 6 bytes, and `"hi"` is `strings.1`.

```c
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
```

A byte string `b"..."` has type `[]byte` and takes the same path. Its bytes may include 0, and chapter 2 makes a write through its slice undefined behaviour. Equal bytes give one global, so `"hi"` and `b"hi"` share storage. A constant of type `str` is a global of its own, of two fields, that holds the address of the literal and its length. Chapter 18 covers the constants.

The name of a global is its index among the globals of its module. Chapter 9 names the symbol of a function `module.name`, and an identifier never starts with a digit, so `strings.0` cannot name a function. On COFF the name follows the `_A` form of chapter 9, as `_A7strings_0`. A library file carries its globals with their module names, so the literals of an imported module keep their own numbers.

The search in `literal_global` skips the globals of other modules. The unit test `keeps_literals` in `tests/unit/test_modules.c` loads a library `words` whose function returns `"hi"`, and its `main` module uses `"hi"` too. The IR of the program holds two globals with equal bytes.

```text
global words.0 size 3 align 1 bytes 68 69 00
global main.0 size 3 align 1 bytes 68 69 00
```

## Data sections

The emitter writes the globals after the functions. Each global is a label, the alignment when it exceeds 1 byte, and lines of at most 16 bytes. Data that holds no address goes to the read-only section of the object format.

```c
/* DESIGN: global data holds the bytes of literals, which no program writes
   to, so it goes to the read-only data section of each object format. */
static const char *const data_sections[] = {
    [FORMAT_ELF] = ".rodata",
    [FORMAT_MACHO] = "__TEXT,__const",
    [FORMAT_COFF] = ".rdata,\"dr\"",
};
```

```c
static void emit_data(struct text *out, enum target t,
                      const struct ir_module *m)
{
    enum object_format format = target_info(t)->format;
    bool relocated = false;
    bool written = false;
    size_t i;

    text_appendf(out, "    .section %s\n", data_sections[format]);
    for (i = 0; i < m->global_count; i++) {
        if (m->globals[i]->is_extern) {
            continue;
        }
        if (m->globals[i]->mutable) {
            written = true;
            continue;
        }
        if (m->globals[i]->reloc_count > 0) {
            relocated = true;
            continue;
        }
        emit_global(out, t, m, m->globals[i]);
    }
    if (written) {
        text_appendf(out, "    .section %s\n", mutable_sections[format]);
        for (i = 0; i < m->global_count; i++) {
            if (m->globals[i]->mutable && !m->globals[i]->is_extern) {
                emit_global(out, t, m, m->globals[i]);
            }
        }
    }
    if (!relocated) {
        return;
    }
    text_appendf(out, "    .section %s\n", reloc_sections[format]);
    for (i = 0; i < m->global_count; i++) {
        if (!m->globals[i]->mutable && m->globals[i]->reloc_count > 0 &&
            !m->globals[i]->is_extern) {
            emit_global(out, t, m, m->globals[i]);
        }
    }
}
```

A global that holds an address goes to a second section. The loader writes that address when the program starts, so a section that is read-only from the first page cannot hold it. Each format names its own: `.data.rel.ro` on ELF, `__DATA,__const` on Mach-O and `.rdata` on COFF, which the PE loader writes before it protects the pages.

```c
/* DESIGN: data that holds an address is written once when the program
   loads, so a section mapped read-only from the start cannot hold it.
   Every format has a section for data the loader writes and then
   protects, and PE protects .rdata after its base relocations. */
static const char *const reloc_sections[] = {
    [FORMAT_ELF] = ".data.rel.ro",
    [FORMAT_MACHO] = "__DATA,__const",
    [FORMAT_COFF] = ".rdata,\"dr\"",
};
```

The eight bytes of an address are left to the linker. The emitter writes `.quad` with the symbol of the global it names, and `emit_global` skips the bytes it covers.

```c
/* The symbol an address at offset names, written into out. It is a
   global, or the function of a table entry, which may be an extern of
   the runtime and then carries no module. Returns false when no
   relocation sits at that offset. */
static bool reloc_at(struct text *out, enum target t,
                     const struct ir_module *m, const struct ir_global *g,
                     uint64_t offset)
{
    size_t i;

    for (i = 0; i < g->reloc_count; i++) {
        if (g->relocs[i].offset != offset) {
            continue;
        }
        if (g->relocs[i].fn) {
            mach_function_symbol(out, t, m->functions[g->relocs[i].global]);
        } else if (m->globals[g->relocs[i].global]->exported) {
            c_symbol(out, t, m->globals[g->relocs[i].global]->name);
        } else {
            mangle(out, t, m->globals[g->relocs[i].global]->module,
                   m->globals[g->relocs[i].global]->name);
        }
        return true;
    }
    return false;
}
```

A global with a relocation, a pointer to another global inside its bytes, stops emission with an error. The IR of chapter 7 allows it, and no Anti expression produces it, because a constant holds no address except `null`.

The back end folds `offset_of str.len` to 8 and `size_of i8` to 1 before it selects instructions. It then optimizes each function that held a symbolic value once more, as chapter 12 describes. In `tail` the multiplication by 1 becomes a copy, so the assembly adds `from` to the pointer directly. On linux-x86_64 `movq 8(%rax), %rax` loads `len` from offset 8. The instruction `leaq strings.0(%rip), %rcx` loads the address of the literal, and the section `.rodata` holds its bytes.

```text
    .text
    .globl anti.rt.main
    .set anti.rt.main, strings.main
strings.tail:
.Lstrings.tail.b0:
    pushq %rbp
    movq %rsp, %rbp
    subq $32, %rsp
    leaq (%rsp), %rax
    movq %rdi, (%rax)
    movq %rsi, 8(%rax)
    leaq 16(%rsp), %rcx
    movq (%rax), %rsi
    movq 8(%rax), %rax
    addq %rdx, %rsi
    movq %rsi, (%rcx)
    subq %rdx, %rax
    movq %rax, 8(%rcx)
    movq (%rcx), %rax
    movq 8(%rcx), %rdx
    movq %rbp, %rsp
    popq %rbp
    ret
strings.main:
.Lstrings.main.b0:
    pushq %rbp
    movq %rsp, %rbp
    subq $80, %rsp
    movq %rbx, 72(%rsp)
    movq %r12, 64(%rsp)
    leaq (%rsp), %rbx
    leaq 16(%rsp), %rax
    leaq 32(%rsp), %r12
    leaq strings.0(%rip), %rcx
    movq %rcx, (%rax)
    movq $5, 8(%rax)
    movq (%rax), %rdi
    movq 8(%rax), %rsi
    movq $1, %rdx
    call strings.tail
    leaq 48(%rsp), %rcx
    movq %rax, (%rcx)
    movq %rdx, 8(%rcx)
    movq (%rcx), %rax
    movq %rax, (%rbx)
    movq 8(%rcx), %rax
    movq %rax, 8(%rbx)
    leaq strings.1(%rip), %rax
    movq %rax, (%r12)
    movq $2, 8(%r12)
    movq (%r12), %rdi
    call puts
    movq (%rbx), %rax
    movb (%rax), %al
    movzbq %al, %rax
    movq 72(%rsp), %rbx
    movq 64(%rsp), %r12
    movq %rbp, %rsp
    popq %rbp
    ret
    .section .rodata
strings.0:
    .byte 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x00
strings.1:
    .byte 0x68, 0x69, 0x00
    .section .note.GNU-stack,"",@progbits
```

On macos-arm64 the address takes `adrp` and `add` with `@PAGE` and `@PAGEOFF`, the pair that chapter 16 introduced for functions. The bytes go to `__TEXT,__const`.

```text
    adrp x10, _strings.0@PAGE
    add x10, x10, _strings.0@PAGEOFF
```

```text
    .section __TEXT,__const
_strings.0:
    .byte 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x00
_strings.1:
    .byte 0x68, 0x69, 0x00
```

On windows-x86_64 the section is `.rdata` with the flags `d` and `r`, for data and read-only. The call of `tail` passes a pointer to a copy of the `str` in `rdx`, after the address of the result in `rcx`.

```text
    leaq _A7strings_0(%rip), %rcx
    movq %rcx, (%rax)
    movq $5, 8(%rax)
    leaq 80(%rsp), %rdi
    leaq 96(%rsp), %rdx
    movq (%rax), %rcx
    movq %rcx, (%rdx)
    movq 8(%rax), %rax
    movq %rax, 8(%rdx)
    movq $1, %r8
    movq %rdi, %rcx
    call _A7strings_tail
```

## Calling C with s.ptr

The NUL after the bytes of a `str` makes `s.ptr` a C string. A slice of a `str` ends at `hi`, where no NUL has to follow, so C needs its length. For `printf` the conversion `%.*s` takes an `int` precision, then the pointer. With a precision, `printf` writes no more than that many bytes, and the array needs no NUL[^4]. The test program `tests/programs/strings.anti` does both, and it fills slices of an array and of heap memory.

```anti
extern fn puts(s: *byte) -> i32;
extern fn printf(format: *byte, ...) -> i32;

const WORD: str = "banana";

fn count(s: []byte, b: byte) -> int
{
    let n = 0;
    let i = 0;
    while i < s.len do {
        if s[i] == b {
            n += 1;
        }
        i += 1;
    }
    return n;
}

fn fill(s: []int, v: int)
{
    let i = 0;
    while i < s.len do {
        s[i] = v + i;
        i += 1;
    }
}

fn main() -> int
{
    puts(WORD.ptr);
    let tail = WORD[2..6];
    printf("%.*s %d\n".ptr, tail.len as i32, tail.ptr, count(tail, 97) as i32);

    let raw = b"raw\x00bytes\xff";
    printf("%d %d\n".ptr, raw.len as i32, count(raw, 0) as i32);

    let numbers = [0; 6];
    fill(numbers[2..5], 10);
    printf("%d %d %d %d\n".ptr, numbers[1] as i32, numbers[2] as i32,
        numbers[4] as i32, numbers[5] as i32);

    let heap = alloc(int, 3);
    let view = []int { ptr: heap, len: 3 };
    fill(view, 100);
    let sum = heap[0] + heap[1] + heap[2];
    free(heap);
    return sum % 256;
}
```

The program prints four lines and exits with 47, the sum 100 + 101 + 102 modulo 256. The byte string holds 10 bytes, one of them 0. The function `fill` wrote 10, 11 and 12 into `numbers[2..5]` and left `numbers[1]` and `numbers[5]` at 0.

```text
exit 47
banana
nana 2
10 1
0 10 12 0
```

## UTF-8 and char

A `char` holds one Unicode scalar value, and a `str` holds its characters as UTF-8 bytes. UTF-8 writes a scalar value below 0x80 as one byte, below 0x800 as two, below 0x10000 as three and above as four[^5]. The first byte of a well-formed sequence gives its length: `00` to `7F` start one byte, `C2` to `DF` two, `E0` to `EF` three and `F0` to `F4` four[^5]. Each following byte carries 6 bits of the value in the range `80` to `BF`[^5].

A `str` holds valid UTF-8 by the rules of chapter 2, so a decoder over a `str` needs no error handling. The test program `tests/programs/utf8.anti` decodes a literal with one character of each length and compares a `char` with a character literal.

```anti
extern fn printf(format: *byte, ...) -> i32;

struct Decoded
{
    c: char,
    size: int,
}

// The bits of continuation byte i of s.
fn low6(s: str, i: int) -> u32
{
    return s[i] as u32 & 63;
}

// The scalar value that starts at byte i of s. A str holds valid UTF-8,
// so the first byte gives the length of the sequence.
fn decode(s: str, i: int) -> Decoded
{
    let b = s[i] as u32;
    if b < 128 {
        return Decoded { c: b as char, size: 1 };
    }
    if b < 224 {
        return Decoded { c: ((b & 31) << 6 | low6(s, i + 1)) as char, size: 2 };
    }
    if b < 240 {
        let v = (b & 15) << 12 | low6(s, i + 1) << 6 | low6(s, i + 2);
        return Decoded { c: v as char, size: 3 };
    }
    let v = (b & 7) << 18 | low6(s, i + 1) << 12 | low6(s, i + 2) << 6 |
        low6(s, i + 3);
    return Decoded { c: v as char, size: 4 };
}

fn main() -> int
{
    let s = "añ€😀";
    let escaped = "a\u{F1}\u{20AC}\u{1F600}";
    printf("%d bytes, equal lengths %d\n".ptr, s.len as i32,
        (s.len == escaped.len) as i32);
    let count = 0;
    let i = 0;
    while i < s.len do {
        let d = decode(s, i);
        printf("U+%04X in %d bytes\n".ptr, d.c as u32, d.size as i32);
        if d.c == '€' {
            printf("euro at byte %d\n".ptr, i as i32);
        }
        i += d.size;
        count += 1;
    }
    return count;
}
```

The literal `"añ€😀"` has 10 bytes. The escapes `\u{F1}`, `\u{20AC}` and `\u{1F600}` produce the same bytes, which the lexer of chapter 4 encodes at compile time. The program exits with 4, the number of characters.

```text
exit 4
10 bytes, equal lengths 1
U+0061 in 1 bytes
U+00F1 in 2 bytes
U+20AC in 3 bytes
euro at byte 3
U+1F600 in 4 bytes
```

## Arguments and environment

A `main` may take `args: []str` and `env: []str`, as chapter 2 specifies. The runtime file `rt/start.c` builds both slices and calls `anti.rt.main`, the runtime entry symbol of chapter 16. The assembly of the main module defines that symbol as a second name of its `main`, as the `.set` line in the listings above shows. The runtime passes both slices to every `main`. All six conventions pass the two slices in registers, or pointers to their copies in registers, so a `main` with fewer parameters ignores the extra registers.

The runtime library `libanti_rt.a`, named `anti_rt.lib` on Windows, holds the objects of `rt/start.c`, `rt/init.c` and `rt/utf.c`. Before it declares the entry, `rt/start.c` defines C structs with the layout of `str` and `[]str`.

```c
/* The layouts of Anti's str and []str. */
struct anti_str {
    const unsigned char *ptr;
    int64_t len;
};

struct anti_slice {
    struct anti_str *ptr;
    int64_t len;
};

/* DESIGN: the entry always passes args and env. All six calling
   conventions pass both slices, or pointers to them, in registers. A main
   with fewer parameters ignores those registers. */

/* A symbol with a dot is not a C identifier, so the declaration names it
   with an assembler label. Mach-O adds '_' to C symbols, ELF does not.
   The COFF symbol is an identifier, and MSVC has no assembler labels. */
#if defined(_WIN32)
extern int64_t _A4anti2rt_main(struct anti_slice args, struct anti_slice env);
#define anti_main _A4anti2rt_main
#else
#if defined(__APPLE__)
#define ANTI_ENTRY_SYMBOL "_anti.rt.main"
#else
#define ANTI_ENTRY_SYMBOL "anti.rt.main"
#endif
extern int64_t anti_main(struct anti_slice args, struct anti_slice env)
    __asm__(ANTI_ENTRY_SYMBOL);
#endif
```

On Linux and macOS the arguments come from `argv` and the environment from the POSIX variable `environ`[^6]. An environment value is an arbitrary sequence of bytes except the null byte[^6]. A `str` must hold valid UTF-8, so the runtime copies each entry through `anti_utf8_repair`. It replaces each ill-formed part with U+FFFD, the replacement character, and adds a NUL. The C `main` calls `anti_rt_init` of `rt/init.c` first, which initialises the state of the runtime before any Anti code runs.

```c
extern char **environ;

/* Byte strings from the system as str values of valid UTF-8. */
static struct anti_slice strings(char **list, size_t count)
{
    struct anti_slice slice;
    size_t i;

    slice.ptr = allocate(count * sizeof *slice.ptr);
    slice.len = (int64_t)count;
    for (i = 0; i < count; i++) {
        size_t n = strlen(list[i]);
        unsigned char *bytes = allocate(3 * n + 1);
        slice.ptr[i] = make_str(
            bytes, anti_utf8_repair((const unsigned char *)list[i], n, bytes));
    }
    return slice;
}

int main(int argc, char **argv)
{
    size_t count = 0;

    anti_rt_init();
    while (environ != NULL && environ[count] != NULL) {
        count++;
    }
    return (int)anti_main(strings(argv, (size_t)argc),
                          strings(environ, count));
}
```

The Unicode Standard allows several ways to replace an ill-formed sequence[^5]. A converter must not consume a byte that begins a well-formed sequence, so `C2 41 42` becomes U+FFFD, `A` and `B`[^5]. Section 3.9.6 describes the practice of the W3C Encoding standard, which replaces each maximal subpart with one U+FFFD[^5]. A maximal subpart is the longest prefix of a well-formed sequence at that offset, or else a single byte[^5]. The sequence `E1 80 E2 F0 91 92 F1 BF 41` has four maximal subparts before the `A`[^5]. In `anti_utf8_repair`, the helper `sequence_length` gives the length that a first byte announces and the range of the second byte from table 3-7. The helper `put_scalar` writes a scalar value as UTF-8.

```c
/* DESIGN: each maximal subpart of an ill-formed sequence becomes one
   U+FFFD, the practice of section 3.9.6 of the Unicode Standard. out holds
   3 * n bytes. */
size_t anti_utf8_repair(const unsigned char *in, size_t n, unsigned char *out)
{
    size_t written = 0;
    size_t i = 0;

    while (i < n) {
        unsigned char low;
        unsigned char high;
        size_t length = sequence_length(in[i], &low, &high);
        size_t good = length == 0 ? 0 : 1;
        while (good > 0 && good < length && i + good < n &&
               in[i + good] >= (good == 1 ? low : 0x80) &&
               in[i + good] <= (good == 1 ? high : 0xBF)) {
            good++;
        }
        if (length > 0 && good == length) {
            for (good = 0; good < length; good++) {
                out[written++] = in[i + good];
            }
            i += length;
        } else {
            written += put_scalar(0xFFFD, out + written);
            i += good == 0 ? 1 : good;
        }
    }
    return written;
}
```

The unit tests in `tests/unit/test_utf.c` check the byte sequences of tables 3-8 to 3-11 of the standard, which cover non-shortest forms, surrogates, values above U+10FFFF and truncated sequences.

## Windows entry

The function `GetCommandLineW` returns the command line of the process as one UTF-16 string[^7]. The environment comes from `GetEnvironmentStringsW` as a block of `name=value` strings, each ended by a 0 unit, with a second 0 unit after the last[^8]. UTF-16 writes a scalar value above U+FFFF as a pair of surrogates, and a surrogate outside a pair is ill-formed[^5]. The function `anti_utf16_to_utf8` joins pairs and replaces any other surrogate with U+FFFD. The Windows `main` calls `anti_rt_init` first as well, then builds `args` and `env` from these two functions.

The Microsoft C startup code splits the command line by documented rules[^9]. Spaces and tabs separate arguments. The program name ends at a space or tab outside double quotes and keeps its backslashes[^9]. In the other arguments, quotes group text, and backslashes are literal unless they precede a quote. Then each pair of backslashes gives one backslash, and an odd one makes the quote literal[^9]. A pair of quotes inside a quoted part gives one quote[^9].

```c
/* DESIGN: the rules of the Microsoft C startup code for command-line
   arguments. The program name ends at a blank outside quotes and keeps its
   backslashes. out receives each argument followed by a 0 unit and holds
   2 * n + 2 units for a line of n units. Returns the number of arguments. */
size_t anti_split_command_line(const uint16_t *line, uint16_t *out)
{
    const uint16_t *p = line;
    size_t count = 0;
    int quoted = 0;

    while (*p != 0 && (quoted || !is_blank(*p))) {
        if (*p == '"') {
            quoted = !quoted;
        } else {
            *out++ = *p;
        }
        p++;
    }
    *out++ = 0;
    count++;
    for (;;) {
        while (is_blank(*p)) {
            p++;
        }
        if (*p == 0) {
            return count;
        }
        quoted = 0;
        while (*p != 0 && (quoted || !is_blank(*p))) {
            size_t slashes = 0;
            int copy = 1;
            while (*p == '\\') {
                slashes++;
                p++;
            }
            if (*p == '"') {
                if (slashes % 2 == 0) {
                    if (quoted && p[1] == '"') {
                        p++;
                    } else {
                        copy = 0;
                        quoted = !quoted;
                    }
                }
                slashes /= 2;
            }
            while (slashes-- > 0) {
                *out++ = '\\';
            }
            if (*p == 0 || (!quoted && is_blank(*p))) {
                break;
            }
            if (copy) {
                *out++ = *p;
            }
            p++;
        }
        *out++ = 0;
        count++;
    }
}
```

The unit tests split the six examples of Microsoft's page, such as `a\\\"b c d` into `a\"b`, `c` and `d`. The Windows branch of `rt/start.c` has not run on Windows, and clang checked it only against a stub `windows.h` on the development Mac.

## Echo program

A program test may put one argument per line into a file `NAME.args` beside its source. The script `tests/run_program.cmake` passes those lines to the program. The test `program_args` also sets the environment variable `ANTI_TEST_GREETING` to `héllo`.

```anti
extern fn printf(format: *byte, ...) -> i32;

fn show(s: str)
{
    printf("[%.*s] %d\n".ptr, s.len as i32, s.ptr, s.len as i32);
}

fn starts_with(s: str, prefix: []byte) -> bool
{
    if s.len < prefix.len {
        return false;
    }
    let i = 0;
    while i < prefix.len do {
        if s[i] != prefix[i] {
            return false;
        }
        i += 1;
    }
    return true;
}

fn main(args: []str, env: []str) -> int
{
    let i = 1;
    while i < args.len do {
        show(args[i]);
        i += 1;
    }
    let j = 0;
    while j < env.len do {
        if starts_with(env[j], b"ANTI_TEST_GREETING=") {
            show(env[j]);
        }
        j += 1;
    }
    return args.len;
}
```

The arguments are `hello`, `grüße` and `two words`. The program prints each with its length in bytes and exits with 4, the length of `args`. The length 7 of `grüße` counts 2 bytes each for `ü` and `ß`.

```text
exit 4
[hello] 5
[grüße] 7
[two words] 9
[ANTI_TEST_GREETING=héllo] 25
```

## Tests

The strings, slices and byte literals of this chapter are pinned in the IR and in the assembly, and the programs run on the development Mac. The build has zero warnings, and every `ctest` test passes.

## Next

[Chapter 20, Function pointers]({{% relref "/programming/writing-a-compiler/20-function-pointers" %}}), adds function types, taking the address of a function and indirect calls in both back ends. It closes with the struct-of-function-pointers pattern.

## References

[^1]: H.J. Lu and others, *System V Application Binary Interface, AMD64 Architecture Processor Supplement*, version 1.0, March 12, 2025, section 3.2.3, https://gitlab.com/x86-psABIs/x86-64-ABI

[^2]: Arm Limited, *Procedure Call Standard for the Arm 64-bit Architecture*, release 2025Q4, section 6.8.2, https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst

[^3]: Microsoft, *x64 calling convention*, section "Parameter passing", https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention?view=msvc-170

[^4]: ISO/IEC JTC1/SC22/WG14, *N1570, Committee Draft of ISO/IEC 9899:201x*, 2011, section 7.21.6.1, https://www.open-std.org/jtc1/sc22/wg14/www/docs/n1570.pdf

[^5]: Unicode Consortium, *The Unicode Standard, Version 16.0, Core Specification*, chapter 3, sections 3.9.1 to 3.9.6 and tables 3-7 to 3-11, https://www.unicode.org/versions/Unicode16.0.0/core-spec/chapter-3/

[^6]: The Open Group, *The Open Group Base Specifications Issue 8*, IEEE Std 1003.1-2024, Base Definitions, section 8.1, https://pubs.opengroup.org/onlinepubs/9799919799/basedefs/V1_chap08.html

[^7]: Microsoft, *GetCommandLineW function (processenv.h)*, https://learn.microsoft.com/en-us/windows/win32/api/processenv/nf-processenv-getcommandlinew

[^8]: Microsoft, *GetEnvironmentStringsW function (processenv.h)*, section "Remarks", https://learn.microsoft.com/en-us/windows/win32/api/processenv/nf-processenv-getenvironmentstringsw

[^9]: Microsoft, *Parsing C command-line arguments*, https://learn.microsoft.com/en-us/cpp/c-language/parsing-c-command-line-arguments?view=msvc-170
