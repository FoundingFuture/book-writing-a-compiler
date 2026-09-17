---
title: "Modules and library files"
description: "How antic derives module paths under search roots, checks imports and exports, mangles symbols, and writes and reads .antl library files."
summary: "Module paths that mirror a directory tree under the search roots, `import` with `as`, `pub`, `export` and name mangling with length-prefixed segments on COFF. The reserved `anti.` root, reverse-domain roots for third-party libraries and single-segment names for a program's own files. The `.antl` file with its package header and licence fields, the public interface with its doc text, and the unoptimised IR. The serialiser and deserialiser with a version stamp. The tests that library files are byte-identical on every host and that both doc comment forms write the same file."
date: 2026-09-13T22:32:00+02:00
lastmod: 2026-09-15T03:53:22+02:00
draft: false
weight: 90
tags: [compilers, programming-languages]
keywords: [module paths, search roots, library files, name mangling, serialisation, binary file format, package header, module interface]
---

## Previously

[Chapter 8, Lowering the syntax tree to IR]({{% relref "/programming/writing-a-compiler/08-lowering-to-ir" %}}), turns expressions, conditions, both loop forms, calls and returns into blocks and instructions. It decides where local variables live before register allocation and how address-taken variables are marked.

## Module paths

[Chapter 2, The Anti language]({{% relref "/programming/writing-a-compiler/02-the-anti-language" %}}), names every module by a module path. A module path is a sequence of lowercase ASCII identifiers joined by dots, such as `com.example.scale`. It mirrors a directory tree under the search roots. The search roots are the directories that the `-I` options of antic name, in their order on the command line.

The function `module_path_of_source` in `src/modpath.c` derives the module path of the source file that antic compiles. The first root whose directory contains the file gives the rest of the path. Slashes become dots, and the suffix `.anti` is dropped. Outside every root the path is the file name alone.

```c
bool module_path_of_source(const char *source, const char *const *roots,
                           size_t root_count, struct text *out, char *error,
                           size_t error_size)
{
    const char *rest = NULL;
    const char *dot = strrchr(source, '.');
    const char *end = dot != NULL && strcmp(dot, ".anti") == 0
                          ? dot
                          : source + strlen(source);
    size_t i;

    for (i = 0; i < root_count && rest == NULL; i++) {
        rest = under(source, roots[i]);
    }
    if (rest == NULL) {
        const char *slash = strrchr(source, '/');
        rest = slash != NULL ? slash + 1 : source;
    }
    while (rest < end) {
        const char *slash = memchr(rest, '/', (size_t)(end - rest));
        size_t n = slash != NULL ? (size_t)(slash - rest)
                                 : (size_t)(end - rest);
        if (!is_lower_identifier(rest, n)) {
            snprintf(error, error_size,
                     "`%.*s` in %s is not a lowercase identifier", (int)n,
                     rest, source);
            return false;
        }
        if (lexer_is_keyword(rest, n)) {
            snprintf(error, error_size, "`%.*s` in %s is a keyword", (int)n,
                     rest, source);
            return false;
        }
        text_appendf(out, "%s%.*s", out->length > 0 ? "." : "", (int)n, rest);
        rest += n + 1;
    }
    return true;
}
```

The helper `under` returns the part of the path after a root and its slash. Each segment then goes through two checks. The function `is_lower_identifier` accepts the letters `a` to `z`, digits and `_`, with no digit first. The function `lexer_is_keyword` from the lexer refuses a keyword, so no directory named `fn` becomes a segment. The unit test `module_paths` in `tests/unit/test_modules.c` runs the function with the roots `lib` and `src/`.

| Source file | Result |
|---|---|
| `src/com/niese/geo.anti` | `com.niese.geo` |
| `lib/anti/text.anti` | `anti.text` |
| `tools/geo.anti` | `geo` |
| `geo.anti`, without roots | `geo` |
| `src/com/my-lib/geo.anti` | `` `my-lib` in src/com/my-lib/geo.anti is not a lowercase identifier `` |
| `src/Com/geo.anti` | `` `Com` in src/Com/geo.anti is not a lowercase identifier `` |
| `src/com/fn/geo.anti` | `` `fn` in src/com/fn/geo.anti is a keyword `` |

### Roots of module paths

The first segment of a module path says who publishes the module. Every path that starts with `anti` belongs to the language's own libraries, such as `anti.text` and the runtime `anti.rt`. Third parties use a root they own in reverse-domain style, such as `com.niese.anti.web`, and the test modules use `com.example`. A path of one segment, such as `main`, names a file of a program and is not for publishing.

The driver checks these rules in `module_name` in `src/driver.c`, after the parser has built the syntax tree.

```c
/* The module path of the input, from its path under the search roots.
   DESIGN: antic -c refuses a library under the anti root without
   --anti-internal. It warns on a path of one segment, which is for a
   program's own files. No compilation may define the runtime's module. */
static bool module_name(const struct options *o, struct text *out)
{
    char error[200];

    if (!module_path_of_source(o->input, o->roots, o->root_count, out, error,
                               sizeof error)) {
        fprintf(stderr, "antic: %s\n", error);
        return false;
    }
    if (strcmp(text_cstr(out), RUNTIME_MODULE) == 0) {
        fprintf(stderr, "antic: %s: the module path `%s` is the runtime's\n",
                o->input, RUNTIME_MODULE);
        return false;
    }
    if (o->library && !o->internal && module_path_reserved(text_cstr(out))) {
        fprintf(stderr, "antic: %s: the module path `%s` is reserved for the "
                        "language's own libraries\n",
                o->input, text_cstr(out));
        return false;
    }
    if (o->library && module_path_segments(text_cstr(out)) == 1) {
        fprintf(stderr, "antic: %s: warning: the module path `%s` has one "
                        "segment, which is for a program's own files\n",
                o->input, text_cstr(out));
    }
    return true;
}
```

