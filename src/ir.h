#ifndef ANTIC_IR_H
#define ANTIC_IR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arena.h"
#include "text.h"

/* The intermediate representation of antic: typed three-address code in
   functions of basic blocks. It is the same for all six targets. It holds
   no register, no calling convention and no symbol mangling, because the
   back end decides those per target.

   DESIGN: the IR records types, never sizes, offsets or register classes.
   A size, an offset or an array stride is a symbolic value that names the
   type. The back end folds it after it lays out the types for its target,
   so a library file holds the same bytes on every host. */

/* Scalar value types. bool is i8, char is i32, and every pointer and
   function pointer is ptr. Signedness lives in the operations. */
enum ir_type {
    IR_VOID,
    IR_I8,
    IR_I16,
    IR_I32,
    IR_I64,
    IR_F32,
    IR_F64,
    IR_PTR,
    IR_AGG,     /* a struct, union, array, str or slice passed by value */
    IR_CLONG,   /* c_long and c_ulong: 32 bits on Windows, else 64 */
    IR_CWCHAR   /* c_wchar: 16 bits on Windows, else 32 */
};

#define IR_NO_AGG UINT32_MAX

/* The type of a value in memory: a scalar, or IR_AGG with the index of an
   aggregate in the module's type table. */
struct ir_vtype {
    enum ir_type type;
    uint32_t agg;
};

enum ir_agg_kind { IR_AGG_STRUCT, IR_AGG_UNION, IR_AGG_ARRAY };

/* How a parameter of 8 or 16 bits extends to 32 bits. The IR keeps
   signedness in operations, and a parameter has none, so the signature
   records it for the conventions whose callers extend. */
enum ir_ext { IR_EXT_NONE, IR_EXT_SIGN, IR_EXT_ZERO };

/* A field of a struct or union, or the element of an array. A bitfield
   has a width in bits and extends by its signedness when it is read. */
struct ir_field {
    const char *name;               /* NULL for an array element */
    struct ir_vtype type;
    uint8_t bits;                   /* 0 for a field that is not a bitfield */
    enum ir_ext ext;
};

/* An aggregate type. A struct or union lists its fields, and an array has
   one field for its element and a symbolic length. */
struct ir_aggtype {
    enum ir_agg_kind kind;
    const char *name;               /* main.Vec2, str, []i32 or [4]i32 */
    struct ir_field *fields;
    size_t field_count;
    bool packed;
    uint64_t align;                 /* the N of align(N), or 0 */
    uint32_t length;                /* IR_AGG_ARRAY: a symbolic value */
    const char *length_text;        /* IR_AGG_ARRAY: the length as written */
};

enum ir_sym_kind {
    IR_SYM_INT,         /* a number */
    IR_SYM_SIZE_OF,     /* size_of T */
    IR_SYM_OFFSET_OF,   /* offset_of T.f */
    IR_SYM_OP           /* an operation on one or two symbolic values */
};

/* A symbolic value, an integer that depends on the target. */
struct ir_sym {
    enum ir_sym_kind kind;
    enum ir_type type;
    uint64_t value;                 /* IR_SYM_INT */
    struct ir_vtype of;             /* IR_SYM_SIZE_OF, IR_SYM_OFFSET_OF */
    uint32_t field;                 /* IR_SYM_OFFSET_OF */
    int op;                         /* IR_SYM_OP, an enum ir_op */
    uint32_t a;                     /* IR_SYM_OP */
    uint32_t b;                     /* IR_SYM_OP, IR_NO_AGG for one operand */
};

