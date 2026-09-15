---
title: "Libraries for C"
description: "How antic builds static and shared libraries from Anti code, writes their C header, packages the runtime and tests the libraries from C programs."
summary: "Static and shared libraries built from Anti code. The export marker, the signature rule and the generated header. The runtime in both forms, symbol visibility, memory and threads across the boundary, versioning, and the tests that check them."
date: 2026-09-15T04:05:22+02:00
lastmod: 2026-09-15T04:05:22+02:00
draft: true
weight: 250
tags: [compilers, assembly]
keywords: [static library, shared library, C header generation, llvm-ar, soname, install name, relocatable link, library constructor]
---

## Previously

Chapter 24, The build tool, describes what `anti` does and how a project uses it, without its code. It covers the manifest and lock file, repositories and resolution, the module cache and the dev and release builds. It also covers `anti check`, `anti fmt`, `anti doc`, `anti bind`, libraries for C, publishing over HTTPS and `anti license`.

## Library forms

A library file `.antl` holds a module for other Anti modules. A library for C holds the machine code of a module for one target, and a C header describes it. C programs use it, and so does every language that calls C functions. A static library is an archive of object files that the linker copies into a program. A shared library is a file that the loader maps into a process at run time.

The compiler antic writes a library for C with `--lib static` or `--lib shared`, and `anti build --lib` passes the same option. Both forms come from the same source as the library file. A library for C has no `main`, because the C program owns the process. A module that defines one stops with the message `` a library for C has no function `main` ``.

The module `com.example.geo` in `tests/clib/com/example/geo.anti` is the example of this chapter. It exports two structs, a union, a constant and six functions, and it keeps the function `square` private. The function `geo_ready` returns the flag of the runtime that the C function `anti_rt_ready` reads.

```anti
//! Plane geometry for C programs.

/// A point on the plane.
export struct Vec2
{
    x: c_int,
    y: c_int,
}

/// An integer or a float in 8 bytes.
export union Num
{
    i: i64,
    d: f64,
}

/// Display flags.
export struct Flags
{
    visible: u32 : 1,
    layer: u32 : 4,
}

/// The largest layer.
export const LAYERS: int = 16;

extern fn anti_rt_ready() -> c_int;

fn square(v: c_int) -> c_int
{
    return v * v;
}

/// The dot product of a and b.
export fn geo_dot(a: Vec2, b: Vec2) -> c_int
{
    return a.x * b.x + a.y * b.y;
}

/// v with both coordinates multiplied by k.
export fn geo_scale(v: Vec2, k: c_int) -> Vec2
{
    return Vec2 { x: v.x * k, y: v.y * k };
}

/// Half of the float in n.
export fn geo_half(n: Num) -> Num
{
    return Num { d: n.d / 2.0 };
}

/// The layer of the flags at f, with visible set.
export fn geo_layer(f: *Flags) -> u32
{
    f.visible = 1;
    return f.layer;
}

/// The squared length of v, through a function pointer.
export fn geo_apply(f: fn(c_int) -> c_int, v: Vec2) -> c_int
{
    return f(v.x) + f(v.y) + square(0);
}

/// 1 once the runtime is initialised.
export fn geo_ready() -> c_int
{
    return anti_rt_ready();
}
```

## Export marker

[Chapter 2]({{% relref "/programming/writing-a-compiler/02-the-anti-language" %}}) defines `export` on `fn`, `struct`, `union` and `const`. The marker implies `pub` and puts the item into the C interface. An `export fn` gets the symbol of its own name, the rule of `extern fn`. In the macos-arm64 assembly of `geo.anti`, the function `geo_dot` is the global symbol `_geo_dot`. The private function keeps the mangled symbol `_com.example.geo.square` of [chapter 9]({{% relref "/programming/writing-a-compiler/09-modules-and-library-files" %}}) and stays local. On COFF the two symbols are `geo_dot` and `_A3com7example3geo_square`.

Two modules may not export one name, and `main` cannot be exported, because the C `main` of the runtime starts every program. [Chapter 6]({{% relref "/programming/writing-a-compiler/06-semantic-analysis" %}}) shows the checks of semantic analysis for both rules.

## Signature rule

A C header spells every type of an exported item in C. An `export fn` therefore takes and returns only types with a C representation.

- Sized integers, `int`, `uint`, `float`, `f32`, `f64`, `bool` and the `c_` types
- Pointers to such types, with `*byte` for the `void *` of C
- Exported structs and unions, by value or by pointer
- Function pointers whose parameters and result follow the rule

The types `char`, `str`, slices, arrays by value and structs without `export` are refused. The fields of an exported struct or union follow the same rule, and a field may also be a fixed-size array of such a type. An array field whose length comes from `size_of` is refused, because a C declaration needs the number. An `export const` has a numeric type, `bool` or `str`. The file `words.anti` below breaks the rule twice.

```anti
struct Pair
{
    a: c_int,
    b: c_int,
}

export fn count(text: str) -> c_int
{
    return text.len as c_int;
}

export fn first(p: Pair) -> c_int
{
    return p.a;
}
```

```text
words.anti:7:17: error: the parameter `text` of export fn `count` has type `str`, which C cannot represent
words.anti:12:17: error: the parameter `p` of export fn `first` has type `Pair`, and `Pair` is not exported
```

A string crosses the boundary as two arguments. They are the pointer and the length that [chapter 19]({{% relref "/programming/writing-a-compiler/19-strings-slices-and-bytes" %}}) gives every `str` and slice as `.ptr` and `.len`. The version below compiles.

```anti
export fn count(ptr: *byte, len: int) -> c_int
{
    let n = 0;
    let i = 0;
    while i < len do {
        if ptr[i] == 32 {
            n += 1;
        }
        i += 1;
    }
    return n as c_int;
}
```

## Driver options

The option `--lib` takes `static` or `shared`. The options `--bundle-runtime` and `--soname` modify the two forms, and `--llvm-ar` names the archiver llvm-ar when it is not on the path.

```c
        } else if (strcmp(arg, "--lib") == 0) {
            const char *kind = value_of(argc, argv, &i);
            if (kind == NULL) {
                return 2;
            }
            if (strcmp(kind, "static") == 0) {
                options.lib = LIB_STATIC;
            } else if (strcmp(kind, "shared") == 0) {
                options.lib = LIB_SHARED;
            } else {
                fprintf(stderr, "antic: --lib takes static or shared, not %s\n",
                        kind);
                return 2;
            }
            continue;
        } else if (strcmp(arg, "--bundle-runtime") == 0) {
            options.bundle_runtime = true;
            continue;
        } else if (strcmp(arg, "--soname") == 0) {
            options.soname = true;
            continue;
        } else if (strcmp(arg, "--llvm-ar") == 0) {
            slot = &options.llvm_ar;
```

A library for C needs `--runtime`. The printed link line of a static library names the runtime library, and a shared library links it in. A shared library and a bundled runtime outside Windows also run the platform linker, which antic runs only on a host with the target's operating system. The option `-S` writes the assembly file without any of these steps.

```c
    if (o->lib != LIB_NONE && !o->assembly_only &&
        (o->runtime == NULL ||
         ((o->lib == LIB_SHARED ||
           (o->bundle_runtime &&
            target_info(o->target)->format != FORMAT_COFF)) &&
          !can_link(o)))) {
        if (o->runtime == NULL) {
            fprintf(stderr, "antic: --lib needs --runtime <dir>, the directory "
                            "that holds %s/<target>/\n",
                    RUNTIME_LIB_DIR);
        }
        return 2;
    }
```

