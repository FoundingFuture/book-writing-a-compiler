#include "driver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "antl.h"
#include "diagnostic.h"
#include "emit.h"
#include "ir.h"
#include "lexer.h"
#include "linker.h"
#include "lower.h"
#include "header.h"
#include "modpath.h"
#include "notice.h"
#include "optimize.h"
#include "regalloc.h"
#include "select.h"
#include "parser.h"
#include "sema.h"
#include "types.h"
#include "process.h"
#include "text.h"

#define SOURCE_SUFFIX ".anti"
#define ASSEMBLY_SUFFIX ".s"
#define HEADER_SUFFIX ".h"
#define PACKAGE_SUFFIX ".package"
#define DEF_SUFFIX ".def"

/* DESIGN: a bundled runtime holds every member of the runtime library
   except two. The object of rt/start.c has the C main of executables, and
   rt/license.c needs the notice, which a static library has none of. */
#define RUNTIME_START_MEMBER "start."
#define RUNTIME_LICENSE_MEMBER "license."

/* What a compilation writes beside the assembly. A library for C has a
   name, a header and a package header copy. A linked binary has the
   licence notice and the names of its export fns. */
struct extras {
    struct text name;
    struct text header;
    struct text package;
    struct text notice;
    struct text exports;
};

static void extras_free(struct extras *e)
{
    text_free(&e->name);
    text_free(&e->header);
    text_free(&e->package);
    text_free(&e->notice);
    text_free(&e->exports);
}


static bool ends_with(const char *s, const char *suffix)
{
    size_t n = strlen(s);
    size_t m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

/* Read the whole of a binary file. */
static bool read_bytes(const char *path, struct text *out)
{
    FILE *f = fopen(path, "rb");
    char buffer[4096];
    size_t n;
    bool ok;

    if (f == NULL) {
        fprintf(stderr, "antic: cannot open %s\n", path);
        return false;
    }
    while ((n = fread(buffer, 1, sizeof buffer, f)) > 0) {
        text_append_bytes(out, buffer, n);
    }
    ok = !ferror(f);
    if (!ok) {
        fprintf(stderr, "antic: cannot read %s\n", path);
    }
    fclose(f);
    return ok;
}

/* Read the whole file. A source file holding a NUL byte is rejected,
   because the text buffer ends at the first NUL. */
static bool read_source(const char *path, struct text *out)
{
    FILE *f = fopen(path, "rb");
    char buffer[4096];
    size_t n;
    bool ok = true;

    if (f == NULL) {
        fprintf(stderr, "antic: cannot open %s\n", path);
        return false;
    }
    while ((n = fread(buffer, 1, sizeof buffer - 1, f)) > 0) {
        buffer[n] = '\0';
        if (strlen(buffer) != n) {
            fprintf(stderr, "antic: %s contains a NUL byte\n", path);
            ok = false;
            break;
        }
        text_append(out, buffer);
    }
    if (ferror(f)) {
        fprintf(stderr, "antic: cannot read %s\n", path);
        ok = false;
    }
    fclose(f);
    return ok;
}

static bool write_file(const char *path, const struct text *content)
{
    FILE *f = fopen(path, "wb");
    bool ok;

    if (f == NULL) {
        fprintf(stderr, "antic: cannot create %s\n", path);
        return false;
    }
    ok = fwrite(text_cstr(content), 1, content->length, f) == content->length;
    ok = fclose(f) == 0 && ok;
    if (!ok) {
        fprintf(stderr, "antic: cannot write %s\n", path);
    }
    return ok;
}

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

/* Run xcrun and keep its output without the final newline. */
static bool xcrun(const char *query, struct text *out)
{
    const char *argv[] = {"xcrun", "--sdk", "macosx", query, NULL};

    if (process_capture(argv, out) != 0) {
        fprintf(stderr, "antic: xcrun %s failed\n", query);
        return false;
    }
    while (out->length > 0 && out->data[out->length - 1] == '\n') {
        out->data[--out->length] = '\0';
    }
    return true;
}

/* The directory of the glibc start files for a Linux target. */
static bool find_crt_dir(enum target t, struct text *out)
{
    const char *const *dirs = link_crt_dirs(t);
    size_t i;

    for (i = 0; dirs[i] != NULL; i++) {
        struct text path = {0};
        FILE *f;
        text_appendf(&path, "%s/Scrt1.o", dirs[i]);
        f = fopen(text_cstr(&path), "rb");
        text_free(&path);
        if (f != NULL) {
            fclose(f);
            text_append(out, dirs[i]);
            return true;
        }
    }
    fprintf(stderr, "antic: cannot find Scrt1.o of the C library in %s, %s "
                    "or %s\n", dirs[0], dirs[1], dirs[2]);
    return false;
}

/* Whether the command ends with a link, rather than a dump, a library
   file or an assembly file. */
static bool links(const struct options *o)
{
    return !o->assembly_only && !o->dump_tokens && !o->dump_ast &&
           !o->dump_types && !o->dump_ir && !o->dump_opt && !o->dump_select &&
           !o->dump_alloc && !o->library && o->lib == LIB_NONE;
}

/* DESIGN: lld links for every target from any host, with the sysroot of
   the runtime archive. The platform linker links only for the operating
   system it runs on, where the C library of the target is installed. */
static bool can_link(const struct options *o)
{
    enum target host;

    if (o->linker == LINKER_LLD) {
        return true;
    }
    if (!target_host(&host) ||
        target_info(host)->os != target_info(o->target)->os) {
        fprintf(stderr, "antic: linking for %s with the platform linker needs "
                        "a host with the same operating system. -S writes "
                        "the assembly without linking.\n",
                target_name(o->target));
        return false;
    }
    return true;
}

/* The texts behind the facts of a link. */
struct link_facts {
    struct text sdk_path;
    struct text sdk_version;
    struct text crt_dir;
    struct text sysroot;
    struct text lld_dir;
};

static void link_facts_free(struct link_facts *f)
{
    text_free(&f->sdk_path);
    text_free(&f->sdk_version);
    text_free(&f->crt_dir);
    text_free(&f->sysroot);
    text_free(&f->lld_dir);
}

static bool file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");

    if (f != NULL) {
        fclose(f);
    }
    return f != NULL;
}

