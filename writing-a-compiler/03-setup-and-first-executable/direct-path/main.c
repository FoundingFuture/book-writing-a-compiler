#include <stdio.h>
#include <string.h>

#include "driver.h"
#include "target.h"

static int usage(FILE *out)
{
    fputs("usage: antic [options] <file.anti>\n"
          "\n"
          "options:\n"
          "  -o <file>            write the executable, or with -S the\n"
          "                       assembly, to <file>\n"
          "  -S                   stop after writing the assembly text\n"
          "  --target <name>      compile for <name>, for example macos-arm64\n"
          "  --llvm-mc <path>     the llvm-mc executable\n"
          "  --runtime <dir>      the directory holding lib/<target>/\n"
          "  --print-host-target  print the target antic runs on\n"
          "  --version            print the version\n",
          out);
    return out == stderr ? 2 : 0;
}

/* Take the value of an option such as -o, or report that it is missing. */
static const char *value_of(int argc, char **argv, int *i)
{
    if (*i + 1 >= argc) {
        fprintf(stderr, "antic: %s needs a value\n", argv[*i]);
        return NULL;
    }
    *i += 1;
    return argv[*i];
}

int main(int argc, char **argv)
{
    struct options options = {0};
    bool have_host = target_host(&options.target);
    const char *target = NULL;
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
        } else if (strcmp(arg, "-S") == 0) {
            options.assembly_only = true;
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
    return driver_run(&options);
}