The library takes its name from the file name of `-o` without `lib` and the suffix, so `-o libgeo.a` names it `geo`. Without `-o` the last segment of the module path names it. A COFF file name keeps a leading `lib`, because MSVC gives a static library no prefix.

## Interface of the library

After semantic analysis, the function `compile` refuses a `main` and builds the interface of the module. It writes the header and the bytes of the package header into memory, before the back end runs. The function `header_write` receives the interfaces of the loaded libraries and the module's own, so an exported item of an imported module reaches the header too.

```c
    if (o->lib != LIB_NONE && defines_main(tree)) {
        fprintf(stderr, "antic: %s: a library for C has no function `main`\n",
                o->input);
        goto done;
    }
    if (o->lib != LIB_NONE || links(o)) {
        struct interface own;
        const struct interface **all =
            malloc((paths.count + 2) * sizeof *all);
        if (all == NULL) {
            fputs("antic: out of memory\n", stderr);
            exit(70);
        }
        if (!own_interface(o, tree, text_cstr(module), &arena, &own) ||
            !build_notice(o, &own, libraries, paths.count, &arena,
                          &extras->notice)) {
            free((void *)all);
            goto done;
        }
        if (o->lib != LIB_NONE) {
            memcpy((void *)all, (void *)libraries,
                   paths.count * sizeof *all);
            all[paths.count] = &own;
            library_name(o, text_cstr(module), &extras->name);
            header_write(&extras->header, text_cstr(&extras->name), all,
                         paths.count + 1, o->bundle_runtime);
            antl_write_header(&extras->package, &own);
        }
        free((void *)all);
    }
    status = back_end(o, tree, text_cstr(module), &program, &diags, assembly,
                      extras);
```

The function `own_interface` gives the interface the package header of the options `--package-name`, `--package-version`, `--dependency`, `--license`, `--license-text` and `--attribution`. A library file of the module holds the same header, as [chapter 9]({{% relref "/programming/writing-a-compiler/09-modules-and-library-files" %}}) describes. The function `antl_write_header` writes the start of that library file, up to the imports and the doc text of the module.

```c
/* The interface of the compiled module with the package header of the
   options, as a library file of the module holds it. */
static bool own_interface(const struct options *o, struct module *tree,
                          const char *module, struct arena *arena,
                          struct interface *out)
{
    struct package package;

    if (!package_header(o, arena, &package)) {
        return false;
    }
    sema_interface(tree, module, arena, out);
    if (package.name != NULL) {
        out->package.name = package.name;
    }
    if (package.version != NULL) {
        out->package.version = package.version;
    }
    out->package.dependencies = package.dependencies;
    out->package.dependency_count = package.dependency_count;
    if (package.license != NULL) {
        out->package.license = package.license;
    }
    if (package.license_text != NULL) {
        out->package.license_text = package.license_text;
    }
    out->package.attribution = package.attribution;
    out->package.attribution_count = package.attribution_count;
    return true;
}
```

The function `build_notice` collects the licence notice of a linked binary. It lists the runtime as the package `anti.rt` with the antic version and 0BSD, then every package of the loaded libraries once, then the module's own package. The function `notice_text` of chapter 16 writes each package name once, so a module of a loaded package adds no second line. A static library carries no notice, and the back end skips it there.

```c
/* The licence notice of a linked binary: the runtime, every package of
   the loaded libraries once, and the package of the compiled module. */
static bool build_notice(const struct options *o, const struct interface *own,
                         const struct interface *const *libraries,
                         size_t count, struct arena *arena, struct text *out)
{
    const struct package **list = calloc(count + 3, sizeof *list);
    struct package runtime;
    struct text path = {0};
    struct text text = {0};
    size_t n = 0;
    size_t i;
    size_t j;

    if (list == NULL) {
        fputs("antic: out of memory\n", stderr);
        exit(70);
    }
    memset(&runtime, 0, sizeof runtime);
    runtime.name = RUNTIME_MODULE;
    runtime.version = ANTIC_VERSION;
    runtime.license = "0BSD";
    runtime.license_text = "";
    text_appendf(&path, "%s/licenses/anti_rt.txt",
                 o->runtime != NULL ? o->runtime : ".");
    if (o->runtime != NULL && read_source(text_cstr(&path), &text)) {
        char *copy = arena_alloc(arena, text.length + 1);
        memcpy(copy, text_cstr(&text), text.length);
        runtime.license_text = copy;
    }
    list[n++] = &runtime;
    for (i = 0; i < count; i++) {
        for (j = 0; j < n; j++) {
            if (strcmp(list[j]->name, libraries[i]->package.name) == 0) {
                break;
            }
        }
        if (j == n) {
            list[n++] = &libraries[i]->package;
        }
    }
    list[n++] = &own->package;
    notice_text(out, list, n);
    free((void *)list);
    text_free(&path);
    text_free(&text);
    return true;
}
```

## Generated header

The command below builds the static library of the example in a directory that holds links to the repository's `build` and `tests` directories. It writes `libgeo.a` and `geo.h` and prints the link line that the section on the static archive shows.

```sh
build/antic --lib static --llvm-mc build/toolchain/bin/llvm-mc \
    --llvm-ar build/toolchain/bin/llvm-ar --runtime build/runtime \
    -I tests/clib -o libgeo.a tests/clib/com/example/geo.anti
```

The header `geo.h` holds, in this order, a top comment, an include guard, the three standard headers and the C++ linkage block. The exported aggregates, constants and prototypes follow, each with its `///` comment as a `/** */` comment.

```c
/* geo.h, the C interface of com.example.geo, written by antic.
   Do not edit. A failure that Anti cannot report calls abort(). */
#ifndef GEO_H
#define GEO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#define ANTI_ALIGNAS(n) alignas(n)
extern "C" {
#else
#define ANTI_ALIGNAS(n) _Alignas(n)
#endif

/** A point on the plane. */
typedef struct Vec2 {
    int32_t x;
    int32_t y;
} Vec2;

/** An integer or a float in 8 bytes. */
typedef union Num {
    int64_t i;
    double d;
} Num;

/** Display flags. */
typedef struct Flags {
    uint32_t visible : 1;
    uint32_t layer : 4;
} Flags;

/** The largest layer. */
#define LAYERS ((int64_t)16)

/** The dot product of a and b. */
int32_t geo_dot(Vec2 a, Vec2 b);
/** v with both coordinates multiplied by k. */
Vec2 geo_scale(Vec2 v, int32_t k);
/** Half of the float in n. */
Num geo_half(Num n);
/** The layer of the flags at f, with visible set. */
uint32_t geo_layer(Flags *f);
/** The squared length of v, through a function pointer. */
int32_t geo_apply(int32_t (*f)(int32_t), Vec2 v);
/** 1 once the runtime is initialised. */
int32_t geo_ready(void);

#ifdef __cplusplus
}
#endif

#endif
```

### Type mapping

Each scalar type maps to a fixed C name. A sized type takes its `<stdint.h>` name, `c_int` included, because the fixed-size `c_` types are the sized types. The types `c_long`, `c_ulong` and `c_wchar` differ in width between targets, so they map to the C types of the same meaning.

```c
/* The C name of a scalar type. DESIGN: a sized type maps to its <stdint.h>
   name, and int, uint and float to int64_t, uint64_t and double. c_long,
   c_ulong and c_wchar map to long, unsigned long and wchar_t. The other c_
   types are sized types, so c_int is int32_t. */
static const char *scalar_name(const struct type *t)
{
    switch (t->kind) {
    case TYPE_VOID: return "void";
    case TYPE_BOOL: return "bool";
    case TYPE_I8: return "int8_t";
    case TYPE_I16: return "int16_t";
    case TYPE_I32: return "int32_t";
    case TYPE_I64: return "int64_t";
    case TYPE_U8: return "uint8_t";
    case TYPE_U16: return "uint16_t";
    case TYPE_U32: return "uint32_t";
    case TYPE_U64: return "uint64_t";
    case TYPE_F32: return "float";
    case TYPE_F64: return "double";
    case TYPE_CLONG: return "long";
    case TYPE_CULONG: return "unsigned long";
    case TYPE_CWCHAR: return "wchar_t";
    default: return "void";
    }
}
```

