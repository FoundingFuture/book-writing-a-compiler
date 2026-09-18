#ifndef ANTIC_AST_H
#define ANTIC_AST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lexer.h"
#include "text.h"

/* The syntax tree of one module. Every node lives in the compilation's
   memory pool, and names point into the source text, which outlives the
   tree. */

/* A name as written in the source. */
struct name {
    const char *text;
    size_t length;
};

struct pos {
    int line;
    int column;
};

struct type;    /* a checked type, filled in by semantic analysis */
struct symbol;  /* what a name refers to, filled in by semantic analysis */
struct expr;

enum type_expr_kind {
    TYPEX_BUILTIN,  /* int, f32, str and the other type keywords */
    TYPEX_NAMED,    /* Vec2 or geometry.Vec2 */
    TYPEX_POINTER,  /* *T */
    TYPEX_ARRAY,    /* [N]T */
    TYPEX_SLICE,    /* []T */
    TYPEX_FN        /* fn(T, U) -> R */
};

struct type_expr {
    enum type_expr_kind kind;
    struct pos pos;
    enum token_kind builtin;        /* TYPEX_BUILTIN */
    struct name module;             /* TYPEX_NAMED, empty when unqualified */
    struct name name;               /* TYPEX_NAMED */
    struct type_expr *element;      /* TYPEX_POINTER, TYPEX_ARRAY, TYPEX_SLICE */
    struct expr *length;            /* TYPEX_ARRAY */
    struct type_expr **params;      /* TYPEX_FN */
    size_t param_count;
    struct type_expr *result;       /* TYPEX_FN, NULL without a result */
    struct type *type;              /* set by semantic analysis */
};

/* The operations of an atomic field. Each is a call on the field, and
   the language offers no other way to read or write one. */
enum atomic_op {
    ATOMIC_LOAD,
    ATOMIC_STORE,
    ATOMIC_SWAP,
    ATOMIC_ADD,
    ATOMIC_SUB,
    ATOMIC_AND,
    ATOMIC_OR,
    ATOMIC_CAS
};

enum expr_kind {
    EXPR_INT,
    EXPR_FLOAT,
    EXPR_CHAR,
    EXPR_STRING,
    EXPR_BYTES,
    EXPR_BOOL,
    EXPR_NULL,
    EXPR_NAME,
    EXPR_UNARY,
    EXPR_BINARY,
    EXPR_CAST,
    EXPR_CALL,
    EXPR_INDEX,
    EXPR_SLICE,
    EXPR_FIELD,
    EXPR_STRUCT_LIT,
    EXPR_SLICE_LIT,
    EXPR_ARRAY_LIT,
    EXPR_ARRAY_REPEAT,
    EXPR_ALLOC,
    EXPR_FREE,
    EXPR_OBJECT,                    /* dup, delete or destroy */
    EXPR_ATOMIC,                    /* an operation on an atomic place */
    EXPR_SIZE_OF,
    EXPR_PARALLEL,
    EXPR_DISPATCH,                  /* dispatch obj -> f(args) */
    EXPR_JOIN                       /* join(job) and join_all(jobs) */
};

/* name: value inside a struct or slice literal. */
struct field_init {
    struct name name;
    struct pos pos;
    struct expr *value;
};

/* How a failing call hands its error on. A handler is a block, `fatal`
   prints and exits, and `try` returns the error to the caller. */
enum handler_kind {
    HANDLE_NONE,
    HANDLE_BLOCK,
    HANDLE_FATAL,
    HANDLE_TRY,
    HANDLE_ENCLOSING            /* the `try` block around it */
};

/* `catch e { }` on a call, on a `let`, or around a block. */
struct handler {
    enum handler_kind kind;
    struct name name;               /* the error, empty in `catch { }` */
    struct pos pos;
    struct block *body;             /* HANDLE_BLOCK */
    struct symbol *symbol;          /* the error the handler binds */
    bool passes;                    /* a `return e` hands the error on */
};

