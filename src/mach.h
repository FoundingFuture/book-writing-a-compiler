#ifndef ANTIC_MACH_H
#define ANTIC_MACH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ir.h"
#include "target.h"
#include "text.h"

/* Machine code holds the instructions of one target before assembly text.
   A virtual register stands for a value, and the allocator of chapter 13
   chooses its register. A physical register is a register of the target,
   used where a calling convention fixes it. */

enum mach_kind {
    MACH_NONE,
    MACH_VREG,      /* A virtual register. */
    MACH_PREG,      /* A physical register. */
    MACH_IMM,       /* An immediate value. */
    MACH_BLOCK,     /* The label of a block. */
    MACH_FUNC,      /* The symbol of an IR function. */
    MACH_COND,      /* A condition code, for cset and setcc. */
    MACH_MEM,       /* Memory at a base register plus an offset. */
    MACH_SLOT,      /* The address of a stack slot, before frame layout. */
    MACH_GLOBAL,    /* The symbol of an IR global. */
    MACH_NAME       /* A symbol outside the IR, such as a runtime helper. */
};

/* How printed machine code names symbols and block labels. The dumps pass
   no names and print IR names and the labels b0, b1. The emitter passes
   its target and the symbol of the function whose blocks it prints. */
struct names {
    enum target target;
    const char *function;
};

/* How a memory operand changes its base register on ARM64. */
enum mach_index { INDEX_NONE, INDEX_PRE, INDEX_POST };

struct mach_operand {
    enum mach_kind kind;
    uint8_t width;          /* The register or memory width in bits. */
    uint32_t reg;           /* MACH_VREG, MACH_PREG, the base of MACH_MEM. */
    bool base_vreg;         /* MACH_MEM: the base is a virtual register. */
    enum mach_index index;  /* MACH_MEM on ARM64. */
    int64_t value;          /* MACH_IMM, BLOCK, FUNC, COND, SLOT, offset. */
    uint32_t index_reg;     /* MACH_MEM with a scale: the index register. */
    bool index_vreg;        /* The index is a virtual register. */
    uint8_t scale;          /* MACH_MEM: 0 without an index, else 1 to 8. */
    bool pc_relative;       /* MACH_FUNC, MACH_GLOBAL as an address. */
    bool got;               /* MACH_FUNC: its entry in the GOT. */
    const char *name;       /* MACH_NAME. */
};

/* Conditions after a comparison of a with b. LO, LS, HI and HS compare
   unsigned values. */
enum mach_cond {
    COND_EQ, COND_NE, COND_LT, COND_LE, COND_GT, COND_GE,
    COND_LO, COND_LS, COND_HI, COND_HS,
    COND_MI, COND_PL,       /* ARM64: the sign flag, set for less */
    COND_P, COND_NP         /* x86_64: the parity flag, set for NaN */
};

enum { MACH_MAX_OPERANDS = 4 };

/* Operands follow the destination-first order of the target's own
   instruction set. The x86_64 printer reverses them for AT&T syntax. */
struct mach_inst {
    uint16_t op;                    /* an opcode of the target */
    uint8_t count;
    struct mach_operand operands[MACH_MAX_OPERANDS];
    uint64_t uses;                  /* physical registers read implicitly */
    uint64_t defs;                  /* physical registers written implicitly */
};

struct mach_block {
    struct mach_inst *insts;
    size_t count;
    size_t capacity;
    uint32_t loop_depth;        /* the loops this block sits inside */
};

/* A stack slot of the function: an IR slot or a spilled register. */
struct mach_slot {
    uint64_t size;
    uint64_t align;
    int64_t offset;         /* from the stack pointer, after frame layout */
};

struct mach_function {
    const struct ir_function *ir;
    struct mach_block *blocks;
    size_t block_count;
    uint32_t vreg_count;
    struct mach_slot *slots;
    size_t slot_count;
    size_t slot_capacity;
    uint64_t outgoing;      /* bytes of stack arguments of the largest call */
    bool stack_params;      /* a parameter arrives on the stack */
    bool unwind;            /* Windows unwind data */
    bool *fp;               /* each virtual register: in the float class */
    size_t fp_capacity;
};

/* How an opcode treats each register operand. The base register of a
   memory operand is always read. */
enum mach_role { ROLE_USE = 1, ROLE_DEF = 2 };

/* What an opcode does to control flow, and whether it is a move of one
   register into another. */
enum mach_flag {
    FLAG_JUMP = 1,          /* always leaves the block */
    FLAG_BRANCH = 2,        /* may jump to a block operand */
    FLAG_RET = 4,
    FLAG_CALL = 8,
    FLAG_MOVE = 16
};

struct mach_opcode {
    const char *name;
    uint8_t roles[MACH_MAX_OPERANDS];
    uint8_t flags;
};

struct target_desc;

struct mach_inst *mach_append(struct mach_block *b);
uint32_t mach_slot_add(struct mach_function *f, uint64_t size,
                       uint64_t align);
/* Add a virtual register to f, of the float class when fp is set and of
   the integer class otherwise. Returns its number. */
uint32_t mach_vreg_add(struct mach_function *f, bool fp);

void mach_function_free(struct mach_function *f);

/* Append the text of f, one instruction per line. */
void mach_print(struct text *out, const struct target_desc *target,
                const struct ir_module *m, const struct mach_function *f);

/* Append the symbol or label of operand o, of kind MACH_BLOCK, MACH_FUNC,
   MACH_GLOBAL or MACH_NAME. An IR function without a module and a name
   outside the IR are C functions. */
void mach_symbol(struct text *out, const struct ir_module *m,
                 const struct names *names, const struct mach_operand *o);

/* Append the symbol of function f for target t. A C function and an
   export fn have their C name, and every other function its mangled name. */
void mach_function_symbol(struct text *out, enum target t,
                          const struct ir_function *f);

#endif