The runtime calls the `main` of a program through the symbol of the function `main` in the module `anti.rt`, the constants `RUNTIME_ENTRY` and `RUNTIME_MODULE` in `src/target.h`. A module with that path would define the symbol a second time, so every compilation refuses it. A library under `anti` needs the option `--anti-internal`. A library path of one segment compiles with a warning. Four tests in `tests/CMakeLists.txt` run these cases.

| Test | Input | Output of antic |
|---|---|---|
| `error_runtime_module` | `tests/errors/anti/rt.anti` with `-S -I tests/errors` | `` the module path `anti.rt` is the runtime's `` |
| `error_reserved_library` | `tests/modules/anti/text.anti` with `-c -I tests/modules` | `` the module path `anti.text` is reserved for the language's own libraries `` |
| `antl_internal` | the same command with `--anti-internal` | no output |
| `warning_one_segment` | `tests/errors/geometry.anti` with `-c` | `` warning: the module path `geometry` has one segment, which is for a program's own files `` |

Each message starts with `antic:` and the path of the source file.

## Commands for modules

The command `antic -c` compiles one module into a library file with the suffix `.antl`. A library file contains a package header, the public interface of the module with its doc text, and the unoptimised IR of all its functions. An importing module reads the interface. A program loads the IR.

The test modules in `tests/modules/com/example` form a chain under the search root `tests/modules`. The module `com.example.scale` has no imports.

```anti
pub const SCALE: int = 6;
pub fn scale(x: int) -> int
{
    return x * SCALE;
}
```

The module `com.example.twice` imports `com.example.scale`. The import declares the last segment of the path, `scale`, as the name of the module in `twice`.

```anti
import com.example.scale;

pub fn twice(x: int) -> int
{
    return scale.scale(x) + scale.scale(x);
}
```

The program `tests/modules/main.anti` imports `com.example.twice` and calls it through the name `twice`.

```anti
import com.example.twice;

fn main() -> int
{
    return twice.twice(7) / 2;
}
```

Three commands build the chain from the repository root. The library files go into the directory `lib`, which the second and third commands name as a search root. The compiler creates no directories, so `lib/com/example` must exist before the first command.

```sh
antic -c -I tests/modules -o lib/com/example/scale.antl tests/modules/com/example/scale.anti
antic -c -I tests/modules -I lib -o lib/com/example/twice.antl tests/modules/com/example/twice.anti
antic --dump-ir -I lib tests/modules/main.anti
```

The tests `antl_twice` and `dump_ir_modules` run the second and third command with the build directory's `tests/modules` in place of `lib`. The test `antl_scale` runs the first one with the package header options of the section on the bytes of `scale.antl`. The file `tests/modules/main.anti` lies outside the root `lib`, so its module path is `main`. The last command finds `lib/com/example/twice.antl` for the import, reads that the file imports `com.example.scale` and finds `lib/com/example/scale.antl` as well. It loads `scale.antl` before `twice.antl`, lowers `main.anti` after both and prints the whole program. The test `dump_ir_modules` compares the output with `tests/modules/main.ir`.

```text
fn com.example.scale.scale(%0: i64) -> i64 {
b0:
    %1 = mul i64 %0, 6
    ret i64 %1
}
fn com.example.twice.twice(%0: i64) -> i64 {
b0:
    %1 = call i64 @com.example.scale.scale(%0)
    %2 = call i64 @com.example.scale.scale(%0)
    %3 = add i64 %1, %2
    ret i64 %3
}
fn main.main() -> i64 {
b0:
    %0 = call i64 @com.example.twice.twice(7)
    %1 = sdiv i64 %0, 2
    ret i64 %1
}
```

The call `@com.example.scale.scale` inside `com.example.twice.twice` refers to the function that `scale.antl` defines. The optimizer of chapter 10 runs on this whole program.

### Library files under the search roots

A library file can also follow the source file on the command line, as in `antic --dump-ir tests/modules/main.anti lib/com/example/twice.antl lib/com/example/scale.antl`. That command prints the same program. The function `find_libraries` combines both ways.

```c
/* The library files of the compilation. They are the files on the command
   line and, for every import that none of them holds, the file under the
   search roots. The imports of a found file are looked up too. */
static bool find_libraries(const struct options *o, const struct module *tree,
                           struct arena *arena, struct paths *out)
{
    struct paths modules = {0};
    struct paths wanted = {0};
    char error[200];
    size_t read = 0;
    size_t i;
    bool ok = true;
    bool grew = true;

    for (i = 0; i < o->library_count; i++) {
        add_path(out, o->libraries[i]);
    }
    for (i = 0; i < tree->import_count; i++) {
        char *name = arena_alloc(arena, tree->imports[i].module.length + 1);
        memcpy(name, tree->imports[i].module.text,
               tree->imports[i].module.length);
        add_path(&wanted, name);
    }
    while (ok && grew) {
        for (; ok && read < out->count; read++) {
            struct text bytes = {0};
            struct interface header;
            ok = read_bytes(out->items[read], &bytes);
            if (ok && !antl_header((const uint8_t *)bytes.data, bytes.length,
                                   arena, &header, error, sizeof error)) {
                fprintf(stderr, "antic: %s %s\n", out->items[read], error);
                ok = false;
            }
            text_free(&bytes);
            if (ok) {
                add_path(&modules, header.module);
                for (i = 0; i < header.import_count; i++) {
                    add_path(&wanted, header.imports[i]);
                }
            }
        }
        grew = false;
        for (i = 0; ok && i < wanted.count; i++) {
            const char *found;
            if (contains(&modules, wanted.items[i])) {
                continue;
            }
            found = search_roots(o, wanted.items[i], arena);
            if (found != NULL && !contains(out, found)) {
                add_path(out, found);
                grew = true;
            }
        }
    }
    free((void *)modules.items);
    free((void *)wanted.items);
    return ok;
}
```