/* The facts of a link for the target. lld takes the sysroot and the lld
   programs of the runtime archive, and the SDK version of a macOS
   sysroot. The platform linker takes the macOS SDK from xcrun and the
   glibc start files of the host. */
static bool link_facts(const struct options *o, struct link_inputs *in,
                       struct link_facts *f)
{
    enum target t = o->target;
    enum target_os os = target_info(t)->os;
    static const char *const flavours[] = {
        [OS_LINUX] = "ld.lld", [OS_MACOS] = "ld64.lld",
        [OS_WINDOWS] = "lld-link",
    };
    struct text marker = {0};
    bool present;

    memset(f, 0, sizeof *f);
    in->linker = o->linker;
    if (o->linker == LINKER_PLATFORM) {
        if (os == OS_MACOS) {
            if (!xcrun("--show-sdk-path", &f->sdk_path) ||
                !xcrun("--show-sdk-version", &f->sdk_version)) {
                return false;
            }
            in->sdk_path = text_cstr(&f->sdk_path);
            in->sdk_version = text_cstr(&f->sdk_version);
        } else if (os == OS_LINUX) {
            if (!find_crt_dir(t, &f->crt_dir)) {
                return false;
            }
            in->crt_dir = text_cstr(&f->crt_dir);
        }
        return true;
    }
    text_appendf(&f->lld_dir, "%s/%s", o->runtime, RUNTIME_BIN_DIR);
    text_appendf(&marker, "%s/%s", text_cstr(&f->lld_dir), flavours[os]);
    in->lld_dir = file_exists(text_cstr(&marker)) ? text_cstr(&f->lld_dir)
                                                  : NULL;
    text_free(&marker);
    text_appendf(&f->sysroot, "%s/%s/%s", o->runtime, RUNTIME_SYSROOT_DIR,
                 target_name(t));
    text_appendf(&marker, "%s/%s", text_cstr(&f->sysroot),
                 os == OS_LINUX   ? "usr/lib/libc.a"
                 : os == OS_MACOS ? SYSROOT_SDK_VERSION
                 : target_info(t)->arch == ARCH_ARM64
                     ? "crt/lib/aarch64/libcmt.lib"
                     : "crt/lib/x86_64/libcmt.lib");
    present = file_exists(text_cstr(&marker)) &&
              (os != OS_MACOS || read_bytes(text_cstr(&marker), &f->sdk_version));
    text_free(&marker);
    /* DESIGN: without a Windows sysroot lld-link reads the library
       directories of LIB, which an MSVC environment sets. */
    if (!present && os != OS_WINDOWS) {
        fprintf(stderr, "antic: linking for %s with lld needs the sysroot "
                        "%s, which tools/get-sysroot.sh installs\n",
                target_name(t), text_cstr(&f->sysroot));
        return false;
    }
    in->sysroot = present ? text_cstr(&f->sysroot) : NULL;
    while (f->sdk_version.length > 0 &&
           (f->sdk_version.data[f->sdk_version.length - 1] == '\n' ||
            f->sdk_version.data[f->sdk_version.length - 1] == '\r')) {
        f->sdk_version.data[--f->sdk_version.length] = '\0';
    }
    in->sdk_version = text_cstr(&f->sdk_version);
    return true;
}