enum ir_op {
    /* result = op a, b: both operands and the result have the same type */
    IR_ADD, IR_SUB, IR_MUL, IR_SDIV, IR_UDIV, IR_SREM, IR_UREM,
    IR_AND, IR_OR, IR_XOR, IR_SHL, IR_SHR_S, IR_SHR_U,
    IR_FADD, IR_FSUB, IR_FMUL, IR_FDIV,
    /* result = op a */
    IR_NEG, IR_NOT, IR_FNEG, IR_COPY,
    /* result i8 = op a, b: 1 when the comparison holds, else 0 */
    IR_EQ, IR_NE, IR_SLT, IR_SLE, IR_SGT, IR_SGE,
    IR_ULT, IR_ULE, IR_UGT, IR_UGE,
    IR_FEQ, IR_FNE, IR_FLT, IR_FLE, IR_FGT, IR_FGE,
    /* result = op a, with the result type named by the instruction */
    IR_TRUNC, IR_SEXT, IR_ZEXT, IR_SITOF, IR_UITOF, IR_FTOSI, IR_FTOUI,
    IR_FEXT, IR_FTRUNC,
    /* Memory. */
    IR_SLOT,        /* Result ptr: a stack slot for a value of type of. */
    IR_LOAD,        /* Result: load type from a. */
    IR_STORE,       /* Store a of type into b. */
    IR_PTRADD,      /* Result ptr: a plus b bytes. */
    IR_MEMCOPY,     /* Copy a value of type of from b to a. */
    IR_ADDR,        /* Result ptr: address of a. */
    IR_BITLOAD,     /* Result: load bitfield field of aggregate of from a. */
    IR_BITSTORE,    /* Store a into bitfield field of aggregate of at b. */
    /* Calls. */
    IR_CALL,        /* Result: call a with args, as signature b if set. */
    /* Terminators, the last instruction of a block. */
    IR_JUMP,        /* Go to block a. */
    IR_BRANCH,      /* Go to b when a is nonzero, else c. */
    IR_RET          /* Return a, or nothing. */
};

enum ir_operand_kind {
    IR_NONE,
    IR_TEMP,        /* A temporary, %n. */
    IR_INT,         /* An integer constant. */
    IR_FLOAT,       /* A float constant. */
    IR_GLOBAL,      /* a global data item, by index */
    IR_FUNC,        /* a function, by index */
    IR_BLOCK,       /* a basic block, by index */
    IR_SYM          /* a symbolic value, by index */
};

struct ir_operand {
    enum ir_operand_kind kind;
    enum ir_type type;
    union {
        uint32_t temp;
        uint64_t integer;
        double floating;
        uint32_t index;
    } as;
};

#define IR_NO_RESULT UINT32_MAX

struct ir_inst {
    enum ir_op op;
    enum ir_type type;              /* the result type, or the stored type */
    uint32_t result;                /* a temporary, or IR_NO_RESULT */
    struct ir_operand a;
    struct ir_operand b;
    struct ir_operand c;
    struct ir_vtype of;             /* IR_SLOT, IR_MEMCOPY, the bitfield ops */
    uint32_t field;                 /* IR_BITLOAD, IR_BITSTORE */
    struct ir_operand *args;        /* IR_CALL */
    size_t arg_count;
};

struct ir_block {
    uint32_t index;
    struct ir_inst *insts;
    size_t count;
    size_t capacity;
};

/* A parameter. An aggregate parameter's temporary holds a pointer to the
   value, and the back end copies it in as its ABI requires. */
struct ir_param {
    enum ir_type type;
    enum ir_ext ext;
    uint32_t agg;                   /* IR_AGG: the aggregate, else IR_NO_AGG */
    uint32_t temp;
};

struct ir_function {
    uint32_t index;
    const char *module;             /* NULL for a C function */
    const char *name;
    struct ir_param *params;
    size_t param_count;
    size_t param_capacity;
    enum ir_type result;
    uint32_t result_agg;            /* IR_AGG: the aggregate, else IR_NO_AGG */
    bool is_extern;                 /* no body: C or another module */
    bool variadic;
    bool exported;                  /* an export fn, with a C symbol */
    struct ir_block **blocks;
    size_t block_count;
    size_t block_capacity;
    enum ir_type *temps;            /* the type of each temporary */
    uint32_t temp_count;
    uint32_t temp_capacity;
};

/* A pointer inside global data to another global. */
struct ir_reloc {
    uint64_t offset;
    uint32_t global;
};

enum ir_const_kind {
    IR_CONST_NONE,      /* no value, such as the break of a bitfield unit */
    IR_CONST_INT,       /* an integer, a bitfield among them */
    IR_CONST_FLOAT,
    IR_CONST_SYM,       /* a symbolic value, such as size_of T */
    IR_CONST_ADDR,      /* the address of another global */
    IR_CONST_AGG        /* a struct, a union or an array */
};

/* DESIGN: an aggregate constant reaches the back end as a typed tree and
   not as bytes. The IR holds no sizes. A field's offset, its padding and
   the bits of a bitfield belong to the back end, which lays the type out
   for its target. */