The list `wanted` starts with the imports of the source. Each library file adds its module path to `modules` and its imports to `wanted`. A wanted module that no file provides goes to `search_roots`. That function builds the path `<root>/<path>.antl` with a slash for each dot and returns the first such file that opens. The loop ends when a pass over `wanted` adds no file. The function `antl_header` reads each file only up to the module's doc text. The search therefore reads no type table, no items and no IR.

## Interfaces

The interface of a module lists its imports and its `pub` items. An item is a function, an `extern fn` declaration, a struct, a union or a constant. An `export` item is a `pub` item whose symbol has the flag `exported`, so the interface stores it like every other `pub` item. The structure is declared in `src/sema.h`, because semantic analysis both builds it and reads it.

```c
/* What other modules see of a module: its imports and its pub items. A
   library file stores it, and an import reads it. */
struct interface {
    struct package package;
    const char *doc;                /* the `//!` text of the module */
    const char *module;
    const char **imports;           /* module names, not aliases */
    size_t import_count;
    struct symbol **items;          /* pub fn, extern fn, struct and const */
    size_t item_count;
};
```

The function `sema_interface` builds the interface of a checked module. Each item is a copy of the symbol of a `pub` item, and the copy's field `home` points to the interface. A symbol with a `home` belongs to another module, which lowering needs to know. A constant keeps the value that semantic analysis computed, so an importing module can use it in its own constant expressions. The field `doc` is the `//!` text of the module, and each item symbol keeps its `///` text in its own field `doc`. The field `package` is the package header of the library file, which the section on the library file format describes.

## Imports in semantic analysis

The function `sema_check` receives the interfaces of all loaded libraries. For each `import` it finds the interface by the full module path. It declares the last segment of the path, or the alias after `as`, as a symbol of kind `SYMBOL_MODULE` at module level. That name counts as a module-level name, so `fn scale()` next to `import com.example.scale;` is a duplicate.

```c
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
```

The function `depends_on` follows the imports of a library through the imports of its imports. A library that depends on the module being checked forms a cycle. Chapter 2 forbids cycles, because each library file is built against the interfaces of its imports, and a cycle has no module to build first.

The parser reads each segment of an import as an identifier, so a keyword in an import path is the syntax error `expected identifier`. An uppercase letter in a segment reaches semantic analysis, which refuses it.

### Qualified names

A qualified name `twice.twice` parses as a field access on the name `twice`. When the base names a module, `check_qualified` looks the item up in the interface and rewrites the node into a plain name with the imported symbol. Lowering then handles `twice.twice(7)` like a call of a function in the same module.

```c
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
```

A type `geometry.Rect` and a struct literal `geometry.Rect { w: 1, h: 2 }` go through `imported_struct`, which accepts only a `pub` struct. The unit tests in `tests/unit/test_modules.c` import a module `geometry` with a `pub` struct `Rect`, a private struct `Private` and a private function `hidden`. The test `dotted_imports` imports the modules `com.example.scale` and `org.other.scale`. The tests check these messages.

| Source | Message |
|---|---|
| `import main;` in `main.anti` | `` `main` cannot import itself `` |
| `import shapes;` without a library | `` cannot find module `shapes` `` |
| `geometry.hidden()` for a private function | `` `geometry` has no public item `hidden` `` |
| `*geometry.Private` for a private struct | `` `geometry` has no public struct `Private` `` |
| `let m = geometry;` | `` `geometry` is a module, not a value `` |
| `let t = geometry.Rect;` | `` `geometry.Rect` is a type, not a value `` |
| `import com.example.scale;` and `import org.other.scale;` | `` `scale` is already declared `` |
| `import com.Example.scale;` | `` the module path `com.Example.scale` is not lowercase `` |

The second import of `org.other.scale` compiles with `as other`, which declares the name `other` in place of `scale`.

### Methods of imported structs

Chapter 2 resolves a method call `v.f(args)` in the module that declares the struct of `v`. The function `method_symbol` compares the module of the struct with the module being checked. For a struct of another module it searches that module's interface, which lists only `pub` functions.

```c
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
    return lib != NULL ? library_item(lib, name) : NULL;
}
```

### Exported names

An `export fn` has the C symbol of its name on every target, the symbol that chapter 16 emits. Two modules of one program must not export one name, and the export check of chapter 6 looks for the name in every loaded interface. The unit test `unique_exports` compiles a library `geo` with `export fn dot` and a module that exports `dot` again. It checks the message `` export fn `dot` has the symbol of export fn `dot` in module `geo` ``.

The reader of a library file marks the type of an exported struct or union as exported. An `export fn` of the importing module may then use that type in its signature, which the export signature rule of chapter 6 allows for exported structs only.

## Calls into other modules

Lowering creates the IR for one module, and a function of another module has no body there. The IR marks such a function as `extern`, as it marks a C function, and keeps its module path. The text form prints it as `extern fn geometry.area(i64, i64) -> i64`, which the unit test `lowers_imports` checks.

```c
/* A function of another Anti module, which that module's IR defines. */
struct ir_function *ir_declare_add(struct ir_module *m, const char *module,
                                   const char *name, enum ir_type result,
                                   uint32_t result_agg);
```

The function `callee_function` finds the IR function of a call. A function of the same module has its index in the symbol. An imported function is looked up by module and name, and declared at its first call with its result aggregate, its parameters and its `exported` flag. An imported `extern fn` is a C function and keeps the plain C name.

```c
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
```

## Name mangling