static bool link_program(const struct options *o, const char *object,
                         const char *executable)
{
    struct link_inputs in;
    struct link_command command;
    struct link_facts facts;
    bool ok;

    memset(&in, 0, sizeof in);
    in.object = object;
    in.executable = executable;
    in.runtime = o->runtime;
    in.extra = o->objects;
    in.extra_count = o->object_count;
    ok = link_facts(o, &in, &facts);
    if (ok) {
        link_command(&command, o->target, &in);
        ok = process_run(command.argv) == 0;
        if (!ok) {
            fprintf(stderr, "antic: the linker failed\n");
        }
        link_command_free(&command);
    }
    link_facts_free(&facts);
    return ok;
}

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

/* Print one token per line: its position, its group and its source text.
   A doc comment spans lines, so it prints its first line. Chapter 1 shows
   this output for the function scale. */
static void dump_tokens(const char *source, const struct token_list *tokens)
{
    size_t i;

    for (i = 0; i + 1 < tokens->count; i++) {
        const struct token *t = &tokens->items[i];
        const char *end = memchr(source + t->offset, '\n', t->length);
        size_t length = end != NULL ? (size_t)(end - (source + t->offset))
                                    : t->length;
        char position[32];

        snprintf(position, sizeof position, "%d:%d", t->line, t->column);
        printf("%-5s %-8s %.*s\n", position, token_category(t->kind),
               (int)length, source + t->offset);
    }
}

/* Lower the module into ir and verify the result. */
static bool lower_checked(const char *input, struct module *tree,
                          const char *module, struct ir_module *ir,
                          struct diagnostics *diags)
{
    struct text errors = {0};
    bool ok = false;

    if (!lower_module(tree, module, ir, diags)) {
        print_diagnostics(input, diags);
    } else if (!ir_verify(ir, &errors)) {
        fprintf(stderr, "antic: internal error, the IR of %s fails "
                        "verification\n%s", input, text_cstr(&errors));
    } else {
        ok = true;
    }
    text_free(&errors);
    return ok;
}

/* Lower the module after the loaded libraries and print the whole
   program, optimized when optimize is set. Returns 2, the status of a
   finished dump, on success. */
static int dump_ir(const char *input, struct module *tree, const char *module,
                   struct ir_module *program, bool optimize,
                   struct diagnostics *diags)
{
    struct text out = {0};
    struct text errors = {0};

    if (!lower_checked(input, tree, module, program, diags)) {
        return 1;
    }
    if (optimize) {
        ir_optimize(program, module);
        if (!ir_verify(program, &errors)) {
            fprintf(stderr, "antic: internal error, the optimized IR of %s "
                            "fails verification\n%s", input,
                    text_cstr(&errors));
            text_free(&errors);
            return 1;
        }
    }
    ir_print(&out, program);
    fputs(text_cstr(&out), stdout);
    text_free(&out);
    return 2;
}

/* Whether the program has a function main in the main module. */
static bool has_main(const struct ir_module *program, const char *module)
{
    size_t i;

    for (i = 0; i < program->function_count; i++) {
        const struct ir_function *f = program->functions[i];
        if (!f->is_extern && f->module != NULL &&
            strcmp(f->module, module) == 0 && strcmp(f->name, "main") == 0) {
            return true;
        }
    }
    return false;
}

/* Lower and optimize the program and run the back end for the target:
   instruction selection, register allocation and emission. The dumps
   print the machine code instead, before allocation for --dump-select.
   Returns 0 with the assembly, 2 after a dump and 1 after an error. */
