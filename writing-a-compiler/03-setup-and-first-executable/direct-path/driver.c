#include "driver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diagnostic.h"
#include "direct.h"
#include "emit_direct.h"
#include "process.h"
#include "text.h"

#define SOURCE_SUFFIX ".anti"
#define ASSEMBLY_SUFFIX ".s"
#define OBJECT_SUFFIX ".o"

/* The runtime archive keeps each target's libraries in
   <runtime>/RUNTIME_LIB_DIR/<target>/. */
#define RUNTIME_LIB_DIR "lib"
#define RUNTIME_LIBRARY "libanti_rt.a"

static bool ends_with(const char *s, const char *suffix)
{
    size_t n = strlen(s);
    size_t m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
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

/* The module name is the file name without its directory and suffix. It
   must be an identifier, and RUNTIME_MODULE is reserved. */
static bool module_name(const char *input, struct text *out)
{
    const char *base = strrchr(input, '/');
    size_t length;
    size_t i;

    base = base == NULL ? input : base + 1;
    length = strlen(base) - strlen(SOURCE_SUFFIX);
    for (i = 0; i < length; i++) {
        char c = base[i];
        bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      c == '_';
        bool digit = c >= '0' && c <= '9';
        if (!letter && !(digit && i > 0)) {
            fprintf(stderr, "antic: %s: the file name is not an identifier\n",
                    input);
            return false;
        }
    }
    if (length == 0) {
        fprintf(stderr, "antic: %s: the file name is empty\n", input);
        return false;
    }
    text_appendf(out, "%.*s", (int)length, base);
    if (strcmp(text_cstr(out), RUNTIME_MODULE) == 0) {
        fprintf(stderr, "antic: %s: the module name `%s` is reserved\n", input,
                RUNTIME_MODULE);
        return false;
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

static bool link_macos_arm64(const char *object, const char *executable,
                             const char *runtime)
{
    struct text sdk_path = {0};
    struct text sdk_version = {0};
    struct text min_version = {0};
    struct text library = {0};
    bool ok = false;

    if (xcrun("--show-sdk-path", &sdk_path) &&
        xcrun("--show-sdk-version", &sdk_version)) {
        const char *argv[] = {
            "ld", "-arch", "arm64", "-platform_version", "macos",
            NULL, NULL, "-syslibroot", NULL, "-o", executable, object,
            NULL, "-lSystem", NULL};

        text_appendf(&min_version, "%d.%d", MACOS_MIN_MAJOR, MACOS_MIN_MINOR);
        text_appendf(&library, "%s/%s/%s/%s", runtime, RUNTIME_LIB_DIR,
                     target_name(TARGET_MACOS_ARM64), RUNTIME_LIBRARY);
        argv[5] = text_cstr(&min_version);
        argv[6] = text_cstr(&sdk_version);
        argv[8] = text_cstr(&sdk_path);
        argv[12] = text_cstr(&library);
        ok = process_run(argv) == 0;
        if (!ok) {
            fprintf(stderr, "antic: the linker failed\n");
        }
    }
    text_free(&sdk_path);
    text_free(&sdk_version);
    text_free(&min_version);
    text_free(&library);
    return ok;
}

static int compile(const struct options *o, struct text *source,
                   struct text *module, struct text *assembly,
                   struct text *base)
{
    struct diagnostic diag;
    int64_t value;

    if (!read_source(o->input, source) || !module_name(o->input, module)) {
        return 1;
    }
    if (!direct_parse(text_cstr(source), source->length, &value, &diag)) {
        fprintf(stderr, "%s:%d:%d: error: %s\n", o->input, diag.line,
                diag.column, diag.message);
        return 1;
    }
    if (!emit_direct(assembly, o->target, text_cstr(module), value)) {
        fprintf(stderr, "antic: the direct path supports only %s, not %s\n",
                target_name(TARGET_MACOS_ARM64), target_name(o->target));
        return 1;
    }
    if (o->output != NULL) {
        text_append(base, o->output);
    } else {
        text_appendf(base, "%.*s",
                     (int)(strlen(o->input) - strlen(SOURCE_SUFFIX)),
                     o->input);
    }
    return 0;
}

int driver_run(const struct options *o)
{
    struct text source = {0};
    struct text module = {0};
    struct text assembly = {0};
    struct text base = {0};
    struct text asm_path = {0};
    struct text obj_path = {0};
    int status = 1;

    if (!ends_with(o->input, SOURCE_SUFFIX)) {
        fprintf(stderr, "antic: %s: expected a %s file\n", o->input,
                SOURCE_SUFFIX);
        return 2;
    }
    if (!o->assembly_only && o->runtime == NULL) {
        fprintf(stderr, "antic: linking needs --runtime <dir>, the directory "
                        "that holds %s/<target>/%s\n",
                RUNTIME_LIB_DIR, RUNTIME_LIBRARY);
        return 2;
    }
    if (compile(o, &source, &module, &assembly, &base) != 0) {
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
    text_appendf(&obj_path, "%s%s", text_cstr(&base), OBJECT_SUFFIX);
    if (!write_file(text_cstr(&asm_path), &assembly)) {
        goto done;
    }
    {
        const char *argv[] = {
            o->llvm_mc != NULL ? o->llvm_mc : "llvm-mc",
            "-triple=arm64-apple-macos", "-filetype=obj",
            "-o", text_cstr(&obj_path), text_cstr(&asm_path), NULL};
        if (process_run(argv) != 0) {
            fprintf(stderr, "antic: llvm-mc failed\n");
            goto done;
        }
    }
    if (link_macos_arm64(text_cstr(&obj_path), text_cstr(&base), o->runtime)) {
        status = 0;
    }

done:
    text_free(&source);
    text_free(&module);
    text_free(&assembly);
    text_free(&base);
    text_free(&asm_path);
    text_free(&obj_path);
    return status;
}