An object file names each function by a symbol. Mangling derives the symbol from the module path and the function name. The IR keeps module and name apart, and only assembly emission in chapter 16 needs a symbol. The function `mangle` in `src/target.c` writes the forms for the three object formats.

```c
/* DESIGN: module.name on ELF and Mach-O, as docs/decisions.md settles, with
   the dots of the module path kept. Mach-O prefixes every C-level symbol
   with '_'. On COFF the symbol holds only letters, digits and '_': _A,
   each segment of the module path after its length, '_' and the function
   name. The lengths keep a '_' inside a name from producing the symbol of
   another pair. C reserves names that start with _A. */
bool mangle(struct text *out, enum target t, const char *module,
            const char *name)
{
    if (t >= TARGET_COUNT) {
        return false;
    }
    switch (infos[t].format) {
    case FORMAT_MACHO:
        text_appendf(out, "_%s.%s", module, name);
        break;
    case FORMAT_ELF:
        text_appendf(out, "%s.%s", module, name);
        break;
    case FORMAT_COFF:
        text_append(out, "_A");
        while (*module != '\0') {
            size_t n = strcspn(module, ".");
            text_appendf(out, "%zu%.*s", n, (int)n, module);
            module += n + (module[n] == '.');
        }
        text_appendf(out, "_%s", name);
        break;
    }
    return true;
}
```

| Target | `scale` in `com.example.scale` | `length` in `geometry` |
|---|---|---|
| Linux, ELF | `com.example.scale.scale` | `geometry.length` |
| macOS, Mach-O | `_com.example.scale.scale` | `_geometry.length` |
| Windows, COFF | `_A3com7example5scale_scale` | `_A8geometry_length` |

The symbols of `scale` appear in the assembly that `antic -S --target <target> -I lib -o main.s tests/modules/main.anti` writes for the targets `linux-x86_64`, `macos-arm64` and `windows-x86_64`. The runtime entry `anti.rt.main` becomes `_A4anti2rt_main` on COFF. An `extern fn` and an `export fn` keep the C symbol of their name and are never mangled.

A Windows symbol contains only letters, digits and `_`. The length before each segment keeps two pairs apart that would otherwise give one symbol. Module `a_b` with function `c` becomes `_A3a_b_c`, and module `a` with function `b_c` becomes `_A1a_b_c`. After each segment comes either a digit of the next length or the `_` before the function name. A symbol therefore decodes to one pair. The C standard reserves every identifier that begins with `_` and an uppercase letter[^1]. A C program that defines such a name has undefined behaviour. No C function of a correct program therefore has the symbol of an Anti function. The unit test `test_target` checks all three forms, `com.example.geometry.vec` included.

## Library file format

Four sections make up every `.antl` file, always in this order.

| Section | Contents |
|---|---|
| Version stamp | The 4 bytes `ANTL` and the format version 13 |
| Package header | Name, version, dependencies, `license`, `license_text` and `attribution` |
| Public interface | Module path, imports, module doc text, type table and `pub` items with their doc text |
| IR | Symbolic values, aggregate types, globals, function signatures and bodies |

The fields follow each other without gaps. Every integer is little-endian with a fixed width of 1, 4 or 8 bytes. A string is a 4-byte length and its bytes. A float is stored as the 8 bytes of its IEEE 754 bit pattern. The file contains no pointer, no struct padding and no value in host byte order. Its bytes depend only on the source and the package header options, so every host writes the same file.

The IR of [chapter 7]({{% relref "/programming/writing-a-compiler/07-intermediate-representation" %}}) records types and never sizes, offsets or register classes. A size is a symbolic value such as `size_of i8`, and `c_long` stays the type `clong` in the file. The back end folds both for its target, so one library file serves all six targets.

### Package header

A package is named by a module path, the root of all its modules. One package version publishes the library files of all of them, and each file names its package in the header. The structure `struct package` in `src/sema.h` declares the fields.

```c
/* The package header of a library file. antic alone writes the module
   path as the name, version 0.0.0 and empty licence fields. */
struct package {
    const char *name;
    const char *version;
    const struct package_dependency *dependencies;
    size_t dependency_count;
    const char *license;            /* an SPDX identifier */
    const char *license_text;       /* the full text */
    const char *const *attribution; /* lines copied verbatim */
    size_t attribution_count;
};
```

Six options of antic fill the fields. The build tool `anti` of chapter 24 passes the values of its manifest `anti.toml`, and antic without the options writes the defaults.

| Option | Field | Without the option |
|---|---|---|
| `--package-name <p>` | `name` | the module path |
| `--package-version <v>` | `version` | `0.0.0` |
| `--dependency <name>,<constraint>,<url>` | one dependency, once per option | no dependencies |
| `--license <spdx>` | `license` | an empty string |
| `--license-text <file>` | `license_text`, the content of the file | an empty string |
| `--attribution <line>` | one attribution line, once per option | no lines |

A dependency names a package, a version constraint such as `^1.2` and the URL of its repository. The header copies the licence text itself, never the path of its file, so the library file does not refer to the source tree. The function `put_header` in `src/antl.c` writes the version stamp, the package header and the first fields of the interface.

```c
/* The magic, the version, the package header, the module path, the
   imports and the module's doc text. */
static void put_header(struct writer *w, const struct interface *iface)
{
    const struct package *p = &iface->package;
    struct text *out = w->out;
    size_t i;

    text_append_bytes(out, magic, sizeof magic);
    put_u32(w, ANTL_VERSION);
    put_str(w, p->name != NULL ? p->name : iface->module);
    put_str(w, p->version != NULL ? p->version : "0.0.0");
    put_u32(w, (uint32_t)p->dependency_count);
    for (i = 0; i < p->dependency_count; i++) {
        put_str(w, p->dependencies[i].name);
        put_str(w, p->dependencies[i].constraint);
        put_str(w, p->dependencies[i].url);
    }
    put_str(w, p->license);
    put_str(w, p->license_text);
    put_u32(w, (uint32_t)p->attribution_count);
    for (i = 0; i < p->attribution_count; i++) {
        put_str(w, p->attribution[i]);
    }
    put_str(w, iface->module);
    put_u32(w, (uint32_t)iface->import_count);
    for (i = 0; i < iface->import_count; i++) {
        put_str(w, iface->imports[i]);
    }
    put_doc(w, iface->doc, iface->doc != NULL ? strlen(iface->doc) : 0);
}
```