| Anti | C |
|---|---|
| `i8` to `i64`, `u8` to `u64` | `int8_t` to `int64_t`, `uint8_t` to `uint64_t` |
| `int`, `uint` | `int64_t`, `uint64_t` |
| `float` or `f64`, `f32` | `double`, `float` |
| `c_long`, `c_ulong`, `c_wchar` | `long`, `unsigned long`, `wchar_t` |
| `*T` | a pointer to the C type of `T`, `uint8_t *` for `*byte` |
| `[N]T` as a field | an array of `N` elements |
| `fn(A) -> R` | a pointer to a function |
| `export struct S`, `export union S` | the typedef `S` |

The function `declaration` builds a C declarator from the inside out. A pointer puts `*` before the name, an array puts `[N]` after it, and a function type wraps it in `(*name)(parameters)`. The element or result type then declares the result. Inside its own definition an aggregate refers to itself with its tag, as `struct Holder *next`.

```c
/* Append the C declaration of name with type t. owner is the aggregate
   whose definition holds the declaration, which names itself with its
   tag. */
static void declaration(struct text *out, const struct type *t,
                        const char *name, const struct type *owner)
{
    struct text inner = {0};
    size_t i;

    switch (t->kind) {
    case TYPE_POINTER:
        text_appendf(&inner, "*%s", name);
        declaration(out, t->element, text_cstr(&inner), owner);
        break;
    case TYPE_ARRAY:
        text_appendf(&inner, "%s[%" PRIu64 "]", name, t->length);
        declaration(out, t->element, text_cstr(&inner), owner);
        break;
    case TYPE_FN:
        /* An Anti function type is a function pointer in C. */
        text_appendf(&inner, "(*%s)(", name);
        for (i = 0; i < t->param_count; i++) {
            struct text param = {0};
            declaration(&param, t->params[i], "", owner);
            text_appendf(&inner, "%s%s", i > 0 ? ", " : "", text_cstr(&param));
            text_free(&param);
        }
        text_append(&inner, t->param_count == 0 ? "void)" : ")");
        declaration(out, t->result, text_cstr(&inner), owner);
        break;
    case TYPE_STRUCT:
        if (t == owner) {
            text_appendf(out, "%s %.*s", t->is_union ? "union" : "struct",
                         (int)t->name.length, t->name.text);
        } else {
            text_appendf(out, "%.*s", (int)t->name.length, t->name.text);
        }
        text_appendf(out, "%s%s", name[0] != '\0' ? " " : "", name);
        break;
    default:
        text_append(out, scalar_name(t));
        text_appendf(out, "%s%s", name[0] != '\0' ? " " : "", name);
        break;
    }
    text_free(&inner);
}
```

### Aggregates

Every exported struct and union becomes a typedef of the same name. The function `aggregate` first writes the exported aggregates that a field holds by value, so C sees each type before its use. A bitfield keeps its width after a colon, and a zero-width bitfield `_: T : 0` becomes the unnamed C field `T : 0`. The back end of [chapter 18]({{% relref "/programming/writing-a-compiler/18-structs-and-arrays" %}}) lays out a struct by the C rules of the target. The C compiler therefore computes the same offsets from this definition.

```c
/* DESIGN: an export struct or union becomes a typedef of the same name.
   packed becomes #pragma pack and align(N) an _Alignas on the first
   field, which C++ spells alignas. */
static void aggregate(struct text *out, const struct symbol *sym,
                      const struct interface *const *ifaces, size_t count,
                      struct emitted *done)
{
    const struct type *t = sym->type;
    const char *kind = t->is_union ? "union" : "struct";
    size_t i;

    if (was_emitted(done, t)) {
        return;
    }
    done->items[done->count++] = t;
    for (i = 0; i < t->field_count; i++) {
        emit_uses(out, t->fields[i].type, ifaces, count, done);
    }
    doc_comment(out, &sym->doc, "");
    if (t->packed) {
        text_append(out, "#pragma pack(push, 1)\n");
    }
    text_appendf(out, "typedef %s %.*s {\n", kind, (int)t->name.length,
                 t->name.text);
    for (i = 0; i < t->field_count; i++) {
        struct text field = {0};
        char buffer[128];
        c_name(buffer, sizeof buffer, &t->fields[i].name);
        doc_comment(out, &t->fields[i].doc, "    ");
        text_append(out, "    ");
        if (i == 0 && t->align != 0) {
            text_appendf(out, "ANTI_ALIGNAS(%" PRIu64 ") ", t->align);
        }
        if (type_field_is_unit_break(&t->fields[i])) {
            buffer[0] = '\0';
        }
        declaration(&field, t->fields[i].type, buffer, t);
        text_append(out, text_cstr(&field));
        if (t->fields[i].bits != 0 || type_field_is_unit_break(&t->fields[i])) {
            text_appendf(out, " : %u", (unsigned)t->fields[i].bits);
        }
        text_append(out, ";\n");
        text_free(&field);
    }
    text_appendf(out, "} %.*s;\n", (int)t->name.length, t->name.text);
    if (t->packed) {
        text_append(out, "#pragma pack(pop)\n");
    }
    text_append(out, "\n");
}
```

A `packed` aggregate stands between `#pragma pack(push, 1)` and `#pragma pack(pop)`. GCC supports these pragmas "for compatibility with Microsoft Windows compilers", and they set the maximum alignment of the members of later structures and unions[^1]. An `align(N)` becomes `ANTI_ALIGNAS(N)` on the first field, whose offset is 0 on every target. C allows no alignment specifier on a bitfield, so semantic analysis refuses an aligned export aggregate whose first field is a bitfield. The header defines that macro as `_Alignas(n)` in C, the alignment specifier of C11[^2]. In C++ it is `alignas(n)`, which may be applied to a class data member[^3].

The module below shows the three forms with an array field, a function pointer field and a pointer to the aggregate itself.

```anti
/// An integer or a float in 8 bytes.
export union Num
{
    i: i64,
    d: f64,
}

/// Five bytes without padding.
export packed struct Tight
{
    tag: u8,
    value: i32,
}

export struct Wide align(16)
{
    a: i8,
    flags: u32 : 3,
}

export struct Holder
{
    tags: [4]u8,
    cb: fn(c_long, *byte) -> bool,
    next: *Holder,
    n: Num,
}

export const HALF: f32 = 0.5;
export const ON: bool = true;
export const NAME: str = "shapes";

/// Set n bytes from p to the low byte of w.
export fn fill(p: *byte, n: c_size_t, w: c_wchar)
{
    let i: c_size_t = 0;
    while i < n do {
        p[i as int] = w as byte;
        i += 1;
    }
}
```

The header of `com.example.shapes` holds these definitions after the union `Num`. Apple clang 21.0.0 compiles the header as C11 and as C++17 on the development Mac. The test `clib_header` checks there that `sizeof(Tight)` is 5 and the alignment of `Wide` is 16 in both languages.