struct ir_const {
    enum ir_const_kind kind;
    struct ir_vtype type;           /* IR_CONST_AGG: the aggregate */
    enum ir_type scalar;            /* the value types */
    uint64_t integer;               /* IR_CONST_INT */
    double floating;                /* IR_CONST_FLOAT */
    uint32_t sym;                   /* IR_CONST_SYM */
    uint32_t global;                /* IR_CONST_ADDR */
    struct ir_const *items;         /* IR_CONST_AGG, one per field */
    size_t item_count;
};

/* Constant data, such as the bytes of a string literal. A global with a
   value holds an aggregate constant instead. The back end writes its
   bytes, its size and its alignment before anything reads them. */
struct ir_global {
    uint32_t index;
    const char *module;
    const char *name;
    uint8_t *bytes;
    uint64_t size;
    uint64_t align;
    struct ir_reloc *relocs;
    size_t reloc_count;
    struct ir_const *value;
};

struct ir_module {
    struct arena *arena;
    const char *name;
    struct ir_aggtype **aggs;
    size_t agg_count;
    size_t agg_capacity;
    struct ir_sym *syms;
    size_t sym_count;
    size_t sym_capacity;
    struct ir_function **functions;
    size_t function_count;
    size_t function_capacity;
    struct ir_global **globals;
    size_t global_count;
    size_t global_capacity;
};

void ir_module_init(struct ir_module *m, struct arena *arena,
                    const char *name);
void ir_module_free(struct ir_module *m);

/* Release the heap memory of a block, or of the body of a function. The
   optimizer uses them for blocks and functions it removes. */
void ir_block_free(struct ir_block *b);
void ir_function_free(struct ir_function *f);

struct ir_vtype ir_scalar(enum ir_type type);
struct ir_vtype ir_aggregate(uint32_t agg);

/* Add a struct or union to the type table, or find the one of that name.
   Returns its index. */
uint32_t ir_struct_add(struct ir_module *m, enum ir_agg_kind kind,
                       const char *name, const struct ir_field *fields,
                       size_t count, bool packed, uint64_t align);
/* Add an array of a symbolic length, or find the one of that name. */
uint32_t ir_array_add(struct ir_module *m, const char *name,
                      struct ir_vtype element, uint32_t length,
                      const char *length_text);
/* The index of the aggregate named name, or IR_NO_AGG. */
uint32_t ir_agg_find(const struct ir_module *m, const char *name);

/* Add a symbolic value, or find an equal one. Returns its index. */
uint32_t ir_sym_int(struct ir_module *m, enum ir_type type, uint64_t value);
uint32_t ir_sym_size_of(struct ir_module *m, struct ir_vtype of);
uint32_t ir_sym_offset_of(struct ir_module *m, uint32_t agg, uint32_t field);
uint32_t ir_sym_op(struct ir_module *m, enum ir_op op, enum ir_type type,
                   uint32_t a, uint32_t b);
/* The operand of symbolic value sym: an integer constant for a number,
   else a symbolic operand. */
struct ir_operand ir_sym_operand(const struct ir_module *m, uint32_t sym);

struct ir_function *ir_function_add(struct ir_module *m, const char *module,
                                    const char *name, enum ir_type result,
                                    uint32_t result_agg);
struct ir_function *ir_extern_add(struct ir_module *m, const char *name,
                                  enum ir_type result, bool variadic);
/* A function of another Anti module, which that module's IR defines. */
struct ir_function *ir_declare_add(struct ir_module *m, const char *module,
                                   const char *name, enum ir_type result,
                                   uint32_t result_agg);
uint32_t ir_param_add(struct ir_function *f, enum ir_type type, uint32_t agg);
struct ir_block *ir_block_add(struct ir_function *f);
uint32_t ir_temp(struct ir_function *f, enum ir_type type);

struct ir_global *ir_global_add(struct ir_module *m, const char *module,
                                const char *name, const uint8_t *bytes,
                                uint64_t size, uint64_t align);
void ir_global_reloc(struct ir_module *m, struct ir_global *g,
                     uint64_t offset, uint32_t target);

/* A global that holds the aggregate constant value. Its bytes stay empty
   until the back end lays the type out. */
struct ir_global *ir_global_add_value(struct ir_module *m, const char *module,
                                      const char *name,
                                      struct ir_const *value);

/* Room for an aggregate constant of count items, in the memory pool. */
struct ir_const *ir_const_agg(struct ir_module *m, struct ir_vtype type,
                              size_t count);