The function `antl_header` reads these fields and stops before the type table. The driver uses it to find library files and to order them before it loads any interface. The function `antl_write_header` writes the same bytes on their own. A static library for C keeps them as the copy of its package header, which chapter 25, Libraries for C, covers.

### Public interface

The type table follows the module's doc text. It lists every type that a `pub` item mentions, and each record refers to other types by their index. The writer numbers the types depth-first in `visit_type`. A pointer, a slice, an array or a function type comes after the types it is built from. A struct comes before its field types, because a struct may hold a pointer to itself. The reader creates every table entry first and fills in the fields of each struct afterwards.

| Record | Fields in order |
|---|---|
| Built-in type | kind |
| Pointer or slice | kind, element type |
| Array | kind, element type, 1 and a symbolic length, or 0 and a u64 length |
| Function type | kind, parameter count, parameter types, result type |
| Struct or union of another module | kind, module path, name |
| Struct or union of the module | kind, module path, name, flags with 1 for a union and 2 for `packed`, `align` as u64, field count, fields |
| Field | name, type, bitfield width or 0, doc text |
| Item | kind, name, type, exported flag, doc text, then the parameter names of a function, and `variadic` for an `extern fn` or the value of a constant |

A struct of the module itself is stored with its fields, private structs included, since a `pub` item may reach one. Each field of a `pub` struct stores its `///` text, and a field of a private struct stores an empty string. A struct of another module is stored as its module path and name only. The reader looks it up in the interface loaded from that module's file, so both files refer to one struct type. Chapter 2 makes structs nominal, and a second copy with the same fields would be a different type.

The exported flag of an item is 1 for an `export` item. A function stores the name of each parameter as a string, one per parameter of its type. The generated header of chapter 25 and the documentation generator of chapter 24 print these names. A constant computed from `size_of` has the constant kind `CONST_SYMBOLIC` and stores its expression tree. Each node is a kind, a type and either a number, the type of `size_of`, or an operator token with one or two operand nodes. The importing module uses such a constant in its own types, and the unit test `keeps_symbolic_sizes` checks the resulting array type `type [size_of(sized.H) * 2]byte = array mul i64(size_of sized.H, 2) of i8`.

### Doc text

A library file stores doc comments as plain strings without their markers. The module's record stores the `//!` text. Each item stores the `///` text before it, and each field of a `pub` struct of the module stores its own. The `//#` and `//#!` notes for the library's developers are never written. Neither is the `///` text of a private function or of the fields of a private struct. The option `--strip-docs` writes every doc string as an empty string. The unit test `package_and_docs` checks that a stripped file equals the file of the same source without comments.

The two test modules `tests/modules/lines/com/example/geo.anti` and `tests/modules/blocks/com/example/geo.anti` have the same doc comments. The first uses the line forms of chapter 4.

```anti
//! Plane geometry.
//#! Built for the tests.
/// A point.
///
///     Two fields.
pub struct Point
{
    /// Across.
    x: int,
    y: int,
}

/// Dot product.
//# Not a note in the file.
pub fn dot(a: Point, b: Point) -> int
{
    return a.x * b.x + a.y * b.y;
}

/// Private, not in the file.
fn hidden() -> int
{
    return 1;
}
```

The second uses the block forms, with other indentation inside each block.

```anti
/*!
    Plane geometry.
*/
/*#!
    Built for the tests.
*/
/**
  A point.

      Two fields.
*/
pub struct Point
{
    /**
        Across.
    */
    x: int,
    y: int,
}

/**
Dot product.
*/
/*#
  Not a note in the file.
*/
pub fn dot(a: Point, b: Point) -> int
{
    return a.x * b.x + a.y * b.y;
}

/**
  Private, not in the file.
*/
fn hidden() -> int
{
    return 1;
}
```

The lexer strips the common leading whitespace of the lines of one comment, so both forms give one text. The tests `antl_docs_lines` and `antl_docs_blocks` compile both modules under their own search roots. The test `antl_docs_equivalent` compares the two files byte for byte.

```sh
antic -c -I tests/modules/lines -o lib/lines.antl tests/modules/lines/com/example/geo.anti
antic -c -I tests/modules/blocks -o lib/blocks.antl tests/modules/blocks/com/example/geo.anti
cmake -E compare_files lib/lines.antl lib/blocks.antl
```

Both files are 1056 bytes long with the module path `com.example.geo`. They contain four doc strings, and the text `Built for the tests.`, `Not a note in the file.` and `Private, not in the file.` appears in neither.

| Record | Doc text |
|---|---|
| Module | `Plane geometry.` |
| Struct `Point` | `A point.`, an empty line and `    Two fields.` |
| Field `x` | `Across.` |
| Field `y` | empty |
| Function `dot` | `Dot product.` |

With `--strip-docs` the file of the line form is 997 bytes long, which is 59 bytes less, the length of the four strings.

### IR section

The IR section starts with the two tables of the IR: the symbolic values and the aggregate types. A symbolic value is an integer that depends on the target, such as `size_of vec.V2` or `offset_of str.len`. An aggregate type is a struct, a union or an array. Both tables refer to each other by index, because an array length is a symbolic value and `size_of` names an aggregate.