```c
/** Five bytes without padding. */
#pragma pack(push, 1)
typedef struct Tight {
    uint8_t tag;
    int32_t value;
} Tight;
#pragma pack(pop)

typedef struct Wide {
    ANTI_ALIGNAS(16) int8_t a;
    uint32_t flags : 3;
} Wide;

typedef struct Holder {
    uint8_t tags[4];
    bool (*cb)(long, uint8_t *);
    struct Holder *next;
    Num n;
} Holder;

#define HALF 0.5f
#define ON true
static const char NAME[] = "shapes";

/** Set n bytes from p to the low byte of w. */
void fill(uint8_t *p, uint64_t n, wchar_t w);
```

### Constants and prototypes

An integer constant becomes a `#define` with a cast to its C type, as `((int64_t)16)`. A float prints with 17 significant digits and an `f` suffix for `f32`. A `bool` becomes `true` or `false`, and a `str` becomes a `static const char` array with escapes for quotes, backslashes and bytes outside printable ASCII.

```c
static void constant(struct text *out, const struct symbol *sym)
{
    const struct const_value *v = sym->value;
    const struct type *t = sym->type;
    size_t i;

    doc_comment(out, &sym->doc, "");
    switch (v->kind) {
    case CONST_BOOL:
        text_appendf(out, "#define %.*s %s\n", (int)sym->name.length,
                     sym->name.text, v->as.boolean ? "true" : "false");
        return;
    case CONST_FLOAT:
        text_appendf(out, "#define %.*s %.17g%s\n", (int)sym->name.length,
                     sym->name.text, v->as.floating,
                     t->kind == TYPE_F32 ? "f" : "");
        return;
    case CONST_TEXT:
        text_appendf(out, "static const char %.*s[] = \"",
                     (int)sym->name.length, sym->name.text);
        for (i = 0; i < v->as.text.length; i++) {
            unsigned char c = (unsigned char)v->as.text.bytes[i];
            if (c == '"' || c == '\\') {
                text_appendf(out, "\\%c", c);
            } else if (c < 0x20 || c >= 0x7f) {
                text_appendf(out, "\\%03o", c);
            } else {
                text_appendf(out, "%c", c);
            }
        }
        text_append(out, "\";\n");
        return;
    default:
        text_appendf(out, "#define %.*s ((%s)%" PRId64 ")\n",
                     (int)sym->name.length, sym->name.text, scalar_name(t),
                     (int64_t)v->as.integer);
        return;
    }
}
```

A prototype names each parameter as the Anti function does, since a library file keeps the parameter names of its functions. The header comes out the same from the source and from a library file. A parameter or a field whose name C or C++ reserves gets a trailing `_`, so `default` becomes `default_`. The reserved names are the keywords of C11 and C++17 and the macros of the three included headers.

```c
/* DESIGN: the header keeps the Anti name of a parameter or a field. A
   name that C or C++ reserves gets a trailing _, as default_. */
static void c_name(char *out, size_t size, const struct name *name)
{
    size_t i;
    bool reserved = stdint_macro(name);

    for (i = 0; !reserved && i < sizeof reserved_names / sizeof *reserved_names;
         i++) {
        reserved = strlen(reserved_names[i]) == name->length &&
                   memcmp(reserved_names[i], name->text, name->length) == 0;
    }
    snprintf(out, size, "%.*s%s", (int)name->length, name->text,
             reserved ? "_" : "");
}
```

```c
/* A prototype names its parameters as the Anti function does. */
static void prototype(struct text *out, const struct symbol *sym)
{
    const struct type *t = sym->type;
    struct text inner = {0};
    struct text decl = {0};
    size_t i;

    doc_comment(out, &sym->doc, "");
    text_appendf(&inner, "%.*s(", (int)sym->name.length, sym->name.text);
    for (i = 0; i < t->param_count; i++) {
        struct text param = {0};
        char name[128];
        c_name(name, sizeof name, &sym->params[i]);
        declaration(&param, t->params[i], name, NULL);
        text_appendf(&inner, "%s%s", i > 0 ? ", " : "", text_cstr(&param));
        text_free(&param);
    }
    text_append(&inner, t->param_count == 0 ? "void)" : ")");
    declaration(&decl, t->result, text_cstr(&inner), NULL);
    text_appendf(out, "%s;\n", text_cstr(&decl));
    text_free(&inner);
    text_free(&decl);
}
```

### Header layout

The function `header_write` writes the frame of the header. The include guard is the library name in uppercase with every other character replaced by `_`. The top comment states that a failure Anti cannot report calls `abort()`. With a bundled runtime, the comment adds that a program may link only one such archive.

```c
void header_write(struct text *out, const char *name,
                  const struct interface *const *ifaces, size_t count,
                  bool bundled)
{
    struct emitted done = {0};
    size_t total = 0;
    size_t i;
    size_t j;
    bool any;

    for (i = 0; i < count; i++) {
        total += ifaces[i]->item_count;
    }
    done.items = calloc(total + 1, sizeof *done.items);
    text_appendf(out, "/* %s.h, the C interface of %s, written by antic.\n"
                      "   Do not edit. A failure that Anti cannot report calls "
                      "abort().%s */\n",
                 name, count > 0 ? ifaces[count - 1]->module : name,
                 bundled ? "\n   The archive holds the Anti runtime. Link only "
                           "one archive\n   with a bundled runtime into a "
                           "program."
                         : "");
    text_append(out, "#ifndef ");
    for (i = 0; name[i] != '\0'; i++) {
        char c = name[i];
        text_appendf(out, "%c", (c >= 'a' && c <= 'z') ? (char)(c - 32)
                                : ((c >= 'A' && c <= 'Z') ||
                                   (c >= '0' && c <= '9'))
                                    ? c
                                    : '_');
    }
    text_append(out, "_H\n#define ");
    for (i = 0; name[i] != '\0'; i++) {
        char c = name[i];
        text_appendf(out, "%c", (c >= 'a' && c <= 'z') ? (char)(c - 32)
                                : ((c >= 'A' && c <= 'Z') ||
                                   (c >= '0' && c <= '9'))
                                    ? c
                                    : '_');
    }
    text_append(out, "_H\n\n"
                     "#include <stdbool.h>\n"
                     "#include <stddef.h>\n"
                     "#include <stdint.h>\n\n"
                     "#ifdef __cplusplus\n"
                     "#define ANTI_ALIGNAS(n) alignas(n)\n"
                     "extern \"C\" {\n"
                     "#else\n"
                     "#define ANTI_ALIGNAS(n) _Alignas(n)\n"
                     "#endif\n\n");
    for (i = 0; i < count; i++) {
        for (j = 0; j < ifaces[i]->item_count; j++) {
            const struct symbol *sym = ifaces[i]->items[j];
            if (sym->exported && sym->kind == SYMBOL_STRUCT) {
                aggregate(out, sym, ifaces, count, &done);
            }
        }
    }
    for (i = 0, any = false; i < count; i++) {
        for (j = 0; j < ifaces[i]->item_count; j++) {
            const struct symbol *sym = ifaces[i]->items[j];
            if (sym->exported && sym->kind == SYMBOL_CONST) {
                constant(out, sym);
                any = true;
            }
        }
    }
    text_append(out, any ? "\n" : "");
    for (i = 0, any = false; i < count; i++) {
        for (j = 0; j < ifaces[i]->item_count; j++) {
            const struct symbol *sym = ifaces[i]->items[j];
            if (sym->exported && sym->kind == SYMBOL_FN) {
                prototype(out, sym);
                any = true;
            }
        }
    }
    text_append(out, any ? "\n" : "");
    text_append(out, "#ifdef __cplusplus\n}\n#endif\n\n#endif\n");
    free((void *)done.items);
}
```

## Library output