/* Whether the two constants hold the same value. */
bool ir_const_equal(const struct ir_const *a, const struct ir_const *b);

struct ir_operand ir_temp_op(const struct ir_function *f, uint32_t temp);
struct ir_operand ir_int_op(enum ir_type type, uint64_t value);
struct ir_operand ir_float_op(enum ir_type type, double value);
struct ir_operand ir_block_op(const struct ir_block *b);
struct ir_operand ir_func_op(const struct ir_function *f);
struct ir_operand ir_global_op(const struct ir_global *g);

/* Append a copy of inst, with a copy of its argument list. The library
   file reader uses it for instructions whose operands it has checked. */
void ir_inst_add(struct ir_block *b, const struct ir_inst *inst);

/* Append an instruction. The emitters with a result create a new
   temporary and return it. ir_assign writes to an existing temporary,
   because the IR is not in static single assignment form. */
uint32_t ir_binary(struct ir_function *f, struct ir_block *b, enum ir_op op,
                   enum ir_type type, struct ir_operand x,
                   struct ir_operand y);
uint32_t ir_unary(struct ir_function *f, struct ir_block *b, enum ir_op op,
                  enum ir_type type, struct ir_operand x);
void ir_assign(struct ir_function *f, struct ir_block *b, uint32_t dst,
               struct ir_operand src);
uint32_t ir_slot(struct ir_function *f, struct ir_block *b,
                 struct ir_vtype of);
/* Add a slot to the entry block, after the slots already there. Lowering
   uses it for a value that it needs in the middle of a function. */
uint32_t ir_entry_slot(struct ir_function *f, struct ir_vtype of);
uint32_t ir_load(struct ir_function *f, struct ir_block *b, enum ir_type type,
                 struct ir_operand pointer);
void ir_store(struct ir_function *f, struct ir_block *b, enum ir_type type,
              struct ir_operand value, struct ir_operand pointer);
uint32_t ir_ptradd(struct ir_function *f, struct ir_block *b,
                   struct ir_operand pointer, struct ir_operand offset);
void ir_memcopy(struct ir_function *f, struct ir_block *b,
                struct ir_operand dst, struct ir_operand src,
                struct ir_vtype of);
uint32_t ir_addr(struct ir_function *f, struct ir_block *b,
                 struct ir_operand target);
/* Read or write bitfield field of aggregate agg in the aggregate at
   pointer. The back end lowers both to shifts and masks. */
uint32_t ir_bitload(struct ir_function *f, struct ir_block *b,
                    enum ir_type type, struct ir_operand pointer, uint32_t agg,
                    uint32_t field);
void ir_bitstore(struct ir_function *f, struct ir_block *b, enum ir_type type,
                 struct ir_operand value, struct ir_operand pointer,
                 uint32_t agg, uint32_t field);
uint32_t ir_call(struct ir_function *f, struct ir_block *b, enum ir_type type,
                 struct ir_operand callee, const struct ir_operand *args,
                 size_t arg_count);
/* A call of the address in target. Operand b names the declared function
   whose parameters and result the call follows. */
uint32_t ir_call_indirect(struct ir_function *f, struct ir_block *b,
                          enum ir_type type, struct ir_operand target,
                          const struct ir_function *signature,
                          const struct ir_operand *args, size_t arg_count);
void ir_jump(struct ir_function *f, struct ir_block *b,
             const struct ir_block *target);
void ir_branch(struct ir_function *f, struct ir_block *b,
               struct ir_operand cond, const struct ir_block *then_block,
               const struct ir_block *else_block);
void ir_ret(struct ir_function *f, struct ir_block *b, enum ir_type type,
            struct ir_operand value);

/* The names of types and operations in the text form. */
const char *ir_type_name(enum ir_type type);
const char *ir_op_name(enum ir_op op);
/* Append the text form of a type in memory, or of a symbolic value. */
void ir_vtype_print(struct text *out, const struct ir_module *m,
                    struct ir_vtype v);
void ir_sym_print(struct text *out, const struct ir_module *m, uint32_t sym);

/* Append the text form of m to out. */
void ir_print(struct text *out, const struct ir_module *m);

/* Check the structural and type rules of the IR. Every violation goes to
   errors, one per line. Returns true when there is none. */
bool ir_verify(const struct ir_module *m, struct text *errors);

#endif