| Record | Fields in order |
|---|---|
| Symbolic value | kind, IR type, u64 value, value type, field index, operation, operand `a`, operand `b` |
| Aggregate type | kind, name, `packed` flag, `align` as u64, length index, length as written, field count, fields |
| Aggregate field | name, value type, bitfield width or 0, extension |
| Value type | IR type, aggregate index or `ff ff ff ff` |
| Global | module path, name, u64 size, u64 alignment, bytes, relocation count, relocations of u64 offset and u32 global |
| Signature | flags, module path, name, result type, result aggregate, parameter count, parameters |
| Parameter | IR type, extension, aggregate index |
| Body | temporary count, the type of each temporary, block count, then per block an instruction count and the instructions |
| Instruction | operation, type, result temporary, three operands, value type, field index, argument count, arguments |
| Operand | kind, type, u64 payload |

The flags of a signature are 1 for a function without a body, 2 for a variadic C function and 4 for an `export fn`. Emission gives an exported function of a library its C symbol from this flag. The optimizer keeps such a function even when no Anti code calls it. All signatures precede all bodies. A body can therefore call a function that the file lists later, because the reader knows every function before it reads the first instruction. An instruction takes at least 49 bytes, and a call adds 10 bytes for each argument.

### Bytes of scale.antl

The test `antl_scale` compiles `tests/modules/com/example/scale.anti` with all six package header options. The script `tests/run_antl.cmake` runs antic and compares the file byte for byte with the listing in `tests/modules/scale.antl.hex`, 16 bytes per line.

```sh
antic -c -I tests/modules --package-name com.example --package-version 1.2.4 --license MIT --license-text tests/modules/LICENSE.txt --attribution "Copyright 2026 Example" --dependency com.example.vec,^1.2,https://anti.example.com/repo -o lib/com/example/scale.antl tests/modules/com/example/scale.anti
```

```text
41 4e 54 4c 0d 00 00 00 0b 00 00 00 63 6f 6d 2e
65 78 61 6d 70 6c 65 05 00 00 00 31 2e 32 2e 34
01 00 00 00 0f 00 00 00 63 6f 6d 2e 65 78 61 6d
70 6c 65 2e 76 65 63 04 00 00 00 5e 31 2e 32 1d
00 00 00 68 74 74 70 73 3a 2f 2f 61 6e 74 69 2e
65 78 61 6d 70 6c 65 2e 63 6f 6d 2f 72 65 70 6f
03 00 00 00 4d 49 54 3d 00 00 00 50 65 72 6d 69
73 73 69 6f 6e 20 69 73 20 67 72 61 6e 74 65 64
20 74 6f 20 75 73 65 20 74 68 69 73 20 74 65 73
74 20 66 69 6c 65 20 66 6f 72 20 61 6e 79 20 70
75 72 70 6f 73 65 2e 0a 01 00 00 00 16 00 00 00
43 6f 70 79 72 69 67 68 74 20 32 30 32 36 20 45
78 61 6d 70 6c 65 11 00 00 00 63 6f 6d 2e 65 78
61 6d 70 6c 65 2e 73 63 61 6c 65 00 00 00 00 00
00 00 00 02 00 00 00 06 16 01 00 00 00 00 00 00
00 00 00 00 00 02 00 00 00 02 05 00 00 00 53 43
41 4c 45 00 00 00 00 00 00 00 00 00 00 06 00 00
00 00 00 00 00 03 05 00 00 00 73 63 61 6c 65 01
00 00 00 00 00 00 00 00 01 00 00 00 78 00 00 00
00 00 00 00 00 00 00 00 00 01 00 00 00 00 11 00
00 00 63 6f 6d 2e 65 78 61 6d 70 6c 65 2e 73 63
61 6c 65 05 00 00 00 73 63 61 6c 65 04 ff ff ff
ff 01 00 00 00 04 00 ff ff ff ff 02 00 00 00 04
04 01 00 00 00 02 00 00 00 02 04 01 00 00 00 01
04 00 00 00 00 00 00 00 00 02 04 06 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 ff ff
ff ff 00 00 00 00 00 00 00 00 39 04 ff ff ff ff
01 04 01 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 ff
ff ff ff 00 00 00 00 00 00 00 00
```

| Bytes | Field | Value |
|---|---|---|
| 0 to 3 | Magic | `ANTL` |
| 4 to 7 | Version | 10 |
| 8 to 22 | Package name | `com.example` |
| 23 to 31 | Package version | `1.2.4` |
| 32 to 35 | Dependency count | 1 |
| 36 to 95 | Dependency | `com.example.vec`, `^1.2` and `https://anti.example.com/repo` |
| 96 to 102 | Licence | `MIT` |
| 103 to 167 | Licence text | the 61 bytes of `tests/modules/LICENSE.txt` |
| 168 to 171 | Attribution count | 1 |
| 172 to 197 | Attribution | `Copyright 2026 Example` |
| 198 to 218 | Module | `com.example.scale` |
| 219 to 222 | Import count | 0 |
| 223 to 226 | Module doc text | empty |
| 227 to 230 | Type count | 2 |
| 231 | Type 0 | 6, `TYPE_I64`, which is `int` |
| 232 to 244 | Type 1 | 22, `TYPE_FN`, one parameter of type 0 and the result type 0 |
| 245 to 248 | Item count | 2 |
| 249 to 276 | Item 0 | 2, `SYMBOL_CONST`, name `SCALE`, type 0, not exported, no doc text, then 0, `CONST_INT`, with the value 6 |
| 277 to 300 | Item 1 | 3, `SYMBOL_FN`, name `scale`, type 1, not exported, no doc text, then the parameter name `x` |
| 301 to 312 | Table counts | no symbolic values, no aggregates, no globals |
| 313 to 316 | Function count | 1 |
| 317 to 362 | Signature | flags 0, module `com.example.scale`, name `scale`, result `i64` and one `i64` parameter |
| 363 to 376 | Body | two temporaries of type `i64`, one block, two instructions |
| 377 to 425 | Instruction | `%1 = mul i64 %0, 6` |
| 426 to 474 | Instruction | `ret i64 %1` |

