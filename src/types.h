#ifndef ANTIC_TYPES_H
#define ANTIC_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arena.h"
#include "ast.h"
#include "text.h"

/* The checked types of a compilation. Every type exists once, so two
   types are the same type exactly when their pointers are equal. Structs
   are the exception chapter 2 makes: each declaration is a new type. */

enum type_kind {
    TYPE_VOID,      /* the result of a function without -> R */
    TYPE_BOOL,
    TYPE_CHAR,
    TYPE_I8,
    TYPE_I16,
    TYPE_I32,
    TYPE_I64,
    TYPE_CLONG,     /* 32 bits on Windows, 64 bits elsewhere */
    TYPE_U8,
    TYPE_U16,
    TYPE_U32,
    TYPE_U64,
    TYPE_CULONG,    /* 32 bits on Windows, 64 bits elsewhere */
    TYPE_CWCHAR,    /* 16 bits on Windows, 32 bits elsewhere, unsigned */
    TYPE_F32,
    TYPE_F64,
    TYPE_STR,
    TYPE_NULL,      /* the type of null before a context gives it one */
    TYPE_ERROR,     /* an expression that already produced a diagnostic */
    TYPE_BUILTIN_COUNT,
    TYPE_POINTER = TYPE_BUILTIN_COUNT,
    TYPE_ARRAY,
    TYPE_SLICE,
    TYPE_FN,
    TYPE_STRUCT
};

struct struct_field {
    struct name name;
    struct pos pos;
    struct type *type;
    uint8_t bits;                   /* the width of a bitfield, or 0 */
    struct doc_text doc;            /* the /// text */
};

enum layout_state { LAYOUT_NONE, LAYOUT_BUSY, LAYOUT_DONE };

enum symbolic_kind {
    SYMBOLIC_INT,
    SYMBOLIC_SIZE_OF,
    SYMBOLIC_UNARY,
    SYMBOLIC_BINARY,
    SYMBOLIC_CAST
};

/* A constant integer or bool whose value depends on the target, because
   it is computed from size_of. Nodes are interned, so two equal values
   are one pointer. */
struct symbolic {
    enum symbolic_kind kind;
    struct type *type;
    uint64_t value;                 /* SYMBOLIC_INT */
    struct type *of;                /* SYMBOLIC_SIZE_OF */
    enum token_kind op;             /* SYMBOLIC_UNARY, SYMBOLIC_BINARY */
    const struct symbolic *a;       /* the operand of an operation or cast */
    const struct symbolic *b;       /* SYMBOLIC_BINARY */
    struct symbolic *next;          /* the list of interned nodes */
};

struct type {
    enum type_kind kind;
    struct type *element;           /* TYPE_POINTER, TYPE_ARRAY, TYPE_SLICE */
    uint64_t length;                /* TYPE_ARRAY, 0 when symbolic */
    const struct symbolic *length_of; /* TYPE_ARRAY, a symbolic length */
    struct type **params;           /* TYPE_FN */
    size_t param_count;
    struct type *result;            /* TYPE_FN, TYPE_VOID without a result */
    struct name module;             /* TYPE_STRUCT */
    struct name name;               /* TYPE_STRUCT */
    struct struct_field *fields;    /* TYPE_STRUCT */
    size_t field_count;

    /* The modifiers of a struct or union: union, packed, the N of
       align(N) or 0, and export. */
    bool is_union;
    bool packed;
    uint64_t align;
    bool item_exported;

    enum layout_state layout;       /* TYPE_STRUCT, for the cycle check */
    struct type *next;              /* the list of derived types */
};

struct types {
    struct arena *arena;
    struct type builtins[TYPE_BUILTIN_COUNT];
    struct type *derived;
    struct symbolic *symbolics;
};

void types_init(struct types *types, struct arena *arena);
struct type *types_builtin(struct types *types, enum type_kind kind);
struct type *types_pointer(struct types *types, struct type *element);
struct type *types_array(struct types *types, struct type *element,
                         uint64_t length);
struct type *types_slice(struct types *types, struct type *element);
/* An array whose length is a symbolic value. */
struct type *types_array_symbolic(struct types *types, struct type *element,
                                  const struct symbolic *length);
/* The interned node equal to key. */
const struct symbolic *types_symbolic(struct types *types,
                                      const struct symbolic *key);
/* Append a symbolic value as a program writes it. With qualified set, a
   struct name carries its module. */
void symbolic_print(struct text *out, const struct symbolic *s,
                    bool qualified);
struct type *types_fn(struct types *types, struct type **params,
                      size_t param_count, struct type *result);

/* A new struct type without fields. Each call returns a distinct type. */
struct type *types_struct(struct types *types, struct name module,
                          struct name name);
void types_set_fields(struct types *types, struct type *s,
                      const struct struct_field *fields, size_t count);

/* DESIGN: the front end computes no layout, because the back end lays out
   types for its target. The front end checks only that no struct contains
   itself by value, directly or through size_of in an array length.
   Returns NULL, or the struct that contains itself. */
struct type *types_find_cycle(struct type *s);

/* The name of t as a program writes it, with int, float and byte for the
   aliased types. */
void type_name(struct text *out, const struct type *t);
/* The name of t with the module of every struct in it, as main.Vec2. */
void type_name_qualified(struct text *out, const struct type *t);

bool type_is_integer(const struct type *t);
/* Whether f is a zero-width bitfield, written `_: T : 0`, which breaks the
   unit of the bitfields and holds no value. */
bool type_field_is_unit_break(const struct struct_field *f);
/* c_long, c_ulong and c_wchar, whose width the target decides. */
bool type_is_target_sized(const struct type *t);
bool type_is_signed(const struct type *t);
bool type_is_float(const struct type *t);
bool type_is_numeric(const struct type *t);
/* The width in bits. A target-sized type has the narrower of its widths,
   the range a value must fit on every target. */
int type_bits(const struct type *t);

/* A type with no pointer inside it, the property the threading chapter
   needs. str counts as pointer-free, because its bytes never change. */
bool type_pointer_free(const struct type *t);

#endif
