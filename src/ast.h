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
    EXPR_SIZE_OF,
    EXPR_PARALLEL
};

/* name: value inside a struct or slice literal. */
struct field_init {
    struct name name;
    struct pos pos;
    struct expr *value;
};

struct expr {
    enum expr_kind kind;
    struct pos pos;
    struct token_text spelling;     /* source text of a literal */
    struct type *type;              /* set by semantic analysis */
    struct symbol *symbol;          /* EXPR_NAME, set by semantic analysis */
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
        } cast;
        struct {
            struct expr *callee;
            struct expr **args;
            size_t arg_count;
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
            struct expr *count;
        } alloc;
        struct expr *free_pointer;  /* EXPR_FREE */
        struct type_expr *size_of;  /* EXPR_SIZE_OF */
        struct {
            struct expr *array;     /* the array to split */
            struct expr *chunks;    /* the count after `by`, or NULL */
            struct expr *call;      /* the worker, called or named */
        } parallel;
    } as;
};

struct stmt;

struct block {
    struct pos pos;
    struct stmt **stmts;
    size_t count;
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
    STMT_BREAK,
    STMT_CONTINUE,
    STMT_RETURN,
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

/* How a field joins its struct. A plain field is its own name. A `use`
   field promotes the names of its type, and an `inherits` field nests its
   type whole at offset 0 and promotes them too. */
enum field_form { FIELD_PLAIN, FIELD_USE, FIELD_INHERITS };

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
};

enum item_kind {
    ITEM_FN,
    ITEM_EXTERN_FN,
    ITEM_STRUCT,
    ITEM_UNION,
    ITEM_CONST,
    ITEM_ENUM
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
    bool has_self;                  /* ITEM_FN: the receiver is its first parameter */
    const struct item *owner;       /* the struct or enum a member belongs to */
    struct type_expr *base;         /* ITEM_ENUM: the underlying type, or NULL */
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