The driver runs the back end on the whole program, writes the assembly file and assembles it with llvm-mc, as for an executable. It then calls `build_c_library` with the object file. The function writes the header file and builds the static archive or links the shared library.

```c
/* Write a library for C from object: its header, and the static archive
   with the copy of the package header, or the shared library. A static
   library prints the line that links a C program with it. */
static bool build_c_library(const struct options *o, const char *object,
                            const char *base, const struct extras *extras)
{
    const struct target_info *info = target_info(o->target);
    const char *name = text_cstr(&extras->name);
    const char *slash = strrchr(base, '/');
    struct text dir = {0};
    struct text path = {0};
    struct text header = {0};
    struct text major = {0};
    bool ok;

    if (slash != NULL) {
        text_appendf(&dir, "%.*s/", (int)(slash - base), base);
    }
    if (o->output != NULL) {
        text_append(&path, o->output);
    } else if (o->lib == LIB_STATIC) {
        text_appendf(&path, info->format == FORMAT_COFF ? "%s%s.lib"
                                                        : "%slib%s.a",
                     text_cstr(&dir), name);
    } else {
        text_appendf(&path,
                     info->os == OS_WINDOWS ? "%s%s.dll"
                     : info->os == OS_MACOS ? "%slib%s.dylib"
                                            : "%slib%s.so",
                     text_cstr(&dir), name);
    }
    text_appendf(&header, "%s%s%s", text_cstr(&dir), name, HEADER_SUFFIX);
    ok = write_file(text_cstr(&header), &extras->header);
    if (ok && o->lib == LIB_STATIC) {
        struct arena arena = {0};
        struct text package = {0};
        struct paths members = {0};
        struct link_command c;
        text_appendf(&package, "%s%s%s", text_cstr(&dir), name, PACKAGE_SUFFIX);
        ok = assemble_package(o, &extras->package, &package) &&
             bundle(o, object, base, &arena, &members);
        if (ok) {
            add_path(&members, text_cstr(&package));
            remove(text_cstr(&path));
            archive_command(&c, o->target,
                            o->llvm_ar != NULL ? o->llvm_ar : "llvm-ar",
                            text_cstr(&path), members.items, members.count);
            ok = run_command(&c, "llvm-ar");
            link_command_free(&c);
        }
        if (ok) {
            struct text line = {0};
            link_line(&line, o->target, text_cstr(&path), o->runtime,
                      o->bundle_runtime);
            printf("%s\n", text_cstr(&line));
            text_free(&line);
        }
        free((void *)members.items);
        arena_free(&arena);
        text_free(&package);
    } else if (ok) {
        struct link_inputs in;
        struct link_command c;
        struct shared_options s = {NULL, NULL, NULL};
        struct link_facts facts;
        struct text def = {0};
        struct text versioned = {0};
        memset(&in, 0, sizeof in);
        in.object = object;
        in.runtime = o->runtime;
        in.extra = o->objects;
        in.extra_count = o->object_count;
        in.executable = text_cstr(&path);
        if (o->soname) {
            const char *version = o->package_version;
            if (version == NULL) {
                fputs("antic: --soname needs --package-version\n", stderr);
                ok = false;
            } else {
                text_appendf(&major, "%.*s", (int)strcspn(version, "."),
                             version);
                s.major = text_cstr(&major);
                s.version = version;
            }
        }
        if (ok && info->os == OS_LINUX && o->soname) {
            text_appendf(&versioned, "%s.%s", text_cstr(&path),
                         text_cstr(&major));
            in.executable = text_cstr(&versioned);
        }
        if (ok && info->os == OS_WINDOWS) {
            struct text content = {0};
            const char *p = text_cstr(&extras->exports);
            text_appendf(&def, "%s%s%s", text_cstr(&dir), name, DEF_SUFFIX);
            text_appendf(&content, "LIBRARY %s\nEXPORTS\n", name);
            while (*p != '\0') {
                size_t n = strcspn(p, "\n");
                text_appendf(&content, "    %.*s\n", (int)n, p);
                p += n + (p[n] == '\n');
            }
            text_append(&content, "    anti_licenses DATA\n");
            ok = write_file(text_cstr(&def), &content);
            s.def_file = text_cstr(&def);
            text_free(&content);
        }
        memset(&facts, 0, sizeof facts);
        ok = ok && link_facts(o, &in, &facts);
        if (ok) {
            link_shared_command(&c, o->target, &in, &s);
            ok = run_command(&c, "the linker");
            link_command_free(&c);
        }
        /* DESIGN: with --soname a Linux library is lib<name>.so.<major>,
           and lib<name>.so links to it for the C compiler. */
        if (ok && versioned.length > 0) {
            const char *link_name = strrchr(text_cstr(&versioned), '/');
            const char *argv[] = {"ln", "-sf", NULL, NULL, NULL};
            argv[2] = link_name != NULL ? link_name + 1 : text_cstr(&versioned);
            argv[3] = text_cstr(&path);
            ok = process_run(argv) == 0;
        }
        link_facts_free(&facts);
        text_free(&def);
        text_free(&versioned);
    }
    text_free(&dir);
    text_free(&path);
    text_free(&header);
    text_free(&major);
    return ok;
}
```

### Static archive

A static library holds two members: the object of the module and the package header copy. The function `archive_command` builds the llvm-ar command line. The option `--format` selects the archive format, and the operation `r` with the modifiers `c` and `s` creates the archive with a symbol table[^4].

```c
void archive_command(struct link_command *c, enum target t,
                     const char *llvm_ar, const char *archive,
                     const char *const *members, size_t count)
{
    static const char *const formats[] = {
        [FORMAT_ELF] = "--format=gnu",
        [FORMAT_MACHO] = "--format=darwin",
        [FORMAT_COFF] = "--format=coff",
    };
    size_t i;

    start(c, count);
    add(c, llvm_ar);
    add(c, formats[target_info(t)->format]);
    add(c, "rcs");
    add(c, archive);
    for (i = 0; i < count; i++) {
        add(c, members[i]);
    }
}
```

```text
geo.o
geo.package.o
```

The archive holds no runtime. Two Anti static libraries in one C program would otherwise define every runtime symbol twice. The driver prints the command line that links a C program `main.c` with the archive and the runtime library of the target. On Linux the line adds `-lpthread -lm`.

```c
void link_line(struct text *out, enum target t, const char *library,
               const char *runtime, bool bundled)
{
    bool windows = target_info(t)->os == OS_WINDOWS;

    text_appendf(out, "%s main.c %s", windows ? "cl" : "cc", library);
    if (!bundled) {
        text_append(out, " ");
        link_runtime_library(out, runtime, t);
    }
    if (target_info(t)->os == OS_LINUX) {
        text_append(out, " -lpthread -lm");
    }
}
```

The command in the section on the generated header prints this line on the development Mac.

```text
cc main.c libgeo.a build/runtime/lib/macos-arm64/libanti_rt.a
```

### Package header copy

A static library has no link step, so it carries no `anti_licenses` notice. It carries a copy of the package header of its library file instead, with the licence fields, which `anti license --from-archive` reads. The function `assemble_package` writes the copy as an assembly file `<name>.package.s` and assembles it into `<name>.package.o`.

```c
/* Assemble the object of the package header copy at <path>.o, and make
   path that object. */
static bool assemble_package(const struct options *o, const struct text *bytes,
                             struct text *path)
{
    struct text source = {0};
    struct text asm_path = {0};
    struct text obj_path = {0};
    bool ok;

    emit_package(&source, o->target, bytes->data, bytes->length);
    text_appendf(&asm_path, "%s%s", text_cstr(path), ASSEMBLY_SUFFIX);
    text_appendf(&obj_path, "%s%s", text_cstr(path),
                 target_info(o->target)->object_suffix);
    ok = write_file(text_cstr(&asm_path), &source) &&
         assemble(o, text_cstr(&asm_path), text_cstr(&obj_path));
    text_free(path);
    text_append(path, text_cstr(&obj_path));
    text_free(&source);
    text_free(&asm_path);
    text_free(&obj_path);
    return ok;
}
```