The byte 57 at offset 426 is the operation `IR_RET`. The four bytes `ff` after the type byte are `IR_NO_RESULT`, and the same bytes in a value type are `IR_NO_AGG`. The unit test `writes_format` compiles the same source as the module `scale` without header options. It compares the 295 bytes with the C array `scale_antl`, which has a comment on each field.

### Version stamp

The format stores the numeric values of C enums, such as type kinds, operations and operand kinds. Adding an operation in a later chapter changes the numbers, and an older file would then decode into different instructions. Eleven static assertions pin the last value or the count of each enum.

```c
/* DESIGN: the file is a flat sequence of little-endian integers of fixed
   width. A string is a u32 byte count and the bytes. A float is the u64
   of its IEEE 754 bits. No pointer, padding or host byte order reaches the
   file. It stores enum values of types.h, sema.h and ir.h. These checks
   fail when one of them changes, and the version changes with it. */
_Static_assert(TYPE_STRUCT == 23, "raise ANTL_VERSION, then update this");
_Static_assert(SYMBOL_MODULE == 6, "raise ANTL_VERSION, then update this");
_Static_assert(CONST_SYMBOLIC == 8, "raise ANTL_VERSION, then update this");
_Static_assert(SYMBOLIC_CAST == 4, "raise ANTL_VERSION, then update this");
_Static_assert(TOKEN_KIND_COUNT == 119, "raise ANTL_VERSION, then update this");
_Static_assert(IR_CWCHAR == 10, "raise ANTL_VERSION, then update this");
_Static_assert(IR_RET == 57, "raise ANTL_VERSION, then update this");
_Static_assert(IR_SYM == 7, "raise ANTL_VERSION, then update this");
_Static_assert(IR_EXT_ZERO == 2, "raise ANTL_VERSION, then update this");
_Static_assert(IR_CONST_AGG == 5, "raise ANTL_VERSION, then update this");
_Static_assert(IR_AGG_ARRAY == 2, "raise ANTL_VERSION, then update this");
_Static_assert(IR_SYM_OP == 3, "raise ANTL_VERSION, then update this");

static const uint8_t magic[4] = {'A', 'N', 'T', 'L'};
```

A value added to or removed from one of the enums moves its last value and stops the build until the assertion is updated. The same change raises `ANTL_VERSION` in `src/antl.h`. A reader refuses every other version with a message such as `has format version 10, and antic reads version 9`.

## Reader

A library file comes from outside the compiler, and a damaged file must not crash it. The reader checks every count against the bytes that remain. Each record takes at least a known number of bytes, so a count that cannot fit is damage and never becomes a huge allocation.

```c
/* A count of records that each take at least min bytes. A larger count
   cannot fit in the rest of the file, which keeps a damaged count from
   causing a huge allocation. */
static uint32_t get_count(struct reader *r, size_t min)
{
    uint32_t n = get_u32(r);

    if (!r->failed && n > (r->size - r->pos) / min) {
        damaged(r);
        return 0;
    }
    return n;
}
```

Every index is checked against its table. A type index must point to an earlier entry, and a temporary to a temporary of the function with the operand's type. A block, a function, a global, an aggregate or a symbolic value must lie below the count the file declared. An operation, a type and an operand kind must lie inside their enums, because the IR printer indexes arrays of names with them. A struct that contains itself, an extension on a parameter wider than 16 bits and a `slot` without a type are damage as well. Any failure produces the message `is damaged at byte N` with the position where reading stopped.

```c
struct interface *antl_read(const uint8_t *data, size_t size,
                            const struct interface *const *libraries,
                            size_t library_count, struct types *types,
                            struct arena *arena, struct ir_module *program,
                            char *error, size_t error_size)
{
    struct reader r;
    size_t i;

    memset(&r, 0, sizeof r);
    r.data = data;
    r.size = size;
    r.error = error;
    r.error_size = error_size;
    r.arena = arena;
    r.types = types;
    r.libraries = libraries;
    r.library_count = library_count;
    r.iface = arena_alloc(arena, sizeof *r.iface);
    read_header(&r, r.iface);
    for (i = 0; i < r.iface->import_count && !r.failed; i++) {
        struct name imported;
        imported.text = r.iface->imports[i];
        imported.length = strlen(imported.text);
        if (library(&r, &imported) == NULL) {
            fail(&r, "needs module `%s`", imported.text);
        }
    }
    if (!r.failed) {
        read_types(&r);
    }
    if (!r.failed) {
        read_items(&r);
    }
    if (!r.failed) {
        read_ir(&r, program);
    }
    if (!r.failed && r.pos != r.size) {
        damaged(&r);
    }
    return r.failed ? NULL : r.iface;
}
```

### Aggregate and symbolic tables

The reader appends the tables, functions and globals of each file to the IR module of the whole program. An index in the file is therefore not the index in the program. The function `read_tables` reads both tables as they are and keeps a map for each. It then maps every aggregate with `map_agg` and every symbolic value with `map_sym`.

