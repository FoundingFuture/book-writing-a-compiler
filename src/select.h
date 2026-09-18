#ifndef ANTIC_SELECT_H
#define ANTIC_SELECT_H

#include <stdbool.h>

#include "ir.h"
#include "layout.h"
#include "mach.h"
#include "target.h"
#include "text.h"

/* Instruction selection turns the IR of one function into machine code on
   virtual registers. The shared part walks the blocks and gives each IR
   temporary a virtual register. It finds the pattern of each instruction
   in the table of the target. */

struct selector;

/* One row of a target's pattern table. The first row whose operation
   matches and whose match function accepts the instruction emits it. A
   NULL match function accepts every instruction of the operation. */
struct pattern {
    enum ir_op op;
    bool (*match)(const struct selector *s, const struct ir_inst *inst);
    void (*emit)(struct selector *s, const struct ir_inst *inst);
};

/* The registers of a calling convention, as physical register numbers of
   the target. The float registers have numbers above the integer ones. */
struct abi {
    const uint8_t *int_args;
    size_t int_arg_count;
    uint8_t int_result;
    uint64_t caller_saved;
    uint64_t callee_saved;
    const uint8_t *allocatable;         /* in the order of preference */
    size_t allocatable_count;
    uint8_t scratch[2];                 /* for spilled registers */
    uint8_t shadow_space;               /* bytes a caller reserves */
    bool probe_stack;                   /* frames of a page call a probe */
    const uint8_t *fp_args;
    size_t fp_arg_count;
    uint8_t fp_result;
    const uint8_t *fp_allocatable;
    size_t fp_allocatable_count;
    uint8_t fp_scratch[2];
    uint8_t fp_save_size;               /* bytes a callee saves of one */
};

/* One register part of an aggregate: bytes at offset go to reg. */
struct arg_part {
    uint8_t reg;
    uint8_t bytes;
    uint8_t offset;
};

/* Where an argument or a parameter goes: a register, or an offset in the
   argument area on the stack. A variadic float on Windows also goes to the
   integer register copy. An aggregate goes to register parts, or with its
   size to the stack, or when indirect as a pointer to a copy. */
struct arg_location {
    bool stack;
    uint8_t reg;
    int64_t offset;
    int copy;                           /* a second register, or -1 */
    bool indirect;
    size_t part_count;
    struct arg_part parts[4];
    uint64_t size;
};

/* A memory address that selection folds into a load or a store: base
   plus index shifted left by shift plus offset. */
struct address {
    const struct ir_operand *base;
    const struct ir_operand *index;     /* NULL without an index */
    uint8_t shift;
    int64_t offset;
};

struct frame;

struct target_desc {
    const char *name;                   /* x86_64 or arm64 */
    int chapter;                        /* the chapter with all patterns */
    const struct mach_opcode *opcodes;
    const struct pattern *patterns;
    size_t pattern_count;
    const struct abi *(*abi)(enum convention convention);
    uint8_t (*width)(enum ir_type type);
    void (*move)(struct selector *s, struct mach_operand dst,
                 struct mach_operand src);
    void (*load)(struct selector *s, struct mach_operand dst, uint64_t value);
    void (*print)(struct text *out, const struct ir_module *m,
                  const struct mach_inst *inst, const struct names *names);
    /* Addressing modes and stack parameters, chapters 14 and 15. */
    bool (*fits_address)(const struct selector *s, const struct address *a,
                         const struct ir_inst *use);
    void (*stack_param)(struct selector *s, int64_t offset,
                        struct mach_operand dst);
    /* The locations of count arguments of types for a call of callee, or
       of the parameters of callee. */
    void (*locate)(const struct selector *s, const struct ir_function *callee,
                   const enum ir_type *types, size_t count,
                   struct arg_location *out);
    void (*load_float)(struct selector *s, struct mach_operand dst,
                       enum ir_type type, double value);
    /* Aggregates, chapter 18. The result of f goes to a register or to the
       register parts of an aggregate. An indirect result goes to memory
       whose address arrives in reg and, with copy set, returns in copy. */
    void (*locate_result)(const struct selector *s,
                          const struct ir_function *f,
                          struct arg_location *out);
    void (*load_bytes)(struct selector *s, struct mach_operand dst,
                       struct mach_operand base, int64_t offset,
                       unsigned bytes);
    void (*store_bytes)(struct selector *s, struct mach_operand src,
                        struct mach_operand base, int64_t offset,
                        unsigned bytes);
    void (*slot_address)(struct selector *s, struct mach_operand dst,
                         uint32_t slot);
    void (*incoming_address)(struct selector *s, struct mach_operand dst,
                             int64_t offset);
    void (*copy_memory)(struct selector *s, struct mach_operand dst,
                        struct mach_operand src, uint64_t size);
    uint64_t frame_limit;               /* 0 for no limit */
    /* Register allocation and frame layout, chapter 13. */
    void (*load_spill)(struct mach_block *b, uint8_t reg, int64_t offset);
    void (*store_spill)(struct mach_block *b, uint8_t reg, int64_t offset);
    void (*resolve_slot)(struct mach_operand *o, int64_t offset);
    /* Append inst after allocation, split where a frame offset exceeds an
       immediate of the target. NULL appends it unchanged. */
    void (*expand)(struct mach_block *b, const struct mach_inst *inst);
    void (*prologue)(struct mach_block *b, const struct frame *frame);
    void (*epilogue)(struct mach_block *b, const struct frame *frame);
};