The function `emit_package` puts the bytes into a section named `anti_package` in the spelling of each object format. No symbol names the section, so a link never takes the member into a program.

```c
/* DESIGN: the copy of the package header in a static library is an object
   file, because Apple's linker refuses an archive member of another kind.
   No symbol names the section, so no link takes the member. */
void emit_package(struct text *out, enum target t, const char *bytes,
                  size_t length)
{
    static const char *const sections[] = {
        [FORMAT_ELF] = ".anti_package,\"\",@progbits",
        [FORMAT_MACHO] = "__DATA,__anti_package",
        [FORMAT_COFF] = ".anti_package,\"dr\"",
    };
    size_t k;

    if (target_info(t)->format == FORMAT_MACHO) {
        text_appendf(out, "    .build_version macos, %d, %d\n",
                     MACOS_MIN_MAJOR, MACOS_MIN_MINOR);
    }
    text_appendf(out, "    .section %s\n", sections[target_info(t)->format]);
    for (k = 0; k < length; k++) {
        text_appendf(out, "%s0x%02x", k % 16 == 0 ? "    .byte " : ", ",
                     (unsigned char)bytes[k]);
        if (k % 16 == 15 || k + 1 == length) {
            text_append(out, "\n");
        }
    }
}
```

The file of the example starts with the magic `ANTL` and the format version 10. Without package options the package name is the module path `com.example.geo`, and the version is `0.0.0`. Four zero fields follow, which hold the counts or lengths of the dependencies, the licence, the licence text and the attribution. The module path, an import count of 0 and the `//!` text of the module end the file.

```text
    .build_version macos, 11, 0
    .section __DATA,__anti_package
    .byte 0x41, 0x4e, 0x54, 0x4c, 0x0a, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x63, 0x6f, 0x6d, 0x2e
    .byte 0x65, 0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x2e, 0x67, 0x65, 0x6f, 0x05, 0x00, 0x00, 0x00, 0x30
    .byte 0x2e, 0x30, 0x2e, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    .byte 0x00, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x63, 0x6f, 0x6d, 0x2e, 0x65, 0x78, 0x61, 0x6d
    .byte 0x70, 0x6c, 0x65, 0x2e, 0x67, 0x65, 0x6f, 0x00, 0x00, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0x50
    .byte 0x6c, 0x61, 0x6e, 0x65, 0x20, 0x67, 0x65, 0x6f, 0x6d, 0x65, 0x74, 0x72, 0x79, 0x20, 0x66, 0x6f
    .byte 0x72, 0x20, 0x43, 0x20, 0x70, 0x72, 0x6f, 0x67, 0x72, 0x61, 0x6d, 0x73, 0x2e
```

### Bundled runtime

With `--bundle-runtime` the archive holds the runtime, for a C project that wants one file. The function `bundle` lists the members of the runtime library with `llvm-ar t` and extracts each one with `llvm-ar x` into the directory `<base>.rt`[^4]. It leaves out the object of `rt/start.c`, whose C `main` belongs to executables. It also leaves out the object of `rt/license.c`, the text of `anti.license`, which needs the notice `anti_licenses` that a static library does not have. On ELF and Mach-O it joins the module object and the runtime objects into one relocatable object `<base>.bundled.o`. COFF has no relocatable link, so a Windows archive holds the runtime objects as members of their own.

```c
/* The members of a static library besides the package header. They are
   the compiled object, or with a bundled runtime one relocatable object
   of it and the runtime's members. COFF has no relocatable link, so a
   Windows archive holds the runtime's objects as members of their own.
   The paths are in the memory pool. */
static bool bundle(const struct options *o, const char *object,
                   const char *base, struct arena *arena,
                   struct paths *members)
{
    const char *llvm_ar = o->llvm_ar != NULL ? o->llvm_ar : "llvm-ar";
    bool coff = target_info(o->target)->format == FORMAT_COFF;
    struct text library = {0};
    struct text listing = {0};
    struct text dir = {0};
    struct text output = {0};
    struct text joined = {0};
    struct paths objects = {0};
    const char *p;
    bool ok;

    if (!o->bundle_runtime) {
        add_path(members, object);
        return true;
    }
    link_runtime_library(&library, o->runtime, o->target);
    text_appendf(&dir, "%s.rt", base);
    text_appendf(&output, "--output=%s", text_cstr(&dir));
    {
        const char *argv[] = {llvm_ar, "t", text_cstr(&library), NULL};
        ok = process_capture(argv, &listing) == 0;
    }
    add_path(&objects, object);
    for (p = text_cstr(&listing); ok && *p != '\0';) {
        size_t n = strcspn(p, "\r\n");
        if (n > 0 &&
            strncmp(p, RUNTIME_START_MEMBER, strlen(RUNTIME_START_MEMBER)) != 0 &&
            strncmp(p, RUNTIME_LICENSE_MEMBER,
                    strlen(RUNTIME_LICENSE_MEMBER)) != 0) {
            struct text member = {0};
            char *path = arena_alloc(arena, dir.length + n + 2);
            const char *argv[] = {llvm_ar, "x", NULL, NULL, NULL, NULL};
            text_appendf(&member, "%.*s", (int)n, p);
            snprintf(path, dir.length + n + 2, "%s/%.*s", text_cstr(&dir),
                     (int)n, p);
            argv[2] = text_cstr(&output);
            argv[3] = text_cstr(&library);
            argv[4] = text_cstr(&member);
            ok = process_run(argv) == 0;
            add_path(&objects, path);
            text_free(&member);
        }
        p += n;
        p += strspn(p, "\r\n");
    }
    if (ok && coff) {
        size_t i;
        for (i = 0; i < objects.count; i++) {
            add_path(members, objects.items[i]);
        }
    } else if (ok) {
        struct link_command c;
        struct link_inputs in;
        struct link_facts facts;
        char *copy;
        text_appendf(&joined, "%s.bundled%s", base,
                     target_info(o->target)->object_suffix);
        memset(&in, 0, sizeof in);
        ok = link_facts(o, &in, &facts);
        if (ok) {
            relocatable_command(&c, o->target, &in, text_cstr(&joined),
                                objects.items, objects.count);
            ok = run_command(&c, "joining the runtime into the library");
            link_command_free(&c);
        }
        link_facts_free(&facts);
        copy = arena_alloc(arena, joined.length + 1);
        memcpy(copy, text_cstr(&joined), joined.length + 1);
        add_path(members, copy);
    }
    if (!ok) {
        fprintf(stderr, "antic: cannot bundle %s\n", text_cstr(&library));
    }
    free((void *)objects.items);
    text_free(&library);
    text_free(&listing);
    text_free(&dir);
    text_free(&output);
    text_free(&joined);
    return ok;
}
```

The relocatable link is `-r`, which merges object files into another object file in GNU ld[^5], in ld.lld and in Apple's ld[^6]. On ELF antic runs `ld.lld -r` of the runtime archive. The linker ld64.lld of LLVM 23.1.1 writes no relocatable object. For `-r` it prints ``warning: Option `-r' is not yet implemented`` and links an executable instead, so a Mach-O bundle joins with Apple's `ld -r` on a Mac. The runtime library is compiled with hidden visibility. Apple's ld turns such symbols into static symbols in the output unless `-keep_private_externs` is given[^6]. With the option the runtime symbols stay global in the joined object.