static int back_end(const struct options *o, struct module *tree,
                    const char *module, struct ir_module *program,
                    struct diagnostics *diags, struct text *assembly,
                    struct extras *extras)
{
    struct mach_function **functions;
    struct text out = {0};
    char error[200];
    bool dump = o->dump_select || o->dump_alloc;
    bool ok;
    int status = 1;
    size_t i;

    if (tree != NULL &&
        !lower_checked(o->input, tree, module, program, diags)) {
        return 1;
    }
    if (o->dev) {
        ir_optimize_module(program, module);
    } else {
        ir_optimize(program, module);
    }
    if (!dump && !o->assembly_only && !o->dev && o->lib == LIB_NONE &&
        !has_main(program, module)) {
        fprintf(stderr, "antic: %s: the program has no function `main`\n",
                o->input);
        return 1;
    }
    functions = calloc(program->function_count + 1, sizeof *functions);
    if (functions == NULL) {
        fputs("antic: out of memory\n", stderr);
        exit(70);
    }
    ok = select_module(o->target, program, functions, error, sizeof error);
    for (i = 0; ok && !(o->dump_select && !o->dump_alloc) &&
                i < program->function_count;
         i++) {
        if (functions[i] != NULL) {
            ok = regalloc_function(o->target, functions[i], error,
                                   sizeof error);
        }
    }
    if (ok && dump) {
        for (i = 0; i < program->function_count; i++) {
            if (functions[i] != NULL) {
                mach_print(&out, target_desc(o->target), program,
                           functions[i]);
            }
        }
        fputs(text_cstr(&out), stdout);
        status = 2;
    } else if (ok) {
        ok = o->dev ? emit_module(assembly, o->target, program, functions,
                                  module, error, sizeof error)
                    : emit_program(assembly, o->target, program, functions,
                                   module, error, sizeof error);
        if (ok && o->dev && !has_main(program, module)) {
            status = 3;
        }
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
    }
    if (!ok) {
        fprintf(stderr, "antic: %s\n", error);
    }
    for (i = 0; i < program->function_count; i++) {
        if (functions[i] != NULL) {
            mach_function_free(functions[i]);
            free(functions[i]);
        }
    }
    free(functions);
    text_free(&out);
    return status;
}

/* The package header from the options. A field without an option stays
   NULL, and sema_interface fills it in. --license-text names a file whose
   content goes into the header. */
static bool package_header(const struct options *o, struct arena *arena,
                           struct package *out)
{
    struct package_dependency *deps =
        arena_alloc(arena, (o->dependency_count + 1) * sizeof *deps);
    size_t i;

    memset(out, 0, sizeof *out);
    out->name = o->package_name;
    out->version = o->package_version;
    out->license = o->license;
    for (i = 0; i < o->dependency_count; i++) {
        const char *spec = o->dependencies[i];
        const char *first = strchr(spec, ',');
        const char *second = first != NULL ? strchr(first + 1, ',') : NULL;
        char *copy;
        if (second == NULL) {
            fprintf(stderr, "antic: --dependency %s is not "
                            "<name>,<constraint>,<url>\n", spec);
            return false;
        }
        copy = arena_alloc(arena, strlen(spec) + 1);
        memcpy(copy, spec, strlen(spec));
        copy[first - spec] = '\0';
        copy[second - spec] = '\0';
        deps[i].name = copy;
        deps[i].constraint = copy + (first - spec) + 1;
        deps[i].url = copy + (second - spec) + 1;
    }
    out->dependencies = deps;
    out->dependency_count = o->dependency_count;
    out->attribution = o->attribution;
    out->attribution_count = o->attribution_count;
    if (o->license_text != NULL) {
        struct text text = {0};
        char *copy;
        if (!read_source(o->license_text, &text)) {
            return false;
        }
        copy = arena_alloc(arena, text.length + 1);
        memcpy(copy, text_cstr(&text), text.length);
        out->license_text = copy;
        text_free(&text);
    }
    return true;
}

static bool own_interface(const struct options *o, struct module *tree,
                          const char *module, struct arena *arena,
                          struct interface *out);

/* Write the library file of the module, by default beside the source.
   Returns 2, the status of a finished command without linking. */
static int write_library(const struct options *o, struct module *tree,
                         const char *module, struct arena *arena,
                         struct diagnostics *diags)
{
    struct ir_module ir;
    struct interface iface;
    struct text bytes = {0};
    struct text path = {0};
    int status = 1;

