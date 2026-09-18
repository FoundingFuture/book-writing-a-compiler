#ifndef ANTIC_DRIVER_H
#define ANTIC_DRIVER_H

#include <stdbool.h>

#include "linker.h"
#include "target.h"

struct options {
    const char *input;          /* The .anti source file. */
    const char *output;         /* NULL: input without .anti. */
    const char *llvm_mc;        /* NULL: llvm-mc from PATH. */
    const char *runtime;        /* Holds lib/<target>/. */
    const char **libraries;     /* The .antl files. */
    size_t library_count;
    const char **objects;       /* Object files and archives to link. */
    size_t object_count;
    const char **roots;         /* -I, the search roots of module paths. */
    size_t root_count;
    bool internal;              /* --anti-internal, allow anti. paths. */
    bool strip_docs;            /* --strip-docs, no doc text with -c. */
    bool doc_warnings;          /* --doc-warnings, as anti check passes. */
    bool dev;                   /* --dev, one module into its own object. */
    bool no_reflect;            /* --no-reflect, no field list in a
                                   class descriptor. */
    /* DESIGN: dev mode keeps assertions and release mode drops them,
       because dev compiles one module and release the whole program.
       --asserts and --no-asserts decide instead of the mode. */
    enum { ASSERTS_MODE, ASSERTS_ON, ASSERTS_OFF } asserts;
    enum { LIB_NONE, LIB_STATIC, LIB_SHARED } lib; /* --lib static|shared */
    bool bundle_runtime;        /* --bundle-runtime, with --lib static. */
    bool soname;                /* --soname, with --lib shared. */
    const char *llvm_ar;        /* NULL: llvm-ar from PATH. */
    enum linker linker;         /* --linker lld|platform, lld by default. */
    const char *package_name;   /* --package-name, of the header. */
    const char *package_version;
    const char **dependencies;  /* --dependency <name>,<constraint>,<url> */
    size_t dependency_count;
    const char *license;        /* --license, an SPDX identifier. */
    const char *license_text;   /* --license-text, a file. */
    const char **attribution;   /* --attribution, one line each. */
    size_t attribution_count;
    bool library;               /* -c, write a library file. */
    bool assembly_only;         /* -S, stop after assembly. */
    bool dump_tokens;           /* --dump-tokens. */
    bool dump_ast;              /* --dump-ast. */
    bool dump_types;            /* --dump-types. */
    bool dump_ir;               /* --dump-ir. */
    bool dump_opt;              /* --dump-opt. */
    bool dump_select;           /* --dump-select. */
    bool dump_alloc;            /* --dump-alloc. */
    enum target target;
};

/* Compile options->input and return the process exit status for antic. */
int driver_run(const struct options *options);

#endif