struct expr {
    enum expr_kind kind;
    struct pos pos;
    struct token_text spelling;     /* source text of a literal */
    struct type *type;              /* set by semantic analysis */
    struct symbol *symbol;          /* EXPR_NAME, set by semantic analysis */
    /* DESIGN: a pointer to a class converts to a pointer to one of its
       interfaces by adding the offset of the sub-object. The checker
       records the field here and lowering adds the offset, so every
       place a value flows into an interface slot is covered once. */
    const struct struct_field *to_iface;
    union {
        uint64_t integer;           /* EXPR_INT */
        uint32_t character;         /* EXPR_CHAR */
        bool boolean;               /* EXPR_BOOL */
        struct token_text text;     /* EXPR_FLOAT digits, EXPR_STRING, EXPR_BYTES */
        struct name name;           /* EXPR_NAME */
        struct {
            enum token_kind op;
            struct expr *operand;
        } unary;
        struct {
            enum token_kind op;
            struct expr *left;
            struct expr *right;
        } binary;
        struct {
            struct expr *operand;
            struct type_expr *type;
            bool checked;           /* `as?`, which gives null on a mismatch */
            bool test;              /* `is`, which gives a bool */
            bool from_sub;          /* the source may be a sub-object */
            const struct type *target;  /* the class of `is` and `as` */
        } cast;
        struct {
            struct expr *callee;
            struct expr **args;
            size_t arg_count;
            /* The class whose table holds the entry, when the call goes
               through one, and the name of that entry. dispatch is NULL
               for a direct call. */
            const struct type *dispatch;
            struct name entry;
            /* DESIGN: a call that can fail carries its handler. The
               checker refuses one that has none, so no program drops an
               error by writing nothing. */
            struct handler handler;
            struct expr *out;       /* the place the result is written to */
            /* `T(args)` and `alloc T(args)` build a value and run its
               `construct` with the arguments. */
            const struct type *builds;
            bool on_heap;
        } call;
        struct {
            struct expr *base;
            struct expr *index;
        } index;
        struct {
            struct expr *base;
            struct expr *low;
            struct expr *high;
        } slice;
        struct {
            struct expr *base;
            struct name name;
            uint32_t enum_value;    /* the index of an enum value, plus 1 */
            bool promoted;          /* the checker wrote it, not the program */
        } field;
        struct {
            struct name module;     /* empty when unqualified */
            struct name name;
            struct field_init *fields;
            size_t field_count;
        } struct_lit;
        struct {
            struct type_expr *element;
            struct field_init *fields;
            size_t field_count;
        } slice_lit;
        struct {
            struct expr **elements;
            size_t count;
        } array_lit;
        struct {
            struct expr *value;
            struct expr *count;
        } array_repeat;
        struct {
            struct type_expr *type;
            struct expr *count;     /* NULL in the literal form */
            struct expr *value;     /* `alloc T { ... }`, or NULL */
        } alloc;
        struct expr *free_pointer;  /* EXPR_FREE */
        struct {
            enum token_kind op;     /* dup, delete or destroy */
            struct expr *operand;
        } object;
        struct {
            struct expr *job;       /* EXPR_JOIN: the job or the slice */
            bool all;               /* join_all */
        } join;
        struct {
            enum atomic_op op;
            struct expr *place;     /* the atomic field */
            struct expr *a;         /* the value, or the expected one */
            struct expr *b;         /* compare_swap: the new value */
        } atomic;
        struct type_expr *size_of;  /* EXPR_SIZE_OF */
        struct {
            struct expr *array;     /* the array to split */
            struct expr *chunks;    /* the count after `by`, or NULL */
            struct expr *call;      /* the worker, called or named */
        } parallel;
        struct {
            struct expr *object;    /* the object to submit */
            struct expr *call;      /* the worker, called or named */
        } dispatch;
    } as;
};

struct stmt;

struct block {
    struct pos pos;
    struct stmt **stmts;
    size_t count;
};