    ir_module_init(&ir, arena, module);
    if (lower_checked(o->input, tree, module, &ir, diags) &&
        own_interface(o, tree, module, arena, &iface)) {
        antl_write(&bytes, &iface, &ir, o->strip_docs);
        if (o->output != NULL) {
            text_append(&path, o->output);
        } else {
            text_appendf(&path, "%.*s%s",
                         (int)(strlen(o->input) - strlen(SOURCE_SUFFIX)),
                         o->input, ANTL_SUFFIX);
        }
        status = write_file(text_cstr(&path), &bytes) ? 2 : 1;
    }
    text_free(&bytes);
    text_free(&path);
    ir_module_free(&ir);
    return status;
}

struct paths {
    const char **items;
    size_t count;
    size_t capacity;
};

static void add_path(struct paths *p, const char *path)
{
    if (p->count == p->capacity) {
        p->capacity = p->capacity == 0 ? 16 : p->capacity * 2;
        p->items = realloc((void *)p->items, p->capacity * sizeof *p->items);
        if (p->items == NULL) {
            fputs("antic: out of memory\n", stderr);
            exit(70);
        }
    }
    p->items[p->count++] = path;
}

/* The library file of module path module under the first search root that
   has one, in the memory pool, or NULL. */
static const char *search_roots(const struct options *o, const char *module,
                                struct arena *arena)
{
    struct text std_root = {0};
    size_t i;

    /* DESIGN: the standard library in std/ of the runtime archive is the
       last search root, so a program imports anti.io without -I. */
    if (o->runtime != NULL) {
        text_appendf(&std_root, "%s/%s", o->runtime, RUNTIME_STD_DIR);
    }
    for (i = 0; i < o->root_count + (o->runtime != NULL); i++) {
        struct text path = {0};
        const char *p;
        FILE *f;
        char *copy;
        text_appendf(&path, "%s/", i < o->root_count ? o->roots[i]
                                                     : text_cstr(&std_root));
        for (p = module; *p != '\0'; p++) {
            text_appendf(&path, "%c", *p == '.' ? '/' : *p);
        }
        text_append(&path, ANTL_SUFFIX);
        f = fopen(text_cstr(&path), "rb");
        if (f != NULL) {
            fclose(f);
            copy = arena_alloc(arena, path.length + 1);
            memcpy(copy, text_cstr(&path), path.length + 1);
            text_free(&path);
            text_free(&std_root);
            return copy;
        }
        text_free(&path);
    }
    text_free(&std_root);
    return NULL;
}

static bool contains(const struct paths *p, const char *s)
{
    size_t i;

    for (i = 0; i < p->count; i++) {
        if (strcmp(p->items[i], s) == 0) {
            return true;
        }
    }
    return false;
}

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

/* Read the library files and load each after the libraries it imports,
   whatever the order on the command line. The interfaces go to out in
   load order. */
