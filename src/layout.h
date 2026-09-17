#ifndef ANTIC_LAYOUT_H
#define ANTIC_LAYOUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ir.h"
#include "target.h"

/* The back end lays out the aggregates of the IR for one target and folds
   its symbolic values. Chapter 18 gives the rules. */

/* One scalar inside an aggregate, for the classification of its ABI. */
struct layout_member {
    uint64_t offset;
    enum ir_type type;
};

/* Where a bitfield lies, and the integer that a load or store of it reads
   and writes. */
struct layout_bits {
    uint64_t pos;                   /* the first bit, from bit 0 of byte 0 */
    uint64_t unit_offset;           /* the byte the integer starts at */
    enum ir_type unit_type;
    uint8_t shift;                  /* the first bit inside the integer */
};

/* The layout of an aggregate on the target. */
struct layout {
    uint64_t size;
    uint64_t align;
    uint64_t *offsets;              /* the offset of each field */
    struct layout_bits *bits;       /* for each field, set for bitfields */
    struct layout_member *members;  /* recorded up to LAYOUT_MEMBER_LIMIT */
    size_t member_count;
    bool unaligned;                 /* a field lies off its alignment */
};

/* DESIGN: the members of an aggregate are recorded up to 32 bytes. Every
   convention passes a larger aggregate in memory, and a float aggregate
   of AAPCS64 has at most four members of 8 bytes. */
enum { LAYOUT_MEMBER_LIMIT = 32 };

struct layouts {
    enum target target;
    const struct ir_module *m;
    struct layout *aggs;
    uint8_t *agg_state;
    uint64_t *values;
    uint8_t *sym_state;
    char *error;
    size_t error_size;
    bool failed;
};

/* Lay out every aggregate of m for target t. Returns false and writes a
   message to error for an array length below 1 or a layout that depends
   on itself. layouts_free releases the tables in either case. The tables
   cover the aggregates and symbolic values that m holds at this call. */
bool layouts_init(struct layouts *l, enum target t, const struct ir_module *m,
                  char *error, size_t error_size);
void layouts_free(struct layouts *l);

const struct layout *layout_agg(struct layouts *l, uint32_t agg);
uint64_t layout_size(struct layouts *l, struct ir_vtype v);
uint64_t layout_align(struct layouts *l, struct ir_vtype v);

/* Fold symbolic value sym to its bits on the target. Returns false after
   a division by zero. */
bool layout_fold(struct layouts *l, uint32_t sym, uint64_t *out);

/* Replace every symbolic operand of function f of the module with a
   constant, and set resolved when there was one. Returns false after a
   folding error. */
bool layout_resolve(struct layouts *l, struct ir_function *f, bool *resolved);

/* Write the bytes of every global that holds an aggregate constant, with
   its size and its alignment on the target. Returns false after a folding
   error. */
bool layout_data(struct layouts *l, struct ir_module *m);

#endif