struct switch_arm {
    struct expr *value;
    struct pos pos;
    struct stmt *body;
};

struct if_branch {
    struct expr *cond;
    struct block *body;
};

enum stmt_kind {
    STMT_LET,
    STMT_CONST,
    STMT_EXPR,
    STMT_ASSIGN,
    STMT_IF,
    STMT_WHILE,
    STMT_DO_WHILE,
    STMT_FOR,
    STMT_DEFER,
    STMT_SWITCH,
    STMT_ASSERT,
    STMT_BREAK,
    STMT_CONTINUE,
    STMT_RETURN,
    STMT_YIELD,
    STMT_TRY,
    STMT_BLOCK
};



struct stmt {
    enum stmt_kind kind;
    struct pos pos;
    union {
        struct {
            struct name name;
            struct pos name_pos;
            struct type_expr *type; /* NULL when a let omits it */
            struct expr *value;
            struct symbol *symbol;
        } let;                      /* STMT_LET, STMT_CONST */
        struct expr *expr;          /* STMT_EXPR */
        struct {
            enum token_kind op;     /* TOKEN_ASSIGN or a compound op */
            struct expr *target;
            struct expr *value;
        } assign;
        struct {
            struct if_branch *branches;
            size_t count;
            struct block *else_body; /* NULL without else */
        } if_chain;
        struct {
            struct expr *cond;
            struct block *body;
        } loop;                     /* STMT_WHILE, STMT_DO_WHILE */
        /* `for i in lo..hi` and `for x in slice`. over is set for the
           second form, and by_pointer for `for x in &slice`. The binding
           is optional in the range form, and step holds the constant of
           `by k`, which walks the set backwards when it is negative. */
        struct {
            struct name name;
            struct pos name_pos;
            struct expr *low;
            struct expr *high;
            struct expr *over;
            struct expr *step;
            struct pos step_pos;
            int64_t step_value;         /* the folded `by k`, or 1 */
            bool by_pointer;
            struct block *body;
            struct symbol *symbol;
        } for_loop;
        struct stmt *deferred;      /* STMT_DEFER */
        struct expr *yielded;       /* STMT_YIELD, NULL without a value */
        /* `try { } catch e { }`: every failing call of the body reaches
           the handler, and the first error abandons the rest. */
        struct {
            struct block *body;
            struct handler handler;
        } try_block;
        /* `assert(cond)` and `assert(cond, "message")`. text is the
           source of the condition, for the failure message. */
        struct {
            struct expr *cond;
            struct token_text message;
            struct token_text text;
        } assertion;
        /* `switch e { A => stmt, else => stmt }`. An arm holds one value
           and one statement, and the else arm has no value. */
        struct {
            struct expr *value;
            struct switch_arm *arms;
            size_t count;
            struct stmt *otherwise;
        } switch_stmt;
        struct expr *return_value;  /* STMT_RETURN, NULL for return; */
        struct block *block;        /* STMT_BLOCK */
    } as;
};

/* The text of the doc comments before a node, empty without one. The
   line form and the block form of a marker give the same text. */
struct doc_text {
    const char *text;
    size_t length;
};

/* How a field joins its class. A plain field is its own name. A `use`
   field promotes the names of its type onto the class that holds it, and
   an `implements` field holds the sub-object of an interface. The checker
   adds two more that no declaration writes. One is the base of a class,
   named by `inherits` and carried as the field `super`. The other is the
   table pointer of the root `anti.rt.Object`. */
enum field_form { FIELD_PLAIN, FIELD_USE, FIELD_BASE, FIELD_TABLE,
                  FIELD_IMPL };

/* DESIGN: the four levels of the object model document. A module item is
   private, internal or public. A class member is private, protected or
   public. The two ends of the range are shared, and the middle level
   differs, so one enumeration serves both. */
enum visibility { VIS_PRIVATE, VIS_PROTECTED, VIS_INTERNAL, VIS_PUB };