static bool load_libraries(const struct paths *paths, const char *module,
                           struct arena *arena, struct types *types,
                           struct ir_module *program,
                           const struct interface **out)
{
    size_t n = paths->count;
    struct text *files = calloc(n + 1, sizeof *files);
    struct interface *headers = calloc(n + 1, sizeof *headers);
    bool *loaded = calloc(n + 1, sizeof *loaded);
    size_t count = 0;
    size_t i;
    size_t j;
    size_t k;
    char error[200];
    bool ok = files != NULL && headers != NULL && loaded != NULL;

    if (!ok) {
        fputs("antic: out of memory\n", stderr);
        exit(70);
    }
    for (i = 0; i < n && ok; i++) {
        ok = read_bytes(paths->items[i], &files[i]);
        if (ok && !antl_header((const uint8_t *)files[i].data,
                               files[i].length, arena, &headers[i], error,
                               sizeof error)) {
            fprintf(stderr, "antic: %s %s\n", paths->items[i], error);
            ok = false;
        }
    }
    for (i = 0; i < n && ok; i++) {
        if (strcmp(headers[i].module, module) == 0) {
            fprintf(stderr, "antic: %s holds module `%s`, which this command "
                            "compiles\n", paths->items[i], module);
            ok = false;
        }
        for (j = 0; j < i && ok; j++) {
            if (strcmp(headers[i].module, headers[j].module) == 0) {
                fprintf(stderr, "antic: %s and %s both hold module `%s`\n",
                        paths->items[j], paths->items[i], headers[i].module);
                ok = false;
            }
        }
    }
    while (ok && count < n) {
        size_t before = count;
        for (i = 0; i < n && ok; i++) {
            bool ready = !loaded[i];
            for (j = 0; j < headers[i].import_count && ready; j++) {
                bool found = false;
                for (k = 0; k < count && !found; k++) {
                    found = strcmp(out[k]->module, headers[i].imports[j]) == 0;
                }
                ready = found;
            }
            if (!ready) {
                continue;
            }
            out[count] = antl_read((const uint8_t *)files[i].data,
                                   files[i].length, out, count, types, arena,
                                   program, error, sizeof error);
            if (out[count] == NULL) {
                fprintf(stderr, "antic: %s %s\n", paths->items[i], error);
                ok = false;
            } else {
                loaded[i] = true;
                count++;
            }
        }
        if (ok && count == before) {
            /* Report the first import that no file provides, or else the
               cycle that blocks the remaining files. */
            for (i = 0; i < n && ok; i++) {
                for (j = 0; j < headers[i].import_count && !loaded[i]; j++) {
                    bool provided = false;
                    if (strcmp(headers[i].imports[j], module) == 0) {
                        fprintf(stderr, "antic: %s depends on `%s`, so the "
                                        "import forms a cycle\n",
                                paths->items[i], module);
                        ok = false;
                        break;
                    }
                    for (k = 0; k < n && !provided; k++) {
                        provided = strcmp(headers[k].module,
                                          headers[i].imports[j]) == 0;
                    }
                    if (!provided) {
                        fprintf(stderr, "antic: %s needs module `%s`\n",
                                paths->items[i], headers[i].imports[j]);
                        ok = false;
                        break;
                    }
                }
            }
            if (ok) {
                fputs("antic: the library files import each other in a "
                      "cycle\n", stderr);
                ok = false;
            }
        }
    }
    for (i = 0; i < n; i++) {
        text_free(&files[i]);
    }
    free(files);
    free(headers);
    free(loaded);
    return ok;
}

static bool defines_main(const struct module *tree)
{
    size_t i;

    for (i = 0; i < tree->item_count; i++) {
        const struct item *it = tree->items[i];
        if (it->kind == ITEM_FN && it->name.length == 4 &&
            memcmp(it->name.text, "main", 4) == 0) {
            return true;
        }
    }
    return false;
}

/* The name of a library for C: the file name of -o without lib and its
   suffix, or the last segment of the module path. */
static void library_name(const struct options *o, const char *module,
                         struct text *out)
{
    const char *base;
    const char *dot;
    size_t n;

    if (o->output == NULL) {
        text_append(out, module_path_last(module));
        return;
    }
    base = strrchr(o->output, '/');
    base = base != NULL ? base + 1 : o->output;
    if (target_info(o->target)->format != FORMAT_COFF &&
        strncmp(base, "lib", 3) == 0) {
        base += 3;
    }
    dot = strchr(base, '.');
    n = dot != NULL ? (size_t)(dot - base) : strlen(base);
    text_appendf(out, "%.*s", (int)n, base);
}

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

static int compile(const struct options *o, struct text *source,
                   struct text *module, struct text *assembly,
                   struct text *base, struct extras *extras)
{
    struct arena arena = {0};
    struct diagnostics diags = {0};
    struct token_list tokens = {0};
    struct module *tree = NULL;
    struct types types;
    struct ir_module program;
    const struct interface **libraries = NULL;
    struct paths paths = {0};
    int status = 1;

    ir_module_init(&program, &arena, "");

