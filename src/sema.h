#ifndef ANTIC_SEMA_H
#define ANTIC_SEMA_H

#include <stdbool.h>
#include <stdint.h>

#include "arena.h"
#include "ast.h"
#include "diagnostic.h"
#include "text.h"
#include "types.h"

enum symbol_kind {
    SYMBOL_LOCAL,
    SYMBOL_PARAM,
    SYMBOL_CONST,
    SYMBOL_FN,
    SYMBOL_EXTERN_FN,
    SYMBOL_STRUCT,
    SYMBOL_MODULE   /* the name an import declares */
};

enum const_kind {
    CONST_INT,
    CONST_FLOAT,
    CONST_BOOL,
    CONST_CHAR,
    CONST_NULL,
    CONST_TEXT,     /* a string or byte string literal */
    CONST_ARRAY,
    CONST_STRUCT,
    CONST_SYMBOLIC  /* an integer or bool computed from size_of */
};

/* The value of a constant, computed at compile time. */
struct const_value {
    enum const_kind kind;
    struct type *type;
    union {
        uint64_t integer;           /* the bits of the value in its type */
        double floating;
        bool boolean;
        uint32_t character;
        struct token_text text;
        struct {
            struct const_value *items;  /* elements, or fields in order */
            size_t count;
        } aggregate;
        const struct symbolic *symbolic;
    } as;
};

enum eval_state { EVAL_NONE, EVAL_BUSY, EVAL_DONE };

struct interface;

/* A dependency in the package header of a library file. */
struct package_dependency {
    const char *name;               /* a module path */
    const char *constraint;         /* a version constraint, such as ^1.2 */
    const char *url;                /* the repository */
};

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

struct symbol {
    enum symbol_kind kind;
    struct name name;
    struct pos pos;
    struct type *type;
    struct item *item;              /* a module-level item */
    struct stmt *stmt;              /* a const in a block */
    struct const_value *value;      /* SYMBOL_CONST */
    enum eval_state state;          /* SYMBOL_CONST */
    bool address_taken;             /* SYMBOL_LOCAL, SYMBOL_PARAM */
    bool variadic;                  /* SYMBOL_EXTERN_FN */
    bool worker;                    /* SYMBOL_FN written `worker fn` */
    const struct name *params;      /* a function of an interface */

    /* An export item, whose function has the C symbol of its name. */
    bool exported;
    struct doc_text doc;            /* the /// text of a pub item */
    const struct interface *home;   /* the library of an imported item */
    uint32_t ir;                    /* set by lowering, see lower.h */
};

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

/* Check one module against the rules of chapter 2: resolve every name,
   give every expression its type and compute every constant. The method
   syntax v.f(args) and qualified names are rewritten into plain names.
   libraries holds the interface of every loaded library. Returns true
   when no error occurred. */
bool sema_check(struct module *module, const char *module_name,
                const struct interface *const *libraries,
                size_t library_count, struct types *types,
                struct arena *arena, struct diagnostics *diags);

/* Warn about each pub item with a `//#` note and no `///` comment. */
void sema_doc_warnings(const struct module *module, struct diagnostics *diags);

/* Fill out with the interface of a checked module. The items are new
   symbols whose home is out. */
void sema_interface(const struct module *module, const char *module_name,
                    struct arena *arena, struct interface *out);

/* The syntax tree as ast_dump prints it, with the type of every
   expression, parameter, variable and function on the right. */
void ast_dump_typed(struct text *out, const struct module *module);

#endif
