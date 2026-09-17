#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "antl.h"
#include "driver.h"
#include "linker.h"
#include "selfpath.h"
#include "target.h"

static int usage(FILE *out)
{
    fputs("usage: antic [options] <file.anti> [<library.antl>...]\n"
          "             [<object.o> <archive.a>...]\n"
          "\n"
          "options:\n"
          "  -o <file>            write the executable, or with -S the\n"
          "                       assembly or with -c the library, to <file>\n"
          "  -S                   stop after writing the assembly text\n"
          "  -c                   write the library file <file.antl>\n"
          "  --dev                compile the module alone into its object,\n"
          "                       and link it when it defines main. The\n"
          "                       input may be a library file instead\n"
          "  --lib static|shared  write a library for C and its header\n"
          "  --bundle-runtime     put the runtime into the static library\n"
          "  --soname             give the shared library the major version\n"
          "  --llvm-ar <path>     the llvm-ar executable\n"
          "  --linker lld|platform  link with lld of the runtime archive, the\n"
          "                       default, or with the platform linker\n"
          "  -I <dir>             a search root: module a.b is a/b.anti\n"
          "                       or a/b.antl under it\n"
          "  --anti-internal      allow -c for a module under anti.\n"
          "  --strip-docs         leave the doc text out of the library\n"
          "  --doc-warnings       warn about documentation, for anti check\n"
          "  --package-name <p>   the package header of the library: name,\n"
          "  --package-version <v>  version,\n"
          "  --dependency <n,c,u> a dependency with name, constraint, URL,\n"
          "  --license <spdx>     the licence identifier,\n"
          "  --license-text <f>   the file of the licence text\n"
          "  --attribution <line> and one attribution line\n"
          "  --target <name>      compile for <name>, for example macos-arm64\n"
          "  --llvm-mc <path>     the llvm-mc executable\n"
          "  --runtime <dir>      the directory holding lib/<target>/\n"
          "  --dump-tokens        print the tokens of <file.anti> and stop\n"
          "  --dump-ast           print the syntax tree and stop\n"
          "  --dump-types         print the checked tree and stop\n"
          "  --dump-ir            print the IR after lowering and stop\n"
          "  --dump-opt           print the IR after optimization and stop\n"
          "  --dump-select        print the selected machine code and stop\n"
          "  --dump-alloc         print the machine code after register\n"
          "                       allocation and stop\n"
          "  --print-host-target  print the target antic runs on\n"
          "  --print-targets      print the six targets and their facts\n"
          "  --version            print the version\n",
          out);
    return out == stderr ? 2 : 0;
}

/* Take the value of an option such as -o, or report that it is missing. */
/* One line per target: object format, calling convention and triple. */
static int print_targets(void)
{
    int t;

    printf("%-15s %-7s %-15s %s\n", "target", "format", "convention",
           "triple");
    for (t = 0; t < TARGET_COUNT; t++) {
        const struct target_info *info = target_info((enum target)t);
        printf("%-15s %-7s %-15s %s\n", target_name((enum target)t),
               object_format_name(info->format),
               convention_name(info->convention), info->triple);
    }
    return 0;
}