```c
/* DESIGN: ld64 turns hidden symbols into local ones in a relocatable
   object unless -keep_private_externs is given. The runtime symbols stay
   global, so two bundled runtimes in one program are a duplicate. ld64.lld
   writes no relocatable object, so Mach-O always joins with ld64. */
void relocatable_command(struct link_command *c, enum target t,
                         const struct link_inputs *in, const char *output,
                         const char *const *objects, size_t count)
{
    size_t i;

    start(c, count);
    add(c, target_info(t)->os == OS_MACOS ? "ld"
                                          : program(c, in, "ld.lld", "ld"));
    add(c, "-r");
    if (target_info(t)->os == OS_MACOS) {
        add(c, "-keep_private_externs");
        add(c, "-arch");
        add(c, target_info(t)->arch == ARCH_ARM64 ? "arm64" : "x86_64");
    }
    add(c, "-o");
    add(c, output);
    for (i = 0; i < count; i++) {
        add(c, objects[i]);
    }
}
```

The command of the section on the generated header, with `--bundle-runtime` added, prints `cc main.c libgeo.a`. It writes an archive with two members.

```text
geo.bundled.o
geo.package.o
```

The header warns about the bundled runtime in its top comment.

```c
/* geo.h, the C interface of com.example.geo, written by antic.
   Do not edit. A failure that Anti cannot report calls abort().
   The archive holds the Anti runtime. Link only one archive
   with a bundled runtime into a program. */
```

A C program that links two bundled archives fails at link time. On the development Mac, `cc` with `libgeo.a` and a bundled `libother.a` of `tests/clib/com/example/other.anti` reports eight duplicate symbols, each defined in both `geo.bundled.o` and `other.bundled.o`. They are `_anti_rt_init`, `_anti_rt_ready`, `_anti_split_command_line`, `_anti_utf16_to_utf8` and `_anti_utf8_repair`, and the functions `_anti_rt_write`, `_anti_rt_exit` and `_anti_rt_text_from_c` of the standard library.

### Shared library

A constructor initialises the runtime when the loader maps the library, and the linker copies the runtime library in. The back end appends that constructor, a call of `anti_rt_init`, to the assembly. It appends the licence notice `anti_licenses` to every output except a static archive, and it records the names of the export functions for a Windows `.def` file. [Chapter 16]({{% relref "/programming/writing-a-compiler/16-assembly-emission" %}}) covers the constructor sections, the notice, hidden symbols and the `.def` file per object format.

```c
        if (ok && o->lib == LIB_SHARED) {
            emit_constructor(assembly, o->target, "anti_rt_init");
        }
        if (ok && extras->notice.length > 0 && status != 3 &&
            !o->assembly_only && o->lib != LIB_STATIC) {
            emit_licenses(assembly, o->target, extras->notice.data,
                          extras->notice.length);
        }
        for (i = 0; ok && i < program->function_count; i++) {
            const struct ir_function *f = program->functions[i];
            if (f->exported && !f->is_extern) {
                text_appendf(&extras->exports, "%s\n", f->name);
            }
        }
        status = !ok ? 1 : status == 3 ? 3 : 0;
```

The function `link_shared_command` builds the linker command line, with lld unless `--linker platform` selects the platform linker. Apple's ld and ld64.lld take `-dylib` for a Mach-O shared library[^6]. GNU ld and ld.lld take `-shared`[^5]. An ELF shared library that ld.lld links names no C library. Its C symbols stay undefined, and the C library of the process that loads it defines them. The Microsoft linker builds a DLL with the option `/DLL`[^7] and reads the exports from the module-definition file that `/DEF` names[^8].

```c
void link_shared_command(struct link_command *c, enum target t,
                         const struct link_inputs *in,
                         const struct shared_options *s)
{
    struct text *library;

    start(c, in->extra_count);
    library = next(c);
    link_runtime_library(library, in->runtime, t);
    switch (target_info(t)->os) {
    case OS_MACOS: {
        struct text *install = next(c);
        struct text *compat = next(c);
        const char *base = strrchr(in->executable, '/');
        text_appendf(install, "@rpath/%s",
                     base != NULL ? base + 1 : in->executable);
        text_appendf(compat, "%s.0.0", s->major != NULL ? s->major : "");
        macos_start(c, t, in, true);
        add(c, "-o");
        add(c, in->executable);
        if (s->major != NULL) {
            add(c, "-install_name");
            add(c, text_cstr(install));
            add(c, "-compatibility_version");
            add(c, text_cstr(compat));
            add(c, "-current_version");
            add(c, s->version);
        }
        add_inputs(c, in);
        add(c, text_cstr(library));
        add(c, "-lSystem");
        break;
    }
    case OS_LINUX: {
        const char *linker = program(c, in, "ld.lld", "ld");
        struct text *search = next(c);
        const char *base = strrchr(in->executable, '/');
        text_appendf(search, "-L%s", in->crt_dir);
        add(c, linker);
        add(c, "-shared");
        add(c, "-o");
        add(c, in->executable);
        if (s->major != NULL) {
            add(c, "-soname");
            add(c, base != NULL ? base + 1 : in->executable);
        }
        add_inputs(c, in);
        add(c, text_cstr(library));
        if (in->linker == LINKER_PLATFORM) {
            add(c, text_cstr(search));
            add(c, "-lc");
        }
        break;
    }
    case OS_WINDOWS: {
        const char *linker = program(c, in, "lld-link", "link.exe");
        struct text *output = next(c);
        struct text *def = next(c);
        text_appendf(output, "/OUT:%s", in->executable);
        text_appendf(def, "/DEF:%s", s->def_file != NULL ? s->def_file : "");
        add(c, linker);
        add(c, "/NOLOGO");
        add(c, "/DLL");
        add(c, target_info(t)->arch == ARCH_ARM64 ? "/MACHINE:ARM64"
                                                  : "/MACHINE:X64");
        add(c, text_cstr(output));
        add(c, text_cstr(def));
        windows_libpaths(c, t, in);
        add_inputs(c, in);
        add(c, text_cstr(library));
        add(c, "libcmt.lib");
        add(c, "libvcruntime.lib");
        add(c, "libucrt.lib");
        break;
    }
    }
}
```

The file `rt/init.c` holds the state of the runtime, a flag that `anti_rt_ready` reads. An executable calls `anti_rt_init` first in the C `main` of `rt/start.c`. A shared library calls it from its constructor. A static library initialises the runtime on the first use of the runtime, as the comment in `rt/rt.h` states.

```c
#ifndef ANTI_RT_H
#define ANTI_RT_H

/* The state of the runtime. An executable initialises it in main. A shared
   library initialises it in a constructor that runs when it loads. A
   static library for C initialises it on the first use of the runtime. */
void anti_rt_init(void);

/* 1 after anti_rt_init, else 0. */
int anti_rt_ready(void);

#endif
```

```c
#include "rt.h"

static int ready;

void anti_rt_init(void)
{
    ready = 1;
}

int anti_rt_ready(void)
{
    return ready;
}
```

Every function of the program is a local symbol in its assembly file, except the export functions. The CMake build compiles the runtime library with hidden visibility, so its symbols stay inside the shared library. On the development Mac, `nm -gU libgeo.dylib` lists the six export functions and `anti_licenses`.

```text
00000000000005f4 S _anti_licenses
0000000000000564 T _geo_apply
0000000000000478 T _geo_dot
0000000000000500 T _geo_half
000000000000053c T _geo_layer
00000000000005c4 T _geo_ready
00000000000004bc T _geo_scale
```