struct selector {
    const struct target_desc *target;
    const struct abi *abi;
    enum convention convention;
    const struct ir_module *m;
    struct layouts *layouts;            /* of every aggregate of m */
    const struct ir_function *f;
    struct mach_function *out;
    struct mach_block *b;
    size_t block;                       /* the index of the IR block */
    uint32_t *uses;                     /* uses of each IR temporary */
    const struct ir_inst *fused;        /* a comparison for the branch */
    struct address address;             /* for the next load or store */
    bool has_address;
    struct mach_operand result_address; /* of an aggregate result */
    char *error;
    size_t error_size;
    bool failed;
};

const struct target_desc *target_desc_x86_64(void);
const struct target_desc *target_desc_arm64(void);
const struct target_desc *target_desc(enum target t);

/* Select the machine code of every function with a body in m into out,
   one entry per IR function, NULL for an extern function. Selection first
   lays out the aggregates of m for the target and replaces its symbolic
   values with constants. Returns false and writes a message to error for
   a layout error or an instruction without a pattern. */
bool select_module(enum target t, struct ir_module *m,
                   struct mach_function **out, char *error,
                   size_t error_size);

/* The layout of aggregate agg on the target, NULL for IR_NO_AGG. */
const struct layout *select_layout(const struct selector *s, uint32_t agg);
uint64_t select_size(const struct selector *s, struct ir_vtype v);
uint64_t select_align(const struct selector *s, struct ir_vtype v);

/* Helpers for the pattern tables of the targets. */
struct mach_operand mach_vreg(uint32_t vreg, uint8_t width);
struct mach_operand mach_preg(uint32_t preg, uint8_t width);
struct mach_operand mach_imm(int64_t value);
struct mach_operand select_result(struct selector *s, const struct ir_inst *inst);
struct mach_operand select_new_vreg(struct selector *s, uint8_t width);
struct mach_operand select_new_fp_vreg(struct selector *s, uint8_t width);
bool select_is_float(enum ir_type type);
/* The register of physical register reg with the width of bytes of an
   aggregate part: 64 bits above 4 bytes. */
struct mach_operand select_part_register(uint8_t reg, unsigned bytes);
/* Store the register parts of an aggregate into the memory at address. */
void select_store_parts(struct selector *s, const struct arg_location *loc,
                        struct mach_operand address);
/* Load the register parts of an aggregate from address. Returns the
   registers it loads as a set. */
uint64_t select_load_parts(struct selector *s, const struct arg_location *loc,
                           struct mach_operand address);
/* An aggregate result of a call: memory in a new slot for its value. */
/* The function whose parameters and result a call follows: the callee of
   a direct call, or the signature of a call through a pointer. */
const struct ir_function *select_callee(const struct selector *s,
                                        const struct ir_inst *inst);
/* DESIGN: a PIE on Linux and macOS reaches a C function in a shared
   library through its GOT entry. Windows links the C runtime statically,
   so its C functions lie in the executable. */
bool select_uses_got(const struct selector *s, const struct ir_operand *o);
struct mach_operand select_result_slot(struct selector *s,
                                       const struct ir_inst *inst,
                                       const struct layout *agg);
struct mach_operand select_reg(struct selector *s,
                               const struct ir_operand *o);
struct mach_inst *select_emit(struct selector *s, uint16_t op, size_t count,
                              const struct mach_operand *operands);
enum mach_cond select_cond(enum ir_op op);
enum mach_cond select_negate(enum mach_cond cond);
bool select_is_next(const struct selector *s, const struct ir_operand *block);
void select_refuse(struct selector *s, const struct ir_inst *inst);
void select_fail(struct selector *s, const char *message);

#endif