static bool ends_with(const char *s, const char *suffix)
{
    size_t n = strlen(s);
    size_t m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

static const char *value_of(int argc, char **argv, int *i)
{
    if (*i + 1 >= argc) {
        fprintf(stderr, "antic: %s needs a value\n", argv[*i]);
        return NULL;
    }
    *i += 1;
    return argv[*i];
}

static int run(int argc, char **argv, struct options *o)
{
    struct options options = *o;
    bool have_host = target_host(&options.target);
    const char *target = NULL;
    struct text home = {0};
    int status;
    int i;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char **slot = NULL;

        if (strcmp(arg, "--version") == 0) {
            printf("antic %s\n", ANTIC_VERSION);
            return 0;
        } else if (strcmp(arg, "--help") == 0) {
            return usage(stdout);
        } else if (strcmp(arg, "--print-host-target") == 0) {
            if (!have_host) {
                fputs("antic: unknown host target\n", stderr);
                return 1;
            }
            printf("%s\n", target_name(options.target));
            return 0;
        } else if (strcmp(arg, "--print-targets") == 0) {
            return print_targets();
        } else if (strcmp(arg, "--dump-tokens") == 0) {
            options.dump_tokens = true;
            continue;
        } else if (strcmp(arg, "--dump-ast") == 0) {
            options.dump_ast = true;
            continue;
        } else if (strcmp(arg, "--dump-types") == 0) {
            options.dump_types = true;
            continue;
        } else if (strcmp(arg, "--dump-ir") == 0) {
            options.dump_ir = true;
            continue;
        } else if (strcmp(arg, "--dump-alloc") == 0) {
            options.dump_alloc = true;
            continue;
        } else if (strcmp(arg, "--dump-select") == 0) {
            options.dump_select = true;
            continue;
        } else if (strcmp(arg, "--dump-opt") == 0) {
            options.dump_opt = true;
            continue;
        } else if (strcmp(arg, "-c") == 0) {
            options.library = true;
            continue;
        } else if (strcmp(arg, "-S") == 0) {
            options.assembly_only = true;
            continue;
        } else if (strcmp(arg, "--anti-internal") == 0) {
            options.internal = true;
            continue;
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
        } else if (strcmp(arg, "--dev") == 0) {
            options.dev = true;
            continue;
        } else if (strcmp(arg, "--strip-docs") == 0) {
            options.strip_docs = true;
            continue;
        } else if (strcmp(arg, "--linker") == 0) {
            const char *value = value_of(argc, argv, &i);
            if (value == NULL) {
                return 2;
            }
            if (strcmp(value, "lld") == 0) {
                options.linker = LINKER_LLD;
            } else if (strcmp(value, "platform") == 0) {
                options.linker = LINKER_PLATFORM;
            } else {
                fprintf(stderr, "antic: --linker takes lld or platform\n");
                return 2;
            }
            continue;
        } else if (strcmp(arg, "--doc-warnings") == 0) {
            options.doc_warnings = true;
            continue;
        } else if (strcmp(arg, "--dependency") == 0 ||
                   strcmp(arg, "--attribution") == 0) {
            const char *value = value_of(argc, argv, &i);
            if (value == NULL) {
                return 2;
            }
            if (arg[2] == 'd') {
                options.dependencies[options.dependency_count++] = value;
            } else {
                options.attribution[options.attribution_count++] = value;
            }
            continue;
        } else if (strcmp(arg, "--package-name") == 0) {
            slot = &options.package_name;
        } else if (strcmp(arg, "--package-version") == 0) {
            slot = &options.package_version;
        } else if (strcmp(arg, "--license") == 0) {
            slot = &options.license;
        } else if (strcmp(arg, "--license-text") == 0) {
            slot = &options.license_text;
        } else if (strcmp(arg, "-I") == 0) {
            if ((options.roots[options.root_count] =
                     value_of(argc, argv, &i)) == NULL) {
                return 2;
            }
            options.root_count++;
            continue;
        } else if (strcmp(arg, "-o") == 0) {
            slot = &options.output;
        } else if (strcmp(arg, "--target") == 0) {
            slot = &target;
        } else if (strcmp(arg, "--llvm-mc") == 0) {
            slot = &options.llvm_mc;
        } else if (strcmp(arg, "--runtime") == 0) {
            slot = &options.runtime;
        } else if (arg[0] == '-') {
            fprintf(stderr, "antic: unknown option %s\n", arg);
            return usage(stderr);
        } else if (ends_with(arg, ANTL_SUFFIX)) {
            options.libraries[options.library_count++] = arg;
            continue;
        } else if (link_is_input(arg)) {
            options.objects[options.object_count++] = arg;
            continue;
        } else if (options.input == NULL) {
            options.input = arg;
            continue;
        } else {
            fputs("antic: one input file only\n", stderr);
            return usage(stderr);
        }
        if ((*slot = value_of(argc, argv, &i)) == NULL) {
            return 2;
        }
    }

    /* DESIGN: in dev mode the first library file is the input when no
       source file is given. Each module of the graph gets its own object. */
    if (options.input == NULL && options.dev && options.library_count > 0) {
        options.input = options.libraries[0];
        options.libraries++;
        options.library_count--;
    }
    if (options.input == NULL) {
        return usage(stderr);
    }
    if (target != NULL) {
        if (!target_from_name(target, &options.target)) {
            fprintf(stderr, "antic: unknown target %s\n", target);
            return 2;
        }
    } else if (!have_host) {
        fputs("antic: unknown host target, pass --target\n", stderr);
        return 2;
    }
    /* DESIGN: an installed antic sits in bin/ of the runtime archive, so
       without --runtime it takes the directory above itself. A tree with
       no lib/ is a build tree rather than an archive, and the driver then
       reports the missing runtime as before. */
    if (options.runtime == NULL) {
        struct text bin = {0};
        struct text lib = {0};
        if (self_directory(&bin)) {
            const char *path = text_cstr(&bin);
            size_t cut = bin.length;
            while (cut > 0 && path[cut - 1] != '/' && path[cut - 1] != '\\') {
                cut--;
            }
            if (cut > 1) {
                text_append_bytes(&home, path, cut - 1);
                text_appendf(&lib, "%s/%s", text_cstr(&home),
                             RUNTIME_LIB_DIR);
                if (directory_exists(text_cstr(&lib))) {
                    options.runtime = text_cstr(&home);
                }
            }
        }
        text_free(&bin);
        text_free(&lib);
    }
    status = driver_run(&options);
    text_free(&home);
    return status;
}

int main(int argc, char **argv)
{
    struct options options = {0};
    int status;

    /* At most argc - 1 arguments are library files or link inputs. */
    options.libraries = malloc((size_t)argc * sizeof *options.libraries);
    options.objects = malloc((size_t)argc * sizeof *options.objects);
    options.roots = malloc((size_t)argc * sizeof *options.roots);
    options.dependencies = malloc((size_t)argc * sizeof *options.dependencies);
    options.attribution = malloc((size_t)argc * sizeof *options.attribution);
    if (options.libraries == NULL || options.objects == NULL ||
        options.roots == NULL || options.dependencies == NULL ||
        options.attribution == NULL) {
        fputs("antic: out of memory\n", stderr);
        return 70;
    }
    status = run(argc, argv, &options);
    free((void *)options.libraries);
    free((void *)options.objects);
    free((void *)options.roots);
    free((void *)options.dependencies);
    free((void *)options.attribution);
    return status;
}
