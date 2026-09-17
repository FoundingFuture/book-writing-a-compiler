#include "layout.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { UNSEEN, BUSY, DONE };

static void *allocate(size_t count, size_t size)
{
    void *p = calloc(count + 1, size);

    if (p == NULL) {
        fputs("antic: out of memory\n", stderr);
        exit(70);
    }
    return p;
}

static void fail(struct layouts *l, const char *format, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

static void fail(struct layouts *l, const char *format, ...)
{
    va_list args;

    if (l->failed) {
        return;
    }
    va_start(args, format);
    vsnprintf(l->error, l->error_size, format, args);
    va_end(args);
    l->failed = true;
}

/* The fixed-width IR type of c_long or c_wchar on the target, or type. */
static enum ir_type target_type(const struct layouts *l, enum ir_type type)
{
    bool windows = target_info(l->target)->os == OS_WINDOWS;

    if (type == IR_CLONG) {
        return windows ? IR_I32 : IR_I64;
    }
    if (type == IR_CWCHAR) {
        return windows ? IR_I16 : IR_I32;
    }
    return type;
}

/* Every scalar is as large as it is aligned, on all six targets. */
static uint64_t scalar_size(const struct layouts *l, enum ir_type type)
{
    switch (target_type(l, type)) {
    case IR_I8: return 1;
    case IR_I16: return 2;
    case IR_I32:
    case IR_F32: return 4;
    default: return 8;
    }
}

static uint64_t round_up(uint64_t n, uint64_t align)
{
    return (n + align - 1) / align * align;
}

static void compute(struct layouts *l, uint32_t agg);

uint64_t layout_size(struct layouts *l, struct ir_vtype v)
{
    return v.type == IR_AGG ? layout_agg(l, v.agg)->size
                            : scalar_size(l, v.type);
}

uint64_t layout_align(struct layouts *l, struct ir_vtype v)
{
    return v.type == IR_AGG ? layout_agg(l, v.agg)->align
                            : scalar_size(l, v.type);
}

const struct layout *layout_agg(struct layouts *l, uint32_t agg)
{
    if (l->agg_state[agg] != DONE) {
        compute(l, agg);
    }
    return &l->aggs[agg];
}

/* Whether f is a zero-width bitfield, `_: T : 0`, which holds no value. */
static bool is_unit_break(const struct ir_field *f)
{
    return f->name != NULL && strcmp(f->name, "_") == 0;
}

struct members {
    struct layout_member items[LAYOUT_MEMBER_LIMIT];
    size_t count;
};

/* The scalars of a value of type v at offset. Every field of a union
   starts at the union's own offset. Two equal scalars at one offset are
   one member. */
static void flatten(struct layouts *l, struct ir_vtype v, uint64_t offset,
                    struct members *out)
{
    const struct ir_aggtype *t;
    const struct layout *layout;
    uint64_t step;
    uint64_t i;

    if (v.type != IR_AGG) {
        for (i = 0; i < out->count; i++) {
            if (out->items[i].offset == offset &&
                out->items[i].type == target_type(l, v.type)) {
                return;
            }
        }
        if (out->count < LAYOUT_MEMBER_LIMIT) {
            out->items[out->count].offset = offset;
            out->items[out->count].type = target_type(l, v.type);
            out->count++;
        }
        return;
    }
    t = l->m->aggs[v.agg];
    layout = layout_agg(l, v.agg);
    if (t->kind == IR_AGG_ARRAY) {
        step = layout_size(l, t->fields[0].type);
        for (i = 0; step > 0 && i < layout->size / step; i++) {
            flatten(l, t->fields[0].type, offset + i * step, out);
        }
        return;
    }
    for (i = 0; i < t->field_count; i++) {
        if (!is_unit_break(&t->fields[i])) {
            flatten(l, t->fields[i].type, offset + layout->offsets[i], out);
        }
    }
}

/* The integer a load or store of a bitfield reads. It is the smallest
   integer that holds the field's bits. It starts at the byte of the first
   bit, or earlier when it would end past the aggregate. */
static bool bit_unit(struct layouts *l, const struct ir_aggtype *t,
                     const struct layout *out, size_t i)
{
    struct layout_bits *b = &out->bits[i];
    uint64_t width = t->fields[i].bits;
    uint64_t need = (b->pos % 8 + width + 7) / 8;
    uint64_t bytes = need <= 1 ? 1 : need <= 2 ? 2 : need <= 4 ? 4 : 8;

    if (need > 8) {
        fail(l, "the bitfield `%s` of `%s` spans more than 8 bytes on %s",
             t->fields[i].name, t->name, target_name(l->target));
        return false;
    }
    b->unit_offset = b->pos / 8;
    if (b->unit_offset + bytes > out->size) {
        b->unit_offset = out->size - bytes;
    }
    b->unit_type = bytes == 1 ? IR_I8 : bytes == 2 ? IR_I16
                   : bytes == 4 ? IR_I32 : IR_I64;
    b->shift = (uint8_t)(b->pos - b->unit_offset * 8);
    return true;
}

/* The C rules of chapter 18. A struct places each field at the next
   multiple of its alignment, and a union places every field at 0. packed
   aligns every field to 1, and align(N) raises the aggregate's alignment.
   The size rounds up to the alignment.

   DESIGN: bitfields follow the rule of the target's C compiler. System V
   on Linux and macOS puts a bitfield at the next free bit, unless it would
   cross a boundary of its type's alignment. MSVC on Windows gives
   consecutive bitfields of one size a shared unit of that size. It opens
   a new unit for another size or when the bits run out. */
static void compute(struct layouts *l, uint32_t agg)
{
    const struct ir_aggtype *t = l->m->aggs[agg];
    struct layout *out = &l->aggs[agg];
    bool msvc = target_info(l->target)->os == OS_WINDOWS;
    bool aapcs64 = target_info(l->target)->os == OS_LINUX &&
                   target_info(l->target)->arch == ARCH_ARM64;
    uint64_t bit = 0;
    uint64_t align = 1;
    uint64_t unit_start = 0;
    uint64_t unit_size = 0;
    uint64_t unit_used = 0;
    uint64_t length;
    struct members members;
    size_t i;

    if (l->agg_state[agg] == BUSY) {
        fail(l, "the layout of `%s` depends on itself", t->name);
        out->size = out->align = 1;
        return;
    }
    l->agg_state[agg] = BUSY;
    out->offsets = allocate(t->field_count, sizeof *out->offsets);
    out->bits = allocate(t->field_count, sizeof *out->bits);
    if (t->kind == IR_AGG_ARRAY) {
        struct ir_vtype element = t->fields[0].type;
        if (!layout_fold(l, t->length, &length)) {
            length = 1;
        } else if ((int64_t)length < 1) {
            fail(l, "the array length `%s` is %" PRId64 " on %s, and an "
                    "array length is at least 1",
                 t->length_text, (int64_t)length, target_name(l->target));
            length = 1;
        }
        bit = length * layout_size(l, element) * 8;
        align = layout_align(l, element);
        out->unaligned = element.type == IR_AGG &&
                         layout_agg(l, element.agg)->unaligned;
    }
    for (i = 0; t->kind != IR_AGG_ARRAY && i < t->field_count; i++) {
        struct ir_vtype type = t->fields[i].type;
        uint64_t width = t->fields[i].bits;
        uint64_t size = layout_size(l, type);
        uint64_t natural = layout_align(l, type);
        uint64_t field_align = t->packed ? 1 : natural;
        uint64_t end = 0;
        if (is_unit_break(&t->fields[i])) {
            /* DESIGN: a zero-width bitfield breaks the unit as C does.
               System V aligns the next field to T, AAPCS64 outside Apple
               also the struct, and MSVC acts only after a bitfield. */
            if (!msvc) {
                bit = round_up(bit, natural * 8);
                align = aapcs64 && natural > align ? natural : align;
            } else if (unit_size != 0) {
                bit = round_up((bit + 7) / 8, field_align) * 8;
                align = field_align > align ? field_align : align;
                unit_size = 0;
            }
            out->offsets[i] = bit / 8;
            continue;
        }
        if (t->kind == IR_AGG_UNION) {
            out->offsets[i] = 0;
            end = width == 0 || msvc ? size * 8 : width;
        } else if (width == 0) {
            unit_size = 0;
            out->offsets[i] = round_up((bit + 7) / 8, field_align);
            bit = (out->offsets[i] + size) * 8;
        } else if (msvc && unit_size == size && unit_used + width <= size * 8) {
            out->bits[i].pos = unit_start + unit_used;
            unit_used += width;
        } else if (msvc) {
            unit_start = round_up((bit + 7) / 8, field_align) * 8;
            unit_size = size;
            unit_used = width;
            out->bits[i].pos = unit_start;
            bit = unit_start + size * 8;
        } else {
            if (!t->packed && bit % (natural * 8) + width > size * 8) {
                bit = round_up(bit, natural * 8);
            }
            out->bits[i].pos = bit;
            bit += width;
        }
        if (width != 0 && t->kind != IR_AGG_UNION) {
            out->offsets[i] = out->bits[i].pos / 8;
        }
        bit = end > bit ? end : bit;
        align = field_align > align ? field_align : align;
        if ((width == 0 && out->offsets[i] % natural != 0) ||
            (type.type == IR_AGG && layout_agg(l, type.agg)->unaligned)) {
            out->unaligned = true;
        }
    }
    /* DESIGN: align(N) raises the alignment, as C's _Alignas does, and a
       value below the alignment the fields give is an error, as in C. */
    if (t->align != 0 && t->align < align) {
        fail(l, "`%s` has align(%" PRIu64 "), below its alignment %" PRIu64
                " on %s", t->name, t->align, align, target_name(l->target));
    } else if (t->align > align) {
        align = t->align;
    }
    out->align = align;
    out->size = round_up((bit + 7) / 8, align);
    for (i = 0; t->kind != IR_AGG_ARRAY && i < t->field_count; i++) {
        if (t->fields[i].bits != 0 && !bit_unit(l, t, out, i)) {
            break;
        }
    }
    l->agg_state[agg] = DONE;
    if (out->size <= LAYOUT_MEMBER_LIMIT) {
        members.count = 0;
        flatten(l, ir_aggregate(agg), 0, &members);
        out->members = allocate(members.count, sizeof *out->members);
        memcpy(out->members, members.items,
               members.count * sizeof *out->members);
        out->member_count = members.count;
    }
}

static int bits(enum ir_type type)
{
    return type == IR_I8 ? 8 : type == IR_I16 ? 16 : type == IR_I32 ? 32 : 64;
}

static uint64_t trim(enum ir_type type, uint64_t v)
{
    int n = bits(type);
    return n == 64 ? v : v & (((uint64_t)1 << n) - 1);
}

static int64_t signed_value(enum ir_type type, uint64_t v)
{
    int n = bits(type);
    uint64_t sign = (uint64_t)1 << (n - 1);

    v = trim(type, v);
    return (int64_t)((v ^ sign) - sign);
}

/* Apply op to the folded operands a and b of type type. The result has
   the type of the symbolic value. */
static bool fold_op(struct layouts *l, const struct ir_sym *s,
                    enum ir_type type, uint64_t a, uint64_t b, uint64_t *out)
{
    enum ir_type result = target_type(l, s->type);

    bool divides = s->op == IR_SDIV || s->op == IR_UDIV ||
                   s->op == IR_SREM || s->op == IR_UREM;

    if (divides && trim(type, b) == 0) {
        fail(l, "a size expression divides by zero on %s",
             target_name(l->target));
        return false;
    }
    switch ((enum ir_op)s->op) {
    case IR_ADD: *out = a + b; break;
    case IR_SUB: *out = a - b; break;
    case IR_MUL: *out = a * b; break;
    case IR_SDIV:
        *out = (uint64_t)(signed_value(type, a) / signed_value(type, b));
        break;
    case IR_UDIV: *out = trim(type, a) / trim(type, b); break;
    case IR_SREM:
        *out = (uint64_t)(signed_value(type, a) % signed_value(type, b));
        break;
    case IR_UREM: *out = trim(type, a) % trim(type, b); break;
    case IR_AND: *out = a & b; break;
    case IR_OR: *out = a | b; break;
    case IR_XOR: *out = a ^ b; break;
    case IR_SHL: *out = a << (b % 64); break;
    case IR_SHR_S:
        *out = (uint64_t)(signed_value(type, a) >> (b % 64));
        break;
    case IR_SHR_U: *out = trim(type, a) >> (b % 64); break;
    case IR_NEG: *out = 0 - a; break;
    case IR_NOT: *out = ~a; break;
    case IR_EQ: *out = trim(type, a) == trim(type, b); break;
    case IR_NE: *out = trim(type, a) != trim(type, b); break;
    case IR_SLT: *out = signed_value(type, a) < signed_value(type, b); break;
    case IR_SLE: *out = signed_value(type, a) <= signed_value(type, b); break;
    case IR_SGT: *out = signed_value(type, a) > signed_value(type, b); break;
    case IR_SGE: *out = signed_value(type, a) >= signed_value(type, b); break;
    case IR_ULT: *out = trim(type, a) < trim(type, b); break;
    case IR_ULE: *out = trim(type, a) <= trim(type, b); break;
    case IR_UGT: *out = trim(type, a) > trim(type, b); break;
    case IR_UGE: *out = trim(type, a) >= trim(type, b); break;
    case IR_TRUNC:
    case IR_ZEXT: *out = trim(type, a); break;
    case IR_SEXT: *out = (uint64_t)signed_value(type, a); break;
    default:
        fail(l, "a size expression uses `%s`, which does not fold",
             ir_op_name((enum ir_op)s->op));
        return false;
    }
    *out = trim(result, *out);
    return true;
}

bool layout_fold(struct layouts *l, uint32_t sym, uint64_t *out)
{
    const struct ir_sym *s = &l->m->syms[sym];
    uint64_t a = 0;
    uint64_t b = 0;
    bool ok = true;

    if (l->sym_state[sym] == DONE) {
        *out = l->values[sym];
        return true;
    }
    switch (s->kind) {
    case IR_SYM_INT:
        *out = s->value;
        break;
    case IR_SYM_SIZE_OF:
        *out = layout_size(l, s->of);
        break;
    case IR_SYM_OFFSET_OF:
        *out = layout_agg(l, s->of.agg)->offsets[s->field];
        break;
    case IR_SYM_OP:
        ok = layout_fold(l, s->a, &a) &&
             (s->b == IR_NO_AGG || layout_fold(l, s->b, &b));
        /* The operands of a comparison or conversion have their own type,
           and the value of the operation has the result type. */
        ok = ok && fold_op(l, s, target_type(l, l->m->syms[s->a].type), a, b,
                           out);
        break;
    }
    if (ok && !l->failed) {
        l->values[sym] = *out;
        l->sym_state[sym] = DONE;
    }
    return ok && !l->failed;
}

bool layouts_init(struct layouts *l, enum target t, const struct ir_module *m,
                  char *error, size_t error_size)
{
    size_t i;

    memset(l, 0, sizeof *l);
    l->target = t;
    l->m = m;
    l->error = error;
    l->error_size = error_size;
    l->aggs = allocate(m->agg_count, sizeof *l->aggs);
    l->agg_state = allocate(m->agg_count, sizeof *l->agg_state);
    l->values = allocate(m->sym_count, sizeof *l->values);
    l->sym_state = allocate(m->sym_count, sizeof *l->sym_state);
    for (i = 0; i < m->agg_count && !l->failed; i++) {
        layout_agg(l, (uint32_t)i);
    }
    return !l->failed;
}

void layouts_free(struct layouts *l)
{
    size_t i;

    for (i = 0; l->aggs != NULL && i < l->m->agg_count; i++) {
        free(l->aggs[i].offsets);
        free(l->aggs[i].bits);
        free(l->aggs[i].members);
    }
    free(l->aggs);
    free(l->agg_state);
    free(l->values);
    free(l->sym_state);
    memset(l, 0, sizeof *l);
}

/* Give a target-sized type its width on the target. */
static void resolve_type(const struct layouts *l, enum ir_type *type,
                         bool *resolved)
{
    enum ir_type fixed = target_type(l, *type);

    if (fixed != *type) {
        *type = fixed;
        *resolved = true;
    }
}

static bool resolve(struct layouts *l, struct ir_operand *o, bool *resolved)
{
    uint64_t value;

    resolve_type(l, &o->type, resolved);
    if (o->kind == IR_INT) {
        *o = ir_int_op(o->type, o->as.integer);
    }
    if (o->kind != IR_SYM) {
        return true;
    }
    if (!layout_fold(l, o->as.index, &value)) {
        return false;
    }
    *o = ir_int_op(o->type, value);
    *resolved = true;
    return true;
}

static bool is_conversion(enum ir_op op)
{
    return op == IR_TRUNC || op == IR_SEXT || op == IR_ZEXT;
}

static struct ir_operand temp_of(const struct ir_function *f, uint32_t t)
{
    return ir_temp_op(f, t);
}

/* Convert v of integer type from to integer type to, extending by ext. */
static struct ir_operand convert(struct ir_function *f, struct ir_block *b,
                                 struct ir_operand v, enum ir_type from,
                                 enum ir_type to, enum ir_ext ext)
{
    enum ir_op op;

    if (bits(from) == bits(to)) {
        return v;
    }
    op = bits(to) < bits(from) ? IR_TRUNC
         : ext == IR_EXT_SIGN  ? IR_SEXT
                               : IR_ZEXT;
    return temp_of(f, ir_unary(f, b, op, to, v));
}

static uint64_t low_bits(uint64_t width)
{
    return width == 64 ? UINT64_MAX : ((uint64_t)1 << width) - 1;
}

/* DESIGN: a bitfield load reads the integer that holds the field. It
   shifts the field down and masks it. A signed field shifts to the top and
   back with an arithmetic shift. A store reads the integer, clears the
   field's bits, puts the new bits in and writes the integer back. It is
   the sequence a C compiler emits. */
static void lower_bits(struct layouts *l, struct ir_function *f,
                       struct ir_block *out, const struct ir_inst *inst)
{
    const struct ir_field *field = &l->m->aggs[inst->of.agg]->fields[inst->field];
    const struct layout_bits *where =
        &layout_agg(l, inst->of.agg)->bits[inst->field];
    enum ir_type unit = where->unit_type;
    int unit_bits = bits(unit);
    uint64_t width = field->bits;
    const struct ir_operand *base = inst->op == IR_BITLOAD ? &inst->a
                                                          : &inst->b;
    struct ir_operand pointer = *base;
    struct ir_operand word;
    struct ir_operand v;

    if (where->unit_offset != 0) {
        pointer = temp_of(f, ir_ptradd(f, out, pointer,
                                       ir_int_op(IR_I64, where->unit_offset)));
    }
    word = temp_of(f, ir_load(f, out, unit, pointer));
    if (inst->op == IR_BITLOAD && field->ext == IR_EXT_SIGN) {
        int up = unit_bits - where->shift - (int)width;
        v = up == 0 ? word
                    : temp_of(f, ir_binary(f, out, IR_SHL, unit, word,
                                           ir_int_op(unit, (uint64_t)up)));
        if (width < (uint64_t)unit_bits) {
            v = temp_of(f, ir_binary(f, out, IR_SHR_S, unit, v,
                                     ir_int_op(unit, (uint64_t)unit_bits - width)));
        }
        v = convert(f, out, v, unit, inst->type, IR_EXT_SIGN);
        ir_assign(f, out, inst->result, v);
        return;
    }
    if (inst->op == IR_BITLOAD) {
        v = where->shift == 0
                ? word
                : temp_of(f, ir_binary(f, out, IR_SHR_U, unit, word,
                                       ir_int_op(unit, where->shift)));
        if (width < (uint64_t)unit_bits) {
            v = temp_of(f, ir_binary(f, out, IR_AND, unit, v,
                                     ir_int_op(unit, low_bits(width))));
        }
        v = convert(f, out, v, unit, inst->type, IR_EXT_ZERO);
        ir_assign(f, out, inst->result, v);
        return;
    }
    v = convert(f, out, inst->a, inst->type, unit, IR_EXT_ZERO);
    if (width < (uint64_t)unit_bits) {
        v = temp_of(f, ir_binary(f, out, IR_AND, unit, v,
                                 ir_int_op(unit, low_bits(width))));
    }
    if (where->shift != 0) {
        v = temp_of(f, ir_binary(f, out, IR_SHL, unit, v,
                                 ir_int_op(unit, where->shift)));
    }
    word = temp_of(f, ir_binary(f, out, IR_AND, unit, word,
                                ir_int_op(unit,
                                          ~(low_bits(width) << where->shift))));
    v = temp_of(f, ir_binary(f, out, IR_OR, unit, word, v));
    ir_store(f, out, unit, v, pointer);
}

/* Replace every bitfield load and store of block b with the instructions
   that lower_bits gives it. */
static void lower_block_bits(struct layouts *l, struct ir_function *f,
                             struct ir_block *b)
{
    struct ir_block out;
    size_t i;

    memset(&out, 0, sizeof out);
    for (i = 0; i < b->count; i++) {
        const struct ir_inst *inst = &b->insts[i];
        if (inst->op == IR_BITLOAD || inst->op == IR_BITSTORE) {
            lower_bits(l, f, &out, inst);
        } else {
            ir_inst_add(&out, inst);
        }
        free(b->insts[i].args);
    }
    free(b->insts);
    b->insts = out.insts;
    b->count = out.count;
    b->capacity = out.capacity;
}

/* Whether wchar_t is signed on the target, as clang defines it. It is int
   on Linux x86_64 and macOS, unsigned int on Linux ARM64 and unsigned
   short on Windows. */
static bool wchar_signed(const struct layouts *l)
{
    const struct target_info *t = target_info(l->target);

    return t->os == OS_MACOS ||
           (t->os == OS_LINUX && t->arch == ARCH_X86_64);
}

/* The signed form of an operation that lowering writes unsigned. */
static enum ir_op signed_op(enum ir_op op)
{
    switch (op) {
    case IR_UDIV: return IR_SDIV;
    case IR_UREM: return IR_SREM;
    case IR_SHR_U: return IR_SHR_S;
    case IR_ULT: return IR_SLT;
    case IR_ULE: return IR_SLE;
    case IR_UGT: return IR_SGT;
    case IR_UGE: return IR_SGE;
    case IR_ZEXT: return IR_SEXT;
    case IR_UITOF: return IR_SITOF;
    case IR_FTOUI: return IR_FTOSI;
    default: return op;
    }
}

/* DESIGN: c_wchar has the signedness of wchar_t on the target. Lowering
   writes the unsigned form of every operation on it, and the back end
   turns that into the signed form where wchar_t is signed. */
static void wchar_signedness(const struct layouts *l, struct ir_inst *inst)
{
    bool on_wchar = inst->type == IR_CWCHAR || inst->a.type == IR_CWCHAR;

    if (on_wchar && wchar_signed(l)) {
        inst->op = signed_op(inst->op);
    }
}

bool layout_resolve(struct layouts *l, struct ir_function *f, bool *resolved)
{
    bool has_bits = false;
    size_t b;
    size_t i;
    size_t k;

    resolve_type(l, &f->result, resolved);
    for (i = 0; i < f->param_count; i++) {
        resolve_type(l, &f->params[i].type, resolved);
    }
    for (i = 0; i < f->temp_count; i++) {
        resolve_type(l, &f->temps[i], resolved);
    }
    for (b = 0; b < f->block_count; b++) {
        for (i = 0; i < f->blocks[b]->count; i++) {
            struct ir_inst *inst = &f->blocks[b]->insts[i];
            wchar_signedness(l, inst);
            resolve_type(l, &inst->type, resolved);
            if (!resolve(l, &inst->a, resolved) ||
                !resolve(l, &inst->b, resolved) ||
                !resolve(l, &inst->c, resolved)) {
                return false;
            }
            for (k = 0; k < inst->arg_count; k++) {
                if (!resolve(l, &inst->args[k], resolved)) {
                    return false;
                }
            }
            /* A conversion between c_long and a type of its width on
               this target changes nothing. */
            if (is_conversion(inst->op) && inst->a.type == inst->type) {
                inst->op = IR_COPY;
            }
            if (inst->op == IR_BITLOAD || inst->op == IR_BITSTORE) {
                has_bits = true;
            }
        }
        if (has_bits) {
            lower_block_bits(l, f, f->blocks[b]);
            *resolved = true;
            has_bits = false;
        }
    }
    return true;
}

/* DESIGN: every target of antic is little endian, so a value of n bytes
   reaches memory lowest byte first. */
static void write_int(struct ir_global *g, uint64_t offset, uint64_t size,
                      uint64_t bits)
{
    uint64_t i;

    for (i = 0; i < size && offset + i < g->size; i++) {
        g->bytes[offset + i] = (uint8_t)(bits >> (i * 8));
    }
}

static uint64_t read_int(const struct ir_global *g, uint64_t offset,
                         uint64_t size)
{
    uint64_t bits = 0;
    uint64_t i;

    for (i = 0; i < size && offset + i < g->size; i++) {
        bits |= (uint64_t)g->bytes[offset + i] << (i * 8);
    }
    return bits;
}

/* The type of a constant, which gives its size and its alignment. */
static struct ir_vtype const_vtype(const struct ir_const *c)
{
    return c->kind == IR_CONST_AGG ? c->type : ir_scalar(c->scalar);
}

static void write_const(struct layouts *l, struct ir_module *m,
                        struct ir_global *g, const struct ir_const *c,
                        uint64_t base);

/* Put the value of a bitfield in the bits it occupies, leaving the other
   fields of its unit as they are. */
static void write_bits(struct layouts *l, struct ir_global *g,
                       const struct ir_aggtype *t, const struct layout *out,
                       size_t i, uint64_t base, uint64_t value)
{
    const struct layout_bits *b = &out->bits[i];
    uint64_t size = layout_size(l, ir_scalar(b->unit_type));
    uint64_t width = t->fields[i].bits;
    uint64_t mask = width >= 64 ? UINT64_MAX : ((uint64_t)1 << width) - 1;
    uint64_t unit = read_int(g, base + b->unit_offset, size);

    write_int(g, base + b->unit_offset, size,
              unit | ((value & mask) << b->shift));
}

static void write_agg(struct layouts *l, struct ir_module *m,
                      struct ir_global *g, const struct ir_const *c,
                      uint64_t base)
{
    const struct ir_aggtype *t = l->m->aggs[c->type.agg];
    const struct layout *out = layout_agg(l, c->type.agg);
    uint64_t step;
    size_t i;

    if (t->kind == IR_AGG_ARRAY) {
        step = layout_size(l, t->fields[0].type);
        for (i = 0; i < c->item_count; i++) {
            write_const(l, m, g, &c->items[i], base + i * step);
        }
        return;
    }
    for (i = 0; i < c->item_count && i < t->field_count; i++) {
        if (is_unit_break(&t->fields[i])) {
            continue;
        }
        if (t->fields[i].bits != 0) {
            write_bits(l, g, t, out, i, base, c->items[i].integer);
            continue;
        }
        write_const(l, m, g, &c->items[i], base + out->offsets[i]);
    }
}

/* Write constant c into the bytes of g at base. */
static void write_const(struct layouts *l, struct ir_module *m,
                        struct ir_global *g, const struct ir_const *c,
                        uint64_t base)
{
    uint64_t size;
    uint64_t bits = 0;
    float single;

    /* A field of no value leaves the zeros that are already there. */
    if (c->kind == IR_CONST_NONE) {
        return;
    }
    size = layout_size(l, const_vtype(c));
    switch (c->kind) {
    case IR_CONST_INT:
        write_int(g, base, size, c->integer);
        break;
    case IR_CONST_FLOAT:
        if (size == 4) {
            single = (float)c->floating;
            memcpy(&bits, &single, sizeof single);
        } else {
            memcpy(&bits, &c->floating, sizeof c->floating);
        }
        write_int(g, base, size, bits);
        break;
    case IR_CONST_SYM:
        if (layout_fold(l, c->sym, &bits)) {
            write_int(g, base, size, bits);
        }
        break;
    case IR_CONST_ADDR:
        /* The bytes stay zero. The linker writes the address. */
        ir_global_reloc(m, g, base, c->global);
        break;
    default: /* IR_CONST_AGG */
        write_agg(l, m, g, c, base);
        break;
    }
}

/* DESIGN: the bytes start as zeros and each field writes its own, so the
   padding of a constant is zero. The data of a constant is then the same
   in every build, and two constants of one value hold equal bytes. */
bool layout_data(struct layouts *l, struct ir_module *m)
{
    size_t i;

    for (i = 0; i < m->global_count && !l->failed; i++) {
        struct ir_global *g = m->globals[i];
        if (g->value == NULL) {
            continue;
        }
        g->size = layout_size(l, const_vtype(g->value));
        g->align = layout_align(l, const_vtype(g->value));
        g->bytes = arena_alloc(m->arena, g->size == 0 ? 1 : g->size);
        write_const(l, m, g, g->value, 0);
    }
    return !l->failed;
}