    if (!read_source(o->input, source)) {
        return 1;
    }
    if (!lex(text_cstr(source), source->length, &arena, &diags, &tokens)) {
        print_diagnostics(o->input, &diags);
        goto done;
    }
    if (o->dump_tokens) {
        dump_tokens(text_cstr(source), &tokens);
        status = 2;
        goto done;
    }
    if (!parse(text_cstr(source), &tokens, &arena, &diags, &tree)) {
        print_diagnostics(o->input, &diags);
        goto done;
    }
    if (o->dump_ast) {
        struct text dump = {0};
        ast_dump(&dump, tree);
        fputs(text_cstr(&dump), stdout);
        text_free(&dump);
        status = 2;
        goto done;
    }
    if (!module_name(o, module)) {
        goto done;
    }
    types_init(&types, &arena);
    if (!find_libraries(o, tree, &arena, &paths)) {
        goto done;
    }
    libraries = malloc((paths.count + 1) * sizeof *libraries);
    if (libraries == NULL) {
        fputs("antic: out of memory\n", stderr);
        exit(70);
    }
    if (!load_libraries(&paths, text_cstr(module), &arena, &types, &program,
                        libraries)) {
        goto done;
    }
    if (!sema_check(tree, text_cstr(module), libraries, paths.count,
                    &types, &arena, &diags)) {
        print_diagnostics(o->input, &diags);
        goto done;
    }
    if (o->doc_warnings) {
        sema_doc_warnings(tree, &diags);
    }
    print_diagnostics(o->input, &diags);
    diags.count = 0;
    if (o->dump_types) {
        struct text dump = {0};
        ast_dump_typed(&dump, tree);
        fputs(text_cstr(&dump), stdout);
        text_free(&dump);
        status = 2;
        goto done;
    }
    if (o->library) {
        status = write_library(o, tree, text_cstr(module), &arena, &diags);
        goto done;
    }
    if (o->dump_ir || o->dump_opt) {
        status = dump_ir(o->input, tree, text_cstr(module), &program,
                         o->dump_opt, &diags);
        goto done;
    }
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
    if (status != 0 && status != 3) {
        goto done;
    }
    if (o->lib != LIB_NONE && !o->assembly_only) {
        const char *slash = strrchr(o->output != NULL ? o->output : o->input,
                                    '/');
        const char *from = o->output != NULL ? o->output : o->input;
        if (slash != NULL) {
            text_appendf(base, "%.*s/", (int)(slash - from), from);
        }
        text_append(base, text_cstr(&extras->name));
    } else if (o->output != NULL) {
        text_append(base, o->output);
    } else {
        text_appendf(base, "%.*s",
                     (int)(strlen(o->input) - strlen(SOURCE_SUFFIX)),
                     o->input);
    }

done:
    ir_module_free(&program);
    free((void *)libraries);
    free((void *)paths.items);
    token_list_free(&tokens);
    diagnostics_free(&diags);
    arena_free(&arena);
    return status;
}

/* DESIGN: dev mode also compiles a library file of the dependency graph
   into its own object. The IR in the file is the module, and the other
   library files declare what it calls. A main module comes from source,
   so this object never links. */
static int compile_library_file(const struct options *o,
                                struct text *assembly, struct text *base,
                                struct extras *extras)
{
    struct arena arena = {0};
    struct diagnostics diags = {0};
    struct types types;
    struct ir_module program;
    struct module empty;
    struct options with_input = *o;
    struct interface header;
    struct text bytes = {0};
    const struct interface **libraries = NULL;
    struct paths paths = {0};
    char error[200];
    int status = 1;

    memset(&empty, 0, sizeof empty);
    ir_module_init(&program, &arena, "");
    types_init(&types, &arena);
    with_input.libraries =
        malloc((o->library_count + 1) * sizeof *with_input.libraries);
    if (with_input.libraries == NULL) {
        fputs("antic: out of memory\n", stderr);
        exit(70);
    }
    with_input.libraries[0] = o->input;
    memcpy((void *)(with_input.libraries + 1), (void *)o->libraries,
           o->library_count * sizeof *o->libraries);
    with_input.library_count = o->library_count + 1;
    if (!read_bytes(o->input, &bytes)) {
        goto done;
    }
    if (!antl_header((const uint8_t *)bytes.data, bytes.length, &arena,
                     &header, error, sizeof error)) {
        fprintf(stderr, "antic: %s %s\n", o->input, error);
        goto done;
    }
    if (!find_libraries(&with_input, &empty, &arena, &paths)) {
        goto done;
    }
    libraries = malloc((paths.count + 1) * sizeof *libraries);
    if (libraries == NULL) {
        fputs("antic: out of memory\n", stderr);
        exit(70);
    }
    if (!load_libraries(&paths, "", &arena, &types, &program, libraries)) {
        goto done;
    }
    status = back_end(o, NULL, header.module, &program, &diags, assembly,
                      extras);
    if (status == 0 || status == 3) {
        status = 3;
        if (o->output != NULL) {
            text_append(base, o->output);
        } else {
            text_appendf(base, "%.*s",
                         (int)(strlen(o->input) - strlen(ANTL_SUFFIX)),
                         o->input);
        }
    }

done:
    ir_module_free(&program);
    free((void *)libraries);
    free((void *)paths.items);
    free((void *)with_input.libraries);
    text_free(&bytes);
    diagnostics_free(&diags);
    arena_free(&arena);
    return status;
}