A C program needs no initialisation call. The program `tests/clib/loader.c` loads `libgeo.dylib` with `dlopen`, calls `geo_ready` first and prints its result `1`. The constructor has initialised the runtime before that call. A C program that links `libgeo.a` and calls `geo_ready` prints `0` on the development Mac, because nothing in `com.example.geo` uses the runtime before the call.

### Shared library versions

The option `--soname` writes the version of `--package-version` into the library, and antic stops with `--soname needs --package-version` without it. The major version is the compatibility version. On Linux the file is `lib<name>.so.<major>`, and the soname is that file name. GNU ld writes it into the `DT_SONAME` field. When an executable linked with such a library runs, the dynamic linker loads the file that `DT_SONAME` names[^5]. The driver then links `lib<name>.so` to the file with `ln -sf`, for the C compiler.

On macOS the install name is `@rpath/lib<name>.dylib`. A client records that path as the way the dynamic loader locates the library[^6]. The compatibility version is `<major>.0.0`, which the loader checks against the version a program was linked with[^6]. The current version is the full version. A shared library with `--soname --package-version 1.2.3` on the development Mac carries these values.

```text
libgeo.dylib:
	@rpath/libgeo.dylib (compatibility version 1.0.0, current version 1.2.3)
	/usr/lib/libSystem.B.dylib (compatibility version 1.0.0, current version 1356.0.0)
```

The Windows command line ignores the version. Without the option a library carries no version.

## Memory across the boundary

Anti code and C code share one heap. The Anti operations `alloc` and `free` call the C functions `malloc` and `free`, so a C caller may `free` what an exported function returned. The `///` comment of each exported function states who frees what, and the header carries the comment.

## Threads across the boundary

A C thread that calls an exported function is the dispatching thread for every `parallel` inside it. Chapter 22, Threads, defines `parallel` and the worker pool. Every exported function of a library shares one pool. A C program that calls exported functions from two threads at once runs two `parallel` blocks on that pool, and a saturated pool runs jobs inline. Anti code holds no mutable global state, so an exported function is reentrant unless its `///` comment says otherwise.

## Errors across the boundary

Anti has no exceptions, and the boundary adds none. An exported function reports failure through its return value or an out parameter, as a C function does. A failure that Anti cannot report calls `abort()`, which the top comment of every header states.

## Tests

The C program `tests/clib/roundtrip.c` calls every export function of `com.example.geo`. It passes structs and a union by value, a pointer to a bitfield struct and a C function as a function pointer. It also prints the constant `LAYERS`.

```c
/* A C program that calls every export fn of the library geo, with structs
   and unions by value, and prints the results. */
#include <stdio.h>

#include "geo.h"

static int32_t square(int32_t v)
{
    return v * v;
}

int main(void)
{
    Vec2 a = {3, 4};
    Vec2 b = {5, -6};
    Vec2 scaled = geo_scale(a, 10);
    Num n;
    Num half;
    Flags flags = {0, 9};

    n.d = 7.0;
    half = geo_half(n);
    printf("%d\n", geo_dot(a, b));
    printf("%d %d\n", scaled.x, scaled.y);
    printf("%g\n", half.d);
    printf("%u %u\n", geo_layer(&flags), flags.visible);
    printf("%d\n", geo_apply(square, a));
    printf("%d\n", (int)LAYERS);
    return 0;
}
```

The file `tests/clib/roundtrip.expected` holds its output. The dot product of (3, 4) and (5, -6) is -9, and half of 7.0 is 3.5. The function `geo_layer` returns the layer 9 and sets the flag `visible` to 1. The C callback squares both coordinates of (3, 4), which gives 25.

```text
-9
30 40
3.5
9 1
25
16
```

The script `tests/run_clib.cmake` builds the libraries with antic and runs one case per ctest test. The tests run on hosts other than Windows when the CMake build finds llvm-ar, and all seven pass on the development Mac.

| Test | Checks |
|---|---|
| `clib_static` | The printed link line, the member `geo.package.o`, and `roundtrip.c` linked with that line against `roundtrip.expected` |
| `clib_shared` | `roundtrip.c` linked with `libgeo.dylib` against `roundtrip.expected` |
| `clib_exports` | The defined global symbols of the shared library equal the six export functions and `anti_licenses` |
| `clib_two` | Two shared libraries in one program, and two static libraries with one `libanti_rt.a`, both print `10` |
| `clib_loader` | `loader.c` loads the shared library with `dlopen`, and `geo_ready` returns 1 as the first call |
| `clib_header` | `geo.h` and `shapes.h` equal `tests/dump/geo.h` and `tests/dump/shapes.h`, and both compile with `cc -std=c11 -Wall -Werror` and `c++ -std=c++17 -Wall -Werror`. `tests/clib/shapes.c` and `shapes.cpp` check that `Tight` has 5 bytes and `Wide` an alignment of 16 |
| `clib_bundle` | `geo.h` of a bundled archive equals `tests/dump/geo.bundle.h`, its link line has no runtime library, and two bundled archives fail to link with a duplicate symbol that names the runtime |

The unit test `test_header` in `tests/unit/test_header.c` pins two headers. The first comes from a module with a packed struct, an aligned struct with a bitfield, a union, an array field and a function pointer field. That module also declares four kinds of constants and two prototypes. The second header is the one of a bundled archive. The function `libraries` in `tests/unit/test_link.c` pins the command lines of llvm-ar, the shared library link, the relocatable link and the printed link line on all three operating systems. For linux-x86_64, macos-arm64 and windows-arm64 the file `tests/unit/test_emit.c` pins the constructor sections. The tests `asm_clib_<target>` write the assembly of the shared library `geo` with `-S` for all six targets and assemble it with llvm-mc. The Linux and Windows libraries are not linked or run, and the Windows `.def` file is not tested. On the development Mac every `ctest` test passes.

## Next

Chapter 26, The standard library, starts once the core is done and decides its scope then. The candidates in order are console and file I/O, string formatting, regular expressions, graphics and audio, and networking. It also covers the `anti.license` module and the `--system-certs` option of `anti.net`.

## References

[^1]: GNU Compiler Collection, *Using the GNU Compiler Collection*, section "Structure-Layout Pragmas", https://gcc.gnu.org/onlinedocs/gcc/Structure-Layout-Pragmas.html

[^2]: ISO/IEC JTC1/SC22/WG14, *N1570, Committee Draft of ISO/IEC 9899:201x*, 2011, section 6.7.5, HTML version at https://port70.net/~nsz/c/c11/n1570.html

[^3]: ISO/IEC JTC1/SC22/WG21, *Working Draft, Programming Languages, C++*, section [dcl.align], https://eel.is/c++draft/dcl.align

[^4]: LLVM Project, *llvm-ar, LLVM archiver*, operations `r`, `t` and `x`, modifiers `c` and `s`, options `--format` and `--output`, https://llvm.org/docs/CommandGuide/llvm-ar.html

[^5]: GNU Binutils, *The GNU linker*, section "Command-line Options", options `-shared`, `-soname` and `-r`, https://sourceware.org/binutils/docs/ld/Options.html

[^6]: Apple, *ld(1)*, options `-dylib`, `-r`, `-keep_private_externs`, `-install_name`, `-compatibility_version` and `-current_version`, https://keith.github.io/xcode-man-pages/ld.1.html

[^7]: Microsoft, */DLL (Build a DLL)*, https://learn.microsoft.com/en-us/cpp/build/reference/dll-build-a-dll?view=msvc-170

[^8]: Microsoft, */DEF (Specify module-definition file)*, https://learn.microsoft.com/en-us/cpp/build/reference/def-specify-module-definition-file?view=msvc-170