```c
/* The program's index of aggregate agg of the file. The types an
   aggregate is built from are added before it. */
static uint32_t map_agg(struct reader *r, struct ir_module *program,
                        struct ir_maps *maps, uint32_t agg)
{
    struct ir_aggtype *t;
    size_t i;

    if (agg >= maps->agg_count || maps->agg_state[agg] == MAP_BUSY) {
        damaged(r);
        return 0;
    }
    if (maps->agg_state[agg] == MAP_DONE) {
        return maps->agg_map[agg];
    }
    maps->agg_state[agg] = MAP_BUSY;
    t = &maps->aggs[agg];
    for (i = 0; i < t->field_count && !r->failed; i++) {
        if (t->fields[i].type.type == IR_AGG) {
            t->fields[i].type.agg = map_agg(r, program, maps,
                                            t->fields[i].type.agg);
        }
    }
    if (r->failed) {
        return 0;
    }
    if (t->kind == IR_AGG_ARRAY) {
        uint32_t length = map_sym(r, program, maps, t->length);
        maps->agg_map[agg] = r->failed ? 0
                                       : ir_array_add(program, t->name,
                                                      t->fields[0].type,
                                                      length, t->length_text);
    } else {
        maps->agg_map[agg] = ir_struct_add(program, t->kind, t->name, t->fields,
                                           t->field_count, t->packed,
                                           t->align);
    }
    maps->agg_state[agg] = MAP_DONE;
    return maps->agg_map[agg];
}
```

A struct maps the aggregates of its fields first, and an array maps its length. The functions `ir_struct_add` and `ir_array_add` return the existing entry when the program already has an aggregate of that name. A struct name includes its module path, and an array name its length expression, so two entries with one name describe the same type. The state `MAP_BUSY` marks an aggregate in progress, and meeting it again is a cycle in the file.

The function `map_sym` works the same way. It maps the aggregate of a `size_of` or `offset_of` and the operands of an operation, then adds the value with `ir_sym_int`, `ir_sym_size_of`, `ir_sym_offset_of` or `ir_sym_op`. Each of them returns an equal entry when the program already has one. Two libraries that both use `size_of vec.V2` therefore share one symbolic value. Operands of kind `IR_SYM`, the value types of instructions and the aggregates of signatures go through the maps as the reader meets them. The unit test `keeps_literals` loads a library that returns a `str` and lowers a module that uses one. The program has one entry `type str = struct { ptr: ptr, len: i64 }`, and the functions of both modules use `offset_of str.len`.

### Function signatures

The function `read_signature` maps each signature of the file to a function of the program. A C function shares an existing entry with the same name. A function of another module maps to the definition that its own library file loaded earlier. A function with a body is added as a new definition. A second definition of `m.f` gives the error `` defines `m.f`, which another library defines ``.

The unit test `damaged_files` reads every prefix of the 295 bytes of `scale_antl`, from 8 bytes to 294, and each read fails with a message. It changes the version to 14 and the magic, which give `has format version 14, and antic reads version 13` and `is not a library file`. Three more changes make a read fail. The first sets the result type of the signature to `agg` without an aggregate. The second puts a sign extension on the `i64` parameter, and the third puts the temporary 9 into the `mul` instruction.

## Loading order

The driver reads the header of every library file that `find_libraries` returned. Two files with one module path and a file with the module path of the source are errors. The driver then repeats one pass over the files that are not loaded yet. A file whose imports are all loaded is read, and a pass that loads no file ends the search. The first file left behind produces the message. The table shows the library files of each test after the source file.

| Test | Library files | Message |
|---|---|---|
| `error_missing_library` | `lib/com/example/twice.antl` | `` lib/com/example/twice.antl needs module `com.example.scale` `` |
| `error_library_cycle` | `lib/com/example/twice.antl` for `scale.anti` | `` lib/com/example/twice.antl depends on `com.example.scale`, so the import forms a cycle `` |
| `error_duplicate_library` | `lib/com/example/scale.antl` twice | `` lib/com/example/scale.antl and lib/com/example/scale.antl both hold module `com.example.scale` `` |

The test `error_library_cycle_later` adds `lib/com/example/one.antl` from `tests/modules/com/example/one.anti` before `twice.antl`. The first file loads, and the cycle stops the second one with the same message. The command of `error_missing_library` names no search root, so antic finds no `scale.antl` to load.

## Tests

The unit tests in `tests/unit/test_modules.c` check the import rules and the messages above. The test `lowers_imports` lowers calls into an imported module, and `writes_format` compares the written bytes with the C array. A round trip reads a library into a new session and writes it again, and both files have the same bytes. The test `dependencies` loads two libraries and checks that a struct of the first one keeps its identity inside a struct of the second one. Further tests read back symbolic sizes, parameter extensions, string literals and function pointer signatures from a library file. The tests `package_and_docs`, `private_field_docs`, `module_paths`, `dotted_imports` and `unique_exports` cover the remaining sections of this chapter.

The ctest tests `antl_scale`, `antl_twice`, `dump_ir_modules`, `antl_docs_lines`, `antl_docs_blocks` and `antl_docs_equivalent` run the commands of this chapter. The tests `antl_one`, `antl_internal`, `warning_one_segment` and the six error tests above check the rules and messages. On the development Mac these 15 tests and the unit tests pass.

The workflow `.github/workflows/test.yml` runs the whole suite on runners for the six targets when it is started by hand. The test `antl_scale` then compares the file of each host with the same listing, and a file that differs fails it. Chapter 21, Testing six targets, describes the workflow.

## Next

[Chapter 10, The optimizer]({{% relref "/programming/writing-a-compiler/10-optimizer" %}}), adds constant folding, dead code elimination, copy propagation and a small set of peephole rules. All passes run on the whole program after the library IR is loaded, or on one module in dev mode. It states what each pass may assume and why no pass folds a symbolic size, and it shows the IR before and after in its tests.

## References

[^1]: ISO/IEC JTC1/SC22/WG14, *N1570, Committee Draft of ISO/IEC 9899:201x*, 2011, section 7.1.3, https://www.open-std.org/jtc1/sc22/wg14/www/docs/n1570.pdf