/* A parameter, or a field of a struct declaration. */
struct param {
    struct name name;
    struct pos pos;
    struct type_expr *type;
    struct symbol *symbol;
    struct doc_text doc;            /* fields only */
    struct doc_text note;           /* fields only */
    struct expr *bits;              /* the width of a bitfield, or NULL */
    struct expr *value;             /* a field default or an enum value */
    enum field_form form;           /* fields only */
    enum visibility vis;            /* fields only */
    bool owned;                     /* `own`: the object frees the memory */
    bool atomic;                    /* `atomic`: read and written by calls */
    bool writable;                  /* `mutable`: a singleton field to write */
};

enum item_kind {
    ITEM_FN,
    ITEM_EXTERN_FN,
    ITEM_STRUCT,
    ITEM_UNION,
    ITEM_CONST,
    ITEM_ENUM,
    ITEM_CLASS
};

/* A function of a struct body against the contracts of its chain. */
enum fn_contract { FN_PLAIN, FN_ABSTRACT, FN_CONCRETE };

struct item {
    enum item_kind kind;
    struct pos pos;
    bool pub;
    bool exported;                  /* export, which implies pub */
    struct name name;
    struct pos name_pos;
    struct param *params;           /* parameters, or fields of a struct or union */
    size_t param_count;
    bool variadic;                  /* ITEM_EXTERN_FN */
    bool worker;                    /* ITEM_FN, may run on a worker */
    struct type_expr *result;       /* NULL without a result */
    struct block *body;             /* ITEM_FN */
    struct type_expr *type;         /* ITEM_CONST */
    struct expr *value;             /* ITEM_CONST */
    bool packed;                    /* ITEM_STRUCT, ITEM_UNION */
    struct expr *align;             /* ITEM_STRUCT, ITEM_UNION, or NULL */
    struct symbol *symbol;
    struct doc_text doc;            /* the /// text */
    struct doc_text note;           /* the //# text */
    struct item **members;          /* the functions and constants of a body */
    size_t member_count;
    enum fn_contract contract;      /* ITEM_FN */
    bool has_self;                  /* ITEM_FN: self is its first parameter */
    struct symbol *self;            /* ITEM_FN: the symbol of self */
    const char *runtime;            /* ITEM_FN of the root: its C symbol */
    const struct item *owner;       /* the class or enum that declares it */
    struct type_expr *base;         /* ITEM_ENUM: the underlying type, or NULL */
    struct name base_name;          /* ITEM_CLASS: the base after `inherits` */
    struct pos base_pos;
    bool is_abstract;               /* ITEM_CLASS, or ITEM_FN in a body */
    bool is_final;                  /* ITEM_CLASS, ITEM_FN */
    bool is_static;                 /* a static atomic field of a class */
    bool atomic;                    /* ITEM_CONST with is_static */
    bool is_singleton;              /* ITEM_CLASS with one instance */
    bool singleton_get;             /* the generated `get` of a singleton */
    bool is_operator;               /* ITEM_FN that an operator calls */
    enum visibility vis;
    struct name qualifier;          /* `concrete fn X::f`, the X */
    struct pos qualifier_pos;
};

struct import {
    struct pos pos;
    struct pos module_pos;
    struct name module;
    struct name alias;              /* empty without as */
};

/* A doc comment that no item, field or module took. Its text is lost, so
   `--doc-warnings` reports it. */
struct dropped_doc {
    struct pos pos;
    struct name marker;             /* the marker, as the reader wrote it */
    bool module_form;               /* `//!` or `//#!`, which a module takes */
};

struct module {
    const char *file;               /* the source path, for a message */
    struct doc_text doc;            /* the `//!` text */
    struct doc_text note;           /* the `//#!` text */
    struct import *imports;
    size_t import_count;
    struct item **items;
    size_t item_count;
    struct dropped_doc *dropped;
    size_t dropped_count;
};

/* Append the tree of module to out, one node per line, indented by two
   spaces per level. Chapter 1 shows this output for the function scale. */
void ast_dump(struct text *out, const struct module *module);

#endif