/* Run llvm-mc on the assembly file for the target. */
static bool assemble(const struct options *o, const char *assembly,
                     const char *object)
{
    struct text triple = {0};
    const char *argv[] = {o->llvm_mc != NULL ? o->llvm_mc : "llvm-mc", NULL,
                          "-filetype=obj", "-o", object, assembly, NULL};
    int run;

    text_appendf(&triple, "-triple=%s", target_info(o->target)->triple);
    argv[1] = text_cstr(&triple);
    run = process_run(argv);
    text_free(&triple);
    if (run != 0) {
        fprintf(stderr, "antic: llvm-mc failed\n");
        return false;
    }
    return true;
}

static bool run_command(const struct link_command *c, const char *what)
{
    if (process_run(c->argv) != 0) {
        fprintf(stderr, "antic: %s failed\n", what);
        return false;
    }
    return true;
}

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

int driver_run(const struct options *o)
{
    struct text source = {0};
    struct text module = {0};
    struct text assembly = {0};
    struct text base = {0};
    struct text asm_path = {0};
    struct text obj_path = {0};
    struct extras extras;
    bool object_only = false;
    int status = 1;

    memset(&extras, 0, sizeof extras);

    if (!ends_with(o->input, SOURCE_SUFFIX) &&
        !(o->dev && ends_with(o->input, ANTL_SUFFIX))) {
        fprintf(stderr, "antic: %s: expected a %s file\n", o->input,
                SOURCE_SUFFIX);
        return 2;
    }
    if (links(o) && !o->dev && o->runtime == NULL) {
        fprintf(stderr, "antic: linking needs --runtime <dir>, the directory "
                        "that holds %s/<target>/\n",
                RUNTIME_LIB_DIR);
        return 2;
    }
    if (links(o) && !o->dev && !can_link(o)) {
        return 2;
    }
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
    switch (ends_with(o->input, ANTL_SUFFIX)
                ? compile_library_file(o, &assembly, &base, &extras)
                : compile(o, &source, &module, &assembly, &base, &extras)) {
    case 0:
        break;
    case 2: /* a dump or a library file finished the command */
        status = 0;
        goto done;
    case 3: /* dev mode: a module without main stops at its object */
        object_only = true;
        break;
    default:
        goto done;
    }
    if (o->dev && !object_only && !o->assembly_only &&
        (o->runtime == NULL || !can_link(o))) {
        if (o->runtime == NULL) {
            fprintf(stderr, "antic: linking needs --runtime <dir>, the "
                            "directory that holds %s/<target>/\n",
                    RUNTIME_LIB_DIR);
        }
        goto done;
    }
    if (o->assembly_only) {
        if (o->output == NULL) {
            text_append(&base, ASSEMBLY_SUFFIX);
        }
        status = write_file(text_cstr(&base), &assembly) ? 0 : 1;
        goto done;
    }

    /* DESIGN: the assembly and object files stay beside the executable,
       so that a reader can open them. */
    text_appendf(&asm_path, "%s%s", text_cstr(&base), ASSEMBLY_SUFFIX);
    text_appendf(&obj_path, "%s%s", text_cstr(&base),
                 target_info(o->target)->object_suffix);
    if (!write_file(text_cstr(&asm_path), &assembly)) {
        goto done;
    }
    if (!assemble(o, text_cstr(&asm_path), text_cstr(&obj_path))) {
        goto done;
    }
    if (object_only) {
        status = 0;
        goto done;
    }
    if (o->lib != LIB_NONE) {
        status = build_c_library(o, text_cstr(&obj_path), text_cstr(&base),
                                 &extras)
                     ? 0
                     : 1;
        goto done;
    }
    if (o->output == NULL) {
        text_append(&base, target_info(o->target)->executable_suffix);
    }
    if (link_program(o, text_cstr(&obj_path), text_cstr(&base))) {
        status = 0;
    }

done:
    extras_free(&extras);
    text_free(&source);
    text_free(&module);
    text_free(&assembly);
    text_free(&base);
    text_free(&asm_path);
    text_free(&obj_path);
    return status;
}
