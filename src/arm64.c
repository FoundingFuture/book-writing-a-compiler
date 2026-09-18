#include "select.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "regalloc.h"

/* The ARM64 back end: registers, opcodes and the pattern table of
   instruction selection. Register n is xn or wn, and 31 is sp. The float
   registers vn, printed dn or sn, have the numbers 32 + n. */

enum {
    X0 = 0, X1, X2, X3, X4, X5, X6, X7, X8, X9, X10, X11, X12, X13, X14,
    X15, X16, X17, X18, X19, X20, X21, X22, X23, X24, X25, X26, X27, X28,
    X29, X30, SP, V0
};

#define V(n) ((uint8_t)(V0 + (n)))

enum a64_op {
    A64_MOV, A64_MOVZ, A64_MOVK, A64_ADD, A64_SUB, A64_MUL, A64_AND,
    A64_ORR, A64_EOR, A64_NEG, A64_MVN, A64_CMP, A64_CSET, A64_B, A64_BCOND,
    A64_CBZ, A64_CBNZ, A64_BL, A64_BLR, A64_LDRGOT, A64_RET, A64_LDR, A64_STR, A64_STP, A64_LDP,
    A64_CMN, A64_SXTB, A64_SXTH, A64_SXTW, A64_UXTB, A64_UXTH, A64_SDIV,
    A64_UDIV, A64_MSUB, A64_LSL, A64_ASR, A64_LSR, A64_ADRP, A64_FADD,
    A64_FSUB, A64_FMUL, A64_FDIV, A64_FNEG, A64_FMOV, A64_FCMP, A64_SCVTF,
    A64_UCVTF, A64_FCVTZS, A64_FCVTZU, A64_FCVT, A64_MOVW,
    A64_SEH_SAVE_FPLR_X, A64_SEH_STACKALLOC, A64_SEH_SAVE_REG,
    A64_SEH_SAVE_FREG, A64_SEH_SET_FP, A64_SEH_ADD_FP, A64_SEH_NOP,
    A64_SEH_ENDPROLOGUE, A64_SEH_STARTEPILOGUE, A64_SEH_ENDEPILOGUE
};

#define USE ROLE_USE
#define DEF ROLE_DEF

static const struct mach_opcode opcodes[] = {
    [A64_MOV] = {"mov", {DEF, USE}, FLAG_MOVE},
    [A64_MOVZ] = {"movz", {DEF}, 0},
    [A64_MOVK] = {"movk", {USE | DEF}, 0},
    [A64_ADD] = {"add", {DEF, USE, USE}, 0},
    [A64_SUB] = {"sub", {DEF, USE, USE}, 0},
    [A64_MUL] = {"mul", {DEF, USE, USE}, 0},
    [A64_AND] = {"and", {DEF, USE, USE}, 0},
    [A64_ORR] = {"orr", {DEF, USE, USE}, 0},
    [A64_EOR] = {"eor", {DEF, USE, USE}, 0},
    [A64_NEG] = {"neg", {DEF, USE}, 0},
    [A64_MVN] = {"mvn", {DEF, USE}, 0},
    [A64_CMP] = {"cmp", {USE, USE}, 0},
    [A64_CSET] = {"cset", {DEF}, 0},
    [A64_B] = {"b", {0}, FLAG_JUMP},
    [A64_BCOND] = {"b.", {0}, FLAG_BRANCH},
    [A64_CBZ] = {"cbz", {USE}, FLAG_BRANCH},
    [A64_CBNZ] = {"cbnz", {USE}, FLAG_BRANCH},
    [A64_BL] = {"bl", {0}, FLAG_CALL},
    [A64_BLR] = {"blr", {USE}, FLAG_CALL},
    /* ldr of the address in a GOT entry, after adrp of its page. */
    [A64_LDRGOT] = {"ldr", {DEF, USE}, 0},
    [A64_RET] = {"ret", {0}, FLAG_RET},
    /* Unwind directives of a Windows prologue and epilogue, lines of the
       assembly file without an instruction. */
    [A64_SEH_SAVE_FPLR_X] = {".seh_save_fplr_x", {0}, 0},
    [A64_SEH_STACKALLOC] = {".seh_stackalloc", {0}, 0},
    [A64_SEH_SAVE_REG] = {".seh_save_reg", {0}, 0},
    [A64_SEH_SAVE_FREG] = {".seh_save_freg", {0}, 0},
    [A64_SEH_SET_FP] = {".seh_set_fp", {0}, 0},
    [A64_SEH_ADD_FP] = {".seh_add_fp", {0}, 0},
    [A64_SEH_NOP] = {".seh_nop", {0}, 0},
    [A64_SEH_ENDPROLOGUE] = {".seh_endprologue", {0}, 0},
    [A64_SEH_STARTEPILOGUE] = {".seh_startepilogue", {0}, 0},
    [A64_SEH_ENDEPILOGUE] = {".seh_endepilogue", {0}, 0},
    [A64_LDR] = {"ldr", {DEF}, 0},
    [A64_STR] = {"str", {USE}, 0},
    [A64_STP] = {"stp", {USE, USE}, 0},
    [A64_LDP] = {"ldp", {DEF, DEF}, 0},
    [A64_CMN] = {"cmn", {USE, USE}, 0},
    [A64_SXTB] = {"sxtb", {DEF, USE}, 0},
    [A64_SXTH] = {"sxth", {DEF, USE}, 0},
    [A64_SXTW] = {"sxtw", {DEF, USE}, 0},
    [A64_UXTB] = {"uxtb", {DEF, USE}, 0},
    [A64_UXTH] = {"uxth", {DEF, USE}, 0},
    [A64_SDIV] = {"sdiv", {DEF, USE, USE}, 0},
    [A64_UDIV] = {"udiv", {DEF, USE, USE}, 0},
    [A64_MSUB] = {"msub", {DEF, USE, USE, USE}, 0},
    [A64_LSL] = {"lsl", {DEF, USE, USE}, 0},
    [A64_ASR] = {"asr", {DEF, USE, USE}, 0},
    [A64_LSR] = {"lsr", {DEF, USE, USE}, 0},
    [A64_ADRP] = {"adrp", {DEF}, 0},
    [A64_FADD] = {"fadd", {DEF, USE, USE}, 0},
    [A64_FSUB] = {"fsub", {DEF, USE, USE}, 0},
    [A64_FMUL] = {"fmul", {DEF, USE, USE}, 0},
    [A64_FDIV] = {"fdiv", {DEF, USE, USE}, 0},
    [A64_FNEG] = {"fneg", {DEF, USE}, 0},
    [A64_FMOV] = {"fmov", {DEF, USE}, FLAG_MOVE},
    [A64_FCMP] = {"fcmp", {USE, USE}, 0},
    [A64_SCVTF] = {"scvtf", {DEF, USE}, 0},
    [A64_UCVTF] = {"ucvtf", {DEF, USE}, 0},
    [A64_FCVTZS] = {"fcvtzs", {DEF, USE}, 0},
    [A64_FCVTZU] = {"fcvtzu", {DEF, USE}, 0},
    [A64_FCVT] = {"fcvt", {DEF, USE}, 0},
    /* A move of a w register clears the upper 32 bits, even into itself,
       so it is no move that register allocation may drop. */
    [A64_MOVW] = {"mov", {DEF, USE}, 0},
};

static const uint8_t int_args[] = {X0, X1, X2, X3, X4, X5, X6, X7};

/* The registers from..to as a set. 2 << 63 wraps to 0 in unsigned
   arithmetic, which makes the upper bound 63 work. */
#define REGS(from, to) \
    ((((uint64_t)2 << (to)) - 1) & ~(((uint64_t)1 << (from)) - 1))

/* DESIGN: allocation prefers x9 to x15, which no convention gives a
   role, then the argument registers and the callee-saved ones last. x16
   and x17 are the scratch registers of spilled values. x18 is never
   allocated, because Apple and Windows reserve it. x29 holds the frame
   record and x30 the return address. */
static const uint8_t allocatable[] = {
    X9, X10, X11, X12, X13, X14, X15, X0, X1, X2, X3, X4, X5, X6, X7, X8,
    X19, X20, X21, X22, X23, X24, X25, X26, X27, X28
};

static const uint8_t fp_args[] = {
    V(0), V(1), V(2), V(3), V(4), V(5), V(6), V(7)
};

/* DESIGN: v16 and v17 are the float scratch registers. Allocation prefers
   v18 to v31, then the argument registers v0 to v7, and v8 to v15 last,
   whose low 64 bits a callee preserves. */
static const uint8_t fp_allocatable[] = {
    V(18), V(19), V(20), V(21), V(22), V(23), V(24), V(25), V(26), V(27),
    V(28), V(29), V(30), V(31), V(0), V(1), V(2), V(3), V(4), V(5), V(6),
    V(7), V(8), V(9), V(10), V(11), V(12), V(13), V(14), V(15)
};

#define FP_CALLER_SAVED (REGS(V(0), V(7)) | REGS(V(16), V(31)))
#define FP_CALLEE_SAVED REGS(V(8), V(15))

/* x18 is a caller-saved register under AAPCS64 on Linux. Apple and
   Windows reserve it, so it is never in use there. bl writes x30. The
   three conventions share their float registers. */
#define ABI(caller, probe)                                                   \
    {                                                                        \
        .int_args = int_args, .int_arg_count = 8, .int_result = X0,          \
        .caller_saved = (caller) | REGS(X30, X30) | FP_CALLER_SAVED,         \
        .callee_saved = REGS(X19, X28) | FP_CALLEE_SAVED,                    \
        .allocatable = allocatable, .allocatable_count = sizeof allocatable, \
        .scratch = {X16, X17}, .shadow_space = 0, .probe_stack = (probe),    \
        .fp_args = fp_args, .fp_arg_count = 8, .fp_result = V(0),            \
        .fp_allocatable = fp_allocatable,                                    \
        .fp_allocatable_count = sizeof fp_allocatable,                       \
        .fp_scratch = {V(16), V(17)}, .fp_save_size = 8,                     \
    }

static const struct abi aapcs64 = ABI(REGS(X0, X18), false);
static const struct abi apple = ABI(REGS(X0, X17), false);
static const struct abi windows = ABI(REGS(X0, X17), true);

static const struct abi *abi(enum convention convention)
{
    switch (convention) {
    case CONVENTION_AAPCS64: return &aapcs64;
    case CONVENTION_WINDOWS_ARM64: return &windows;
    default: return &apple;
    }
}

/* Values of 8, 16 and 32 bits live in the 32-bit w registers. The bits
   above the width of a value are unknown. */
static uint8_t width(enum ir_type type)
{
    return type == IR_I64 || type == IR_PTR || type == IR_F64 ? 64 : 32;
}

static uint8_t bits(enum ir_type type)
{
    switch (type) {
    case IR_I8: return 8;
    case IR_I16: return 16;
    case IR_I32:
    case IR_F32: return 32;
    default: return 64;
    }
}

static int64_t signed_value(uint64_t v, uint8_t w)
{
    return w == 8    ? (int8_t)(uint8_t)v
           : w == 16 ? (int16_t)(uint16_t)v
           : w == 32 ? (int32_t)(uint32_t)v
                     : (int64_t)v;
}

/* The instruction that extends an 8-bit or 16-bit value to 32 bits. */
static enum a64_op extension(uint8_t n, bool is_signed)
{
    if (n == 8) {
        return is_signed ? A64_SXTB : A64_UXTB;
    }
    return is_signed ? A64_SXTH : A64_UXTH;
}

static void emit2(struct selector *s, enum a64_op op, struct mach_operand a,
                  struct mach_operand b)
{
    struct mach_operand ops[2];
    ops[0] = a;
    ops[1] = b;
    select_emit(s, (uint16_t)op, 2, ops);
}

static void emit3(struct selector *s, enum a64_op op, struct mach_operand a,
                  struct mach_operand b, struct mach_operand c)
{
    struct mach_operand ops[3];
    ops[0] = a;
    ops[1] = b;
    ops[2] = c;
    select_emit(s, (uint16_t)op, 3, ops);
}

/* A move between two registers, with fmov for floats. */
static void move(struct selector *s, struct mach_operand dst,
                 struct mach_operand src)
{
    bool fp = dst.kind == MACH_VREG ? s->out->fp[dst.reg] : dst.reg >= V0;

    emit2(s, fp ? A64_FMOV : A64_MOV, dst, src);
}

static void append(struct mach_block *b, enum a64_op op, size_t count,
                   const struct mach_operand *operands)
{
    struct mach_inst *inst = mach_append(b);

    inst->op = (uint16_t)op;
    inst->count = (uint8_t)count;
    memcpy(inst->operands, operands, count * sizeof *operands);
}

/* An instruction holds a 16-bit immediate. mov takes a value that fits,
   or whose bitwise inverse fits. Other values go in as 16-bit pieces with
   movz and movk. */
static void load_into(struct mach_block *b, struct mach_operand dst,
                      uint64_t value)
{
    uint64_t mask = dst.width == 64 ? UINT64_MAX : UINT32_MAX;
    uint64_t v = value & mask;
    int64_t signed_v = dst.width == 64 ? (int64_t)v : (int32_t)(uint32_t)v;
    struct mach_operand ops[3];
    bool first = true;
    int shift;

    ops[0] = dst;
    if (v <= 0xffff || (~v & mask) <= 0xffff) {
        ops[1] = mach_imm(v <= 0xffff ? (int64_t)v : signed_v);
        append(b, A64_MOV, 2, ops);
        return;
    }
    for (shift = 0; shift < dst.width; shift += 16) {
        uint64_t piece = (v >> shift) & 0xffff;
        if (piece == 0) {
            continue;
        }
        ops[1] = mach_imm((int64_t)piece);
        ops[2] = mach_imm(shift);
        append(b, first ? A64_MOVZ : A64_MOVK, shift == 0 ? 2 : 3, ops);
        first = false;
    }
}

static void load(struct selector *s, struct mach_operand dst, uint64_t value)
{
    load_into(s->b, dst, value);
}

/* Patterns */

static bool is_arith_type(enum ir_type type)
{
    return type == IR_I8 || type == IR_I16 || type == IR_I32 ||
           type == IR_I64 || type == IR_PTR;
}

static bool match_arith(const struct selector *s, const struct ir_inst *inst)
{
    (void)s;
    return is_arith_type(inst->type);
}

static bool match_copy(const struct selector *s, const struct ir_inst *inst)
{
    (void)s;
    return is_arith_type(inst->type) || inst->type == IR_I8;
}

/* x = xor x, 1 on a bool, the lowering of logical not. */
static bool match_not_bool(const struct selector *s,
                           const struct ir_inst *inst)
{
    (void)s;
    return inst->type == IR_I8 && inst->b.kind == IR_INT &&
           inst->b.as.integer == 1;
}

static bool match_compare(const struct selector *s,
                          const struct ir_inst *inst)
{
    (void)s;
    return is_arith_type(inst->a.type);
}

static bool match_fused(const struct selector *s, const struct ir_inst *inst)
{
    (void)inst;
    return s->fused != NULL;
}

static bool match_call(const struct selector *s, const struct ir_inst *inst)
{
    return (inst->a.kind == IR_FUNC || inst->b.kind == IR_FUNC) &&
           (inst->type == IR_VOID || match_copy(s, inst) ||
            select_is_float(inst->type) || inst->type == IR_AGG);
}

/* add, sub, cmp and cmn take a 12-bit immediate, optionally shifted left
   by 12 bits. */
static bool fits_imm12(int64_t v)
{
    return v >= 0 && (v <= 4095 || ((v & 0xfff) == 0 && v >> 12 <= 4095));
}

/* Append op with the immediate v after its count operands, in the form
   #v >> 12, lsl #12 when v is larger than 4095. */
static void append_imm12(struct mach_block *b, enum a64_op op, size_t count,
                         struct mach_operand *ops, int64_t v)
{
    if (v > 4095) {
        ops[count] = mach_imm(v >> 12);
        ops[count + 1] = mach_imm(12);
        append(b, op, count + 2, ops);
    } else {
        ops[count] = mach_imm(v);
        append(b, op, count + 1, ops);
    }
}

static void emit_imm12(struct selector *s, enum a64_op op, size_t count,
                       struct mach_operand *ops, int64_t v)
{
    append_imm12(s->b, op, count, ops, v);
}

static unsigned popcount(uint64_t v)
{
    unsigned n = 0;

    for (; v != 0; v &= v - 1) {
        n++;
    }
    return n;
}

/* and, orr and eor take a logical immediate: an element of 2, 4, 8, 16,
   32 or 64 bits that repeats across the register. The element is one
   rotated run of ones, so it is neither all zeros nor all ones. */
static bool is_logical_imm(uint64_t v, uint8_t w)
{
    uint64_t mask = w == 64 ? UINT64_MAX : UINT32_MAX;
    unsigned e;

    v &= mask;
    if (v == 0 || v == mask) {
        return false;
    }
    for (e = 2; e <= w; e *= 2) {
        uint64_t emask = e == 64 ? UINT64_MAX : ((uint64_t)1 << e) - 1;
        uint64_t element = v & emask;
        unsigned i;
        bool repeats = true;
        for (i = e; i < w; i += e) {
            repeats = repeats && ((v >> i) & emask) == element;
        }
        if (repeats) {
            uint64_t rotated = ((element << 1) | (element >> (e - 1))) & emask;
            return popcount(element ^ rotated) == 2;
        }
    }
    return false;
}

static void emit_copy(struct selector *s, const struct ir_inst *inst)
{
    struct mach_operand r = select_result(s, inst);

    if (inst->a.kind == IR_INT) {
        load(s, r, inst->a.as.integer);
    } else {
        move(s, r, select_reg(s, &inst->a));
    }
}

/* A negative immediate turns add into sub and sub into add. */
static void emit_add_sub(struct selector *s, const struct ir_inst *inst)
{
    struct mach_operand ops[4];
    enum a64_op op = inst->op == IR_SUB ? A64_SUB : A64_ADD;
    enum a64_op other = inst->op == IR_SUB ? A64_ADD : A64_SUB;
    int64_t v = inst->b.kind == IR_INT
                    ? signed_value(inst->b.as.integer, bits(inst->b.type))
                    : 0;

    ops[0] = select_result(s, inst);
    ops[1] = select_reg(s, &inst->a);
    if (inst->b.kind == IR_INT && fits_imm12(v)) {
        emit_imm12(s, op, 2, ops, v);
    } else if (inst->b.kind == IR_INT && v < 0 && fits_imm12(-v)) {
        emit_imm12(s, other, 2, ops, -v);
    } else {
        emit3(s, op, ops[0], ops[1], select_reg(s, &inst->b));
    }
}

static void emit_binary(struct selector *s, const struct ir_inst *inst)
{
    enum a64_op op = inst->op == IR_MUL   ? A64_MUL
                     : inst->op == IR_AND ? A64_AND
                     : inst->op == IR_OR  ? A64_ORR
                                          : A64_EOR;
    struct mach_operand r = select_result(s, inst);
    struct mach_operand a = select_reg(s, &inst->a);

    if (op != A64_MUL && inst->b.kind == IR_INT &&
        is_logical_imm(inst->b.as.integer, r.width)) {
        emit3(s, op, r, a,
              mach_imm(signed_value(inst->b.as.integer, r.width)));
        return;
    }
    emit3(s, op, r, a, select_reg(s, &inst->b));
}

static struct mach_operand widened(struct mach_operand o, uint8_t w)
{
    if (o.kind == MACH_VREG || o.kind == MACH_PREG) {
        o.width = w;
    }
    return o;
}

/* The register that holds operand o extended to at least 32 bits. */
static struct mach_operand extended(struct selector *s,
                                    const struct ir_operand *o,
                                    bool is_signed)
{
    uint8_t n = bits(o->type);
    struct mach_operand r;

    if (n >= 32) {
        return select_reg(s, o);
    }
    r = select_new_vreg(s, 32);
    if (o->kind == IR_INT) {
        load(s, r, is_signed ? (uint64_t)signed_value(o->as.integer, n)
                             : o->as.integer);
    } else {
        emit2(s, extension(n, is_signed), r, select_reg(s, o));
    }
    return r;
}

/* sdiv and udiv divide registers of 32 or 64 bits, so 8-bit and 16-bit
   operands extend first. msub computes the remainder a - (a / b) * b. */
static void emit_div(struct selector *s, const struct ir_inst *inst)
{
    bool is_signed = inst->op == IR_SDIV || inst->op == IR_SREM;
    enum a64_op op = is_signed ? A64_SDIV : A64_UDIV;
    struct mach_operand r = select_result(s, inst);
    struct mach_operand a = extended(s, &inst->a, is_signed);
    struct mach_operand b = extended(s, &inst->b, is_signed);
    struct mach_operand ops[4];

    if (inst->op == IR_SDIV || inst->op == IR_UDIV) {
        emit3(s, op, r, a, b);
        return;
    }
    ops[0] = r;
    ops[1] = select_new_vreg(s, r.width);
    ops[2] = b;
    ops[3] = a;
    emit3(s, op, ops[1], a, b);
    select_emit(s, A64_MSUB, 4, ops);
}

/* lsl, asr and lsr shift by a register, modulo the width of the register,
   or by a constant below that width. A right shift of an 8-bit or 16-bit
   value extends it to 32 bits first. */
static void emit_shift(struct selector *s, const struct ir_inst *inst)
{
    enum a64_op op = inst->op == IR_SHL     ? A64_LSL
                     : inst->op == IR_SHR_S ? A64_ASR
                                            : A64_LSR;
    struct mach_operand r = select_result(s, inst);
    struct mach_operand a = inst->op == IR_SHL
                                ? select_reg(s, &inst->a)
                                : extended(s, &inst->a, op == A64_ASR);

    if (inst->b.kind == IR_INT && inst->b.as.integer < r.width) {
        emit3(s, op, r, a, mach_imm((int64_t)inst->b.as.integer));
    } else {
        emit3(s, op, r, a, select_reg(s, &inst->b));
    }
}

/* Memory of width bits at offset after the address in register base. */
static struct mach_operand memory_at(struct mach_operand base, int64_t offset,
                                     uint8_t width_bits)
{
    struct mach_operand m = base;

    m.kind = MACH_MEM;
    m.base_vreg = base.kind == MACH_VREG;
    m.width = width_bits;
    m.value = offset;
    return m;
}

/* A call of a C library function by name with integer arguments. */
static void call_c(struct selector *s, const char *name,
                   const struct mach_operand *args, size_t count)
{
    struct mach_operand f;
    struct mach_inst *call;
    uint64_t uses = 0;
    size_t i;

    for (i = 0; i < count; i++) {
        move(s, mach_preg(s->abi->int_args[i], 64), args[i]);
        uses |= (uint64_t)1 << s->abi->int_args[i];
    }
    memset(&f, 0, sizeof f);
    f.kind = MACH_NAME;
    f.name = name;
    call = select_emit(s, A64_BL, 1, &f);
    call->uses = uses;
    call->defs = s->abi->caller_saved;
}

/* DESIGN: a copy of up to 64 bytes moves 8, 4, 2 and 1 bytes at a time
   through an integer register. A larger copy calls memcpy. */
static void copy_memory(struct selector *s, struct mach_operand dst,
                        struct mach_operand src, uint64_t size)
{
    uint64_t offset = 0;

    if (size > 64) {
        struct mach_operand args[3];
        args[0] = dst;
        args[1] = src;
        args[2] = select_new_vreg(s, 64);
        load(s, args[2], size);
        call_c(s, "memcpy", args, 3);
        return;
    }
    while (offset < size) {
        uint64_t left = size - offset;
        uint8_t n = left >= 8 ? 64 : left >= 4 ? 32 : left >= 2 ? 16 : 8;
        struct mach_operand t = select_new_vreg(s, n == 64 ? 64 : 32);
        emit2(s, A64_LDR, t, memory_at(src, (int64_t)offset, n));
        emit2(s, A64_STR, t, memory_at(dst, (int64_t)offset, n));
        offset += n / 8;
    }
}

static bool is_float_register(const struct selector *s,
                              struct mach_operand o)
{
    return o.kind == MACH_VREG ? s->out->fp[o.reg] : o.reg >= V0;
}

/* Load bytes of an aggregate at offset after base into register dst. An
   integer part of 3, 5, 6 or 7 bytes combines its pieces with shifts. */
static void load_bytes(struct selector *s, struct mach_operand dst,
                       struct mach_operand base, int64_t offset,
                       unsigned bytes)
{
    unsigned low = bytes >= 4 ? 4 : 2;
    struct mach_operand rest;

    if (is_float_register(s, dst) || bytes == 8) {
        emit2(s, A64_LDR, widened(dst, (uint8_t)(bytes * 8)),
              memory_at(base, offset, (uint8_t)(bytes * 8)));
    } else if (bytes == 4 || bytes == 2 || bytes == 1) {
        emit2(s, A64_LDR, widened(dst, 32),
              memory_at(base, offset, (uint8_t)(bytes * 8)));
    } else {
        load_bytes(s, dst, base, offset, low);
        rest = select_new_vreg(s, 64);
        load_bytes(s, rest, base, offset + low, bytes - low);
        emit3(s, A64_LSL, rest, rest, mach_imm(8 * low));
        emit3(s, A64_ORR, widened(dst, 64), widened(dst, 64), rest);
    }
}

/* Store the low bytes of register src at offset after base. */
static void store_bytes(struct selector *s, struct mach_operand src,
                        struct mach_operand base, int64_t offset,
                        unsigned bytes)
{
    unsigned low = bytes >= 4 ? 4 : 2;
    struct mach_operand rest;

    if (is_float_register(s, src) || bytes == 8) {
        emit2(s, A64_STR, widened(src, (uint8_t)(bytes * 8)),
              memory_at(base, offset, (uint8_t)(bytes * 8)));
    } else if (bytes == 4 || bytes == 2 || bytes == 1) {
        emit2(s, A64_STR, widened(src, 32),
              memory_at(base, offset, (uint8_t)(bytes * 8)));
    } else {
        store_bytes(s, src, base, offset, low);
        rest = select_new_vreg(s, 64);
        emit3(s, A64_LSR, rest, widened(src, 64), mach_imm(8 * low));
        store_bytes(s, rest, base, offset + low, bytes - low);
    }
}

static void slot_address(struct selector *s, struct mach_operand dst,
                         uint32_t slot)
{
    struct mach_operand o = mach_imm(slot);

    o.kind = MACH_SLOT;
    emit3(s, A64_ADD, widened(dst, 64), mach_preg(SP, 64), o);
}

/* The address of an aggregate in the argument area of the caller, 16
   bytes above the frame record. */
static void incoming_address(struct selector *s, struct mach_operand dst,
                             int64_t offset)
{
    struct mach_operand ops[4];

    s->out->stack_params = true;
    ops[0] = widened(dst, 64);
    ops[1] = mach_preg(X29, 64);
    emit_imm12(s, A64_ADD, 2, ops, 16 + offset);
}

/* An aggregate argument on the stack is copied into the argument area. An
   indirect one is copied into a slot of the caller, and its address goes
   to the stack or returns for a register. */
static struct mach_operand copy_argument(struct selector *s,
                                         const struct arg_location *loc,
                                         const struct layout *agg,
                                         struct mach_operand value)
{
    struct mach_operand address = select_new_vreg(s, 64);
    struct mach_operand ops[4];

    if (loc->indirect) {
        slot_address(s, address, mach_slot_add(s->out, agg->size, 16));
        copy_memory(s, address, value, agg->size);
        if (loc->stack) {
            emit2(s, A64_STR, address,
                  memory_at(mach_preg(SP, 64), loc->offset, 64));
        }
        return address;
    }
    ops[0] = address;
    ops[1] = mach_preg(SP, 64);
    emit_imm12(s, A64_ADD, 2, ops, loc->offset);
    copy_memory(s, address, value, agg->size);
    return address;
}

static void emit_memcopy(struct selector *s, const struct ir_inst *inst)
{
    copy_memory(s, select_reg(s, &inst->a), select_reg(s, &inst->b),
                select_size(s, inst->of));
}

/* Floats */

static struct mach_operand cond(enum mach_cond c);

static bool match_float(const struct selector *s, const struct ir_inst *inst)
{
    (void)s;
    return select_is_float(inst->type);
}

/* A float constant goes in as its bits through an integer register and
   fmov. */
static void load_float(struct selector *s, struct mach_operand dst,
                       enum ir_type type, double value)
{
    struct mach_operand bits_reg;
    uint64_t pattern;

    if (type == IR_F32) {
        float narrow = (float)value;
        uint32_t word;
        memcpy(&word, &narrow, sizeof word);
        pattern = word;
    } else {
        memcpy(&pattern, &value, sizeof pattern);
    }
    bits_reg = select_new_vreg(s, width(type));
    load(s, bits_reg, pattern);
    emit2(s, A64_FMOV, dst, bits_reg);
}

static void emit_float_copy(struct selector *s, const struct ir_inst *inst)
{
    emit2(s, A64_FMOV, select_result(s, inst), select_reg(s, &inst->a));
}

static void emit_float_binary(struct selector *s, const struct ir_inst *inst)
{
    enum a64_op op = inst->op == IR_FADD   ? A64_FADD
                     : inst->op == IR_FSUB ? A64_FSUB
                     : inst->op == IR_FMUL ? A64_FMUL
                                           : A64_FDIV;

    emit3(s, op, select_result(s, inst), select_reg(s, &inst->a),
          select_reg(s, &inst->b));
}

/* fcmp sets N for less, Z and C for equal, C for greater and C and V for
   NaN. mi, ls, gt and ge are false for NaN, and ne is true. */
static void emit_float_compare(struct selector *s, const struct ir_inst *inst)
{
    enum mach_cond c = inst->op == IR_FEQ   ? COND_EQ
                       : inst->op == IR_FNE ? COND_NE
                       : inst->op == IR_FLT ? COND_MI
                       : inst->op == IR_FLE ? COND_LS
                       : inst->op == IR_FGT ? COND_GT
                                            : COND_GE;

    emit2(s, A64_FCMP, select_reg(s, &inst->a), select_reg(s, &inst->b));
    emit2(s, A64_CSET, select_result(s, inst), cond(c));
}

/* scvtf and ucvtf convert a w or x register, fcvtzs and fcvtzu truncate
   toward zero, and fcvt changes the float width. An integer of 8 or 16
   bits extends to 32 bits first. */
static void emit_float_convert(struct selector *s, const struct ir_inst *inst)
{
    struct mach_operand r = select_result(s, inst);
    bool is_signed = inst->op == IR_SITOF || inst->op == IR_FTOSI;

    switch (inst->op) {
    case IR_SITOF:
    case IR_UITOF:
        emit2(s, is_signed ? A64_SCVTF : A64_UCVTF, r,
              extended(s, &inst->a, is_signed));
        break;
    case IR_FTOSI:
    case IR_FTOUI:
        emit2(s, is_signed ? A64_FCVTZS : A64_FCVTZU, r,
              select_reg(s, &inst->a));
        break;
    default:
        emit2(s, A64_FCVT, r, select_reg(s, &inst->a));
        break;
    }
}

static void emit_float_neg(struct selector *s, const struct ir_inst *inst)
{
    emit2(s, A64_FNEG, select_result(s, inst), select_reg(s, &inst->a));
}

/* The address of a function or a global. adrp writes the address of the
   4 KB page that holds the symbol, relative to the pc, and add adds the
   low 12 bits of the symbol. A C function on Linux and macOS loads its
   address from the GOT entry in that page instead. */
static void emit_addr(struct selector *s, const struct ir_inst *inst)
{
    struct mach_operand r = select_result(s, inst);
    struct mach_operand symbol = mach_imm(inst->a.as.index);

    symbol.kind = inst->a.kind == IR_FUNC ? MACH_FUNC : MACH_GLOBAL;
    symbol.got = select_uses_got(s, &inst->a);
    emit2(s, A64_ADRP, r, symbol);
    emit3(s, symbol.got ? A64_LDRGOT : A64_ADD, r, r, symbol);
}

/* trunc keeps the low bits with a move of the w register. sext uses sxtb,
   sxth or sxtw. zext uses uxtb, uxth or, from 32 bits, a move of the w
   register, because every write to a w register clears the upper 32
   bits. */
static void emit_convert(struct selector *s, const struct ir_inst *inst)
{
    struct mach_operand r = select_result(s, inst);
    uint8_t from = bits(inst->a.type);
    static const enum a64_op sext[] = {A64_SXTB, A64_SXTH, A64_SXTW};

    if (inst->a.kind == IR_INT) {
        load(s, r, inst->op == IR_SEXT
                       ? (uint64_t)signed_value(inst->a.as.integer, from)
                       : inst->a.as.integer);
    } else if (inst->op == IR_TRUNC) {
        move(s, widened(r, 32), widened(select_reg(s, &inst->a), 32));
    } else if (inst->op == IR_ZEXT && from == 32) {
        emit2(s, A64_MOVW, widened(r, 32),
              widened(select_reg(s, &inst->a), 32));
    } else if (inst->op == IR_SEXT) {
        emit2(s, sext[from == 8 ? 0 : from == 16 ? 1 : 2], r,
              widened(select_reg(s, &inst->a), 32));
    } else {
        emit2(s, from == 8 ? A64_UXTB : A64_UXTH, widened(r, 32),
              widened(select_reg(s, &inst->a), 32));
    }
}

static bool match_convert(const struct selector *s, const struct ir_inst *inst)
{
    (void)s;
    return is_arith_type(inst->type) && is_arith_type(inst->a.type);
}

static void emit_not_bool(struct selector *s, const struct ir_inst *inst)
{
    struct mach_operand r = select_result(s, inst);

    emit3(s, A64_EOR, r, select_reg(s, &inst->a), mach_imm(1));
}

static void emit_unary(struct selector *s, const struct ir_inst *inst)
{
    struct mach_operand r = select_result(s, inst);

    emit2(s, inst->op == IR_NEG ? A64_NEG : A64_MVN, r,
          select_reg(s, &inst->a));
}

/* cmp compares 32 or 64 bits. An 8-bit or 16-bit first operand extends
   into a new register, and a register second operand extends inside cmp.
   A negative constant compares with cmn, which adds its negation. */
static void compare(struct selector *s, const struct ir_inst *inst)
{
    uint8_t n = bits(inst->a.type);
    bool is_signed = inst->op >= IR_SLT && inst->op <= IR_SGE;
    struct mach_operand a = select_reg(s, &inst->a);
    struct mach_operand ops[3];
    int64_t v;

    if (n < 32) {
        struct mach_operand wide = select_new_vreg(s, 32);
        emit2(s, extension(n, is_signed), wide, a);
        a = wide;
    }
    if (inst->b.kind == IR_INT) {
        v = n < 32 && !is_signed ? (int64_t)inst->b.as.integer
                                 : signed_value(inst->b.as.integer, n);
        ops[0] = a;
        if (fits_imm12(v)) {
            emit_imm12(s, A64_CMP, 1, ops, v);
        } else if (v < 0 && fits_imm12(-v)) {
            emit_imm12(s, A64_CMN, 1, ops, -v);
        } else {
            struct mach_operand b = select_new_vreg(s, a.width);
            load(s, b, (uint64_t)v);
            emit2(s, A64_CMP, a, b);
        }
        return;
    }
    if (n < 32) {
        ops[0] = a;
        ops[1] = select_reg(s, &inst->b);
        ops[2] = mach_imm(extension(n, is_signed));
        select_emit(s, A64_CMP, 3, ops);
        return;
    }
    emit2(s, A64_CMP, a, select_reg(s, &inst->b));
}

static struct mach_operand cond(enum mach_cond c)
{
    struct mach_operand o = mach_imm(c);

    o.kind = MACH_COND;
    return o;
}

static struct mach_operand block(const struct ir_operand *o)
{
    struct mach_operand b = mach_imm(o->as.index);

    b.kind = MACH_BLOCK;
    return b;
}

static void emit_set(struct selector *s, const struct ir_inst *inst)
{
    compare(s, inst);
    emit2(s, A64_CSET, select_result(s, inst), cond(select_cond(inst->op)));
}

static void jump(struct selector *s, const struct ir_operand *target)
{
    struct mach_operand b = block(target);

    if (!select_is_next(s, target)) {
        select_emit(s, A64_B, 1, &b);
    }
}

static void emit_jump(struct selector *s, const struct ir_inst *inst)
{
    jump(s, &inst->a);
}

/* When the true block follows, the negated condition jumps to the false
   block, and control falls through otherwise. */
static void emit_fused_branch(struct selector *s, const struct ir_inst *inst)
{
    enum mach_cond c = select_cond(s->fused->op);

    compare(s, s->fused);
    if (select_is_next(s, &inst->b)) {
        emit2(s, A64_BCOND, cond(select_negate(c)), block(&inst->c));
        return;
    }
    emit2(s, A64_BCOND, cond(c), block(&inst->b));
    jump(s, &inst->c);
}

static void emit_branch(struct selector *s, const struct ir_inst *inst)
{
    struct mach_operand c = select_reg(s, &inst->a);

    if (select_is_next(s, &inst->b)) {
        emit2(s, A64_CBZ, c, block(&inst->c));
        return;
    }
    emit2(s, A64_CBNZ, c, block(&inst->b));
    jump(s, &inst->c);
}

static struct mach_operand memory(struct mach_operand base,
                                  enum ir_type type);

/* A homogeneous floating-point aggregate: one to four members, all f32 or
   all f64. Returns the number of members, or 0. */
static size_t hfa_members(const struct layout *agg)
{
    size_t i;

    if (agg->member_count == 0 || agg->member_count > 4 ||
        !select_is_float(agg->members[0].type)) {
        return 0;
    }
    for (i = 1; i < agg->member_count; i++) {
        if (agg->members[i].type != agg->members[0].type) {
            return 0;
        }
    }
    return agg->member_count;
}

/* The register parts of an aggregate, starting at register first. A
   homogeneous float aggregate takes one register per member. Another
   aggregate of at most 16 bytes takes 8-byte parts. */
static void aggregate_parts(const struct layout *agg, uint8_t first,
                            struct arg_location *out)
{
    size_t n = hfa_members(agg);
    size_t k;

    if (n > 0) {
        for (k = 0; k < n; k++) {
            out->parts[k].reg = (uint8_t)(first + k);
            out->parts[k].bytes = agg->members[0].type == IR_F32 ? 4 : 8;
            out->parts[k].offset = (uint8_t)agg->members[k].offset;
        }
        out->part_count = n;
        return;
    }
    n = (size_t)(agg->size + 7) / 8;
    for (k = 0; k < n; k++) {
        out->parts[k].reg = (uint8_t)(first + k);
        out->parts[k].bytes =
            (uint8_t)(agg->size - 8 * k < 8 ? agg->size - 8 * k : 8);
        out->parts[k].offset = (uint8_t)(8 * k);
    }
    out->part_count = n;
}

static void locate_result(const struct selector *s,
                          const struct ir_function *f,
                          struct arg_location *out)
{
    const struct layout *agg = select_layout(s, f->result_agg);

    memset(out, 0, sizeof *out);
    out->copy = -1;
    out->reg = select_is_float(f->result) ? s->abi->fp_result
                                          : s->abi->int_result;
    if (f->result != IR_AGG) {
        return;
    }
    if (hfa_members(agg) > 0) {
        aggregate_parts(agg, V(0), out);
    } else if (agg->size <= 16) {
        aggregate_parts(agg, X0, out);
    } else {
        out->indirect = true;
        out->reg = X8;
    }
}

/* DESIGN: AAPCS64 counts integer and float arguments separately, and an
   argument after its eight registers goes to the stack in 8 bytes. Apple
   gives a named stack argument its own size at its own alignment and
   passes every variadic argument on the stack in 8 bytes. Windows passes
   a variadic float in an integer register. A homogeneous float aggregate
   takes float registers and any other aggregate of up to 16 bytes integer
   registers, all or none. A larger aggregate is a pointer to a copy. On
   the stack, Apple aligns a float aggregate to its members, and every
   other aggregate takes 8-byte slots. */
static void locate(const struct selector *s, const struct ir_function *callee,
                   const enum ir_type *types, size_t count,
                   struct arg_location *out)
{
    size_t ints = 0;
    size_t floats = 0;
    int64_t next = 0;
    int64_t align;
    size_t i;

    for (i = 0; i < count; i++) {
        bool variadic = i >= callee->param_count;
        bool fp = select_is_float(types[i]) &&
                  !(s->abi == &windows && variadic);
        int64_t size = s->abi == &apple && !variadic ? bits(types[i]) / 8 : 8;
        memset(&out[i], 0, sizeof out[i]);
        out[i].copy = -1;
        if (types[i] == IR_AGG) {
            const struct layout *agg = select_layout(s, callee->params[i].agg);
            size_t hfa = hfa_members(agg);
            size_t parts = (size_t)(agg->size + 7) / 8;
            size = 8;
            if (hfa > 0 && floats + hfa <= s->abi->fp_arg_count) {
                aggregate_parts(agg, s->abi->fp_args[floats], &out[i]);
                floats += hfa;
                continue;
            }
            /* An aggregate aligned to 16 starts at an even register,
               except on Apple. */
            if (hfa == 0 && agg->size <= 16 && agg->align == 16 &&
                s->abi != &apple && ints % 2 == 1 &&
                ints + 1 + parts <= s->abi->int_arg_count) {
                ints++;
            }
            if (hfa == 0 && agg->size <= 16 &&
                ints + parts <= s->abi->int_arg_count) {
                aggregate_parts(agg, s->abi->int_args[ints], &out[i]);
                ints += parts;
                continue;
            }
            if (hfa == 0 && agg->size > 16 && ints < s->abi->int_arg_count) {
                out[i].indirect = true;
                out[i].reg = s->abi->int_args[ints++];
                continue;
            }
            if (hfa > 0) {
                floats = s->abi->fp_arg_count;
            } else if (agg->size <= 16) {
                ints = s->abi->int_arg_count;
            }
            out[i].stack = true;
            out[i].indirect = agg->size > 16 && hfa == 0;
            align = 8;
            if (!out[i].indirect) {
                out[i].size = agg->size;
                size = (int64_t)(agg->size + 7) / 8 * 8;
            }
            if (hfa > 0 && s->abi == &apple) {
                align = (int64_t)agg->align;
                size = (int64_t)agg->size;
            } else if (agg->align == 16 && !out[i].indirect) {
                align = 16;
            }
            out[i].offset = (next + align - 1) / align * align;
            next = out[i].offset + size;
            continue;
        }
        if (s->abi == &apple && variadic) {
            out[i].stack = true;
        } else if (fp && floats < s->abi->fp_arg_count) {
            out[i].reg = s->abi->fp_args[floats++];
        } else if (!fp && ints < s->abi->int_arg_count) {
            out[i].reg = s->abi->int_args[ints++];
        } else {
            out[i].stack = true;
        }
        if (out[i].stack) {
            out[i].offset = (next + size - 1) / size * size;
            next = out[i].offset + size;
        }
    }
}

/* A value into the register of its location. A float for an integer
   register moves its bits with fmov. */
static void load_value(struct selector *s, struct mach_operand reg,
                       const struct ir_operand *value, enum ir_ext ext)
{
    bool fp_reg = reg.reg >= V0;

    if (value->kind == IR_INT) {
        load(s, reg,
             ext == IR_EXT_SIGN
                 ? (uint64_t)signed_value(value->as.integer, bits(value->type))
                 : value->as.integer);
    } else if (select_is_float(value->type)) {
        struct mach_operand v = select_reg(s, value);
        emit2(s, A64_FMOV, fp_reg ? reg : widened(reg, v.width), v);
    } else if (ext != IR_EXT_NONE) {
        emit2(s, extension(bits(value->type), ext == IR_EXT_SIGN), reg,
              select_reg(s, value));
    } else {
        move(s, reg, select_reg(s, value));
    }
}

/* Stack arguments go to the area at sp, then the register arguments into
   their registers. Apple makes the caller extend a register argument of 8
   or 16 bits. bl overwrites every caller-saved register. */
static void emit_call(struct selector *s, const struct ir_inst *inst)
{
    const struct ir_function *callee = select_callee(s, inst);
    bool indirect = inst->b.kind == IR_FUNC;
    struct mach_operand target = indirect ? select_reg(s, &inst->a)
                                          : mach_imm(0);
    enum ir_type *types = calloc(inst->arg_count + 1, sizeof *types);
    struct arg_location *locations =
        calloc(inst->arg_count + 1, sizeof *locations);
    struct mach_operand *copies = calloc(inst->arg_count + 1, sizeof *copies);
    struct arg_location result;
    struct mach_operand result_address;
    struct mach_operand f = mach_imm(inst->a.as.index);
    struct mach_inst *call;
    uint64_t uses = 0;
    int64_t next = 0;
    size_t i;

    memset(&result_address, 0, sizeof result_address);
    if (types == NULL || locations == NULL || copies == NULL) {
        fputs("antic: out of memory\n", stderr);
        exit(70);
    }
    for (i = 0; i < inst->arg_count; i++) {
        types[i] = i < callee->param_count ? callee->params[i].type
                                           : inst->args[i].type;
    }
    locate(s, callee, types, inst->arg_count, locations);
    locate_result(s, callee, &result);
    if (callee->result == IR_AGG && result.indirect) {
        result_address = select_result_slot(s, inst, select_layout(s, callee->result_agg));
    }
    for (i = 0; i < inst->arg_count; i++) {
        const struct ir_operand *arg = &inst->args[i];
        struct mach_operand slot = memory(mach_preg(SP, 64), arg->type);
        if (types[i] == IR_AGG && (locations[i].indirect ||
                                   locations[i].stack)) {
            copies[i] = copy_argument(s, &locations[i],
                                      select_layout(s, callee->params[i].agg),
                                      select_reg(s, arg));
        }
        if (types[i] == IR_AGG && locations[i].stack) {
            int64_t end = locations[i].offset +
                          (locations[i].indirect
                               ? 8
                               : (int64_t)(select_layout(s, callee->params[i].agg)->size + 7) /
                                     8 * 8);
            if (end > next) {
                next = end;
            }
        }
        if (types[i] == IR_AGG || !locations[i].stack) {
            continue;
        }
        slot.value = locations[i].offset;
        if (i >= callee->param_count) {
            slot.width = select_is_float(arg->type) ? width(arg->type) : 64;
        }
        emit2(s, A64_STR,
              arg->kind == IR_INT && arg->as.integer == 0
                  ? mach_imm(0)
                  : select_reg(s, arg),
              slot);
        if (slot.value + slot.width / 8 > next) {
            next = slot.value + slot.width / 8;
        }
    }
    next = (next + 7) / 8 * 8;
    if ((uint64_t)next > s->out->outgoing) {
        s->out->outgoing = (uint64_t)next;
    }
    for (i = 0; i < inst->arg_count; i++) {
        const struct ir_operand *arg = &inst->args[i];
        enum ir_ext ext = IR_EXT_NONE;
        if (locations[i].stack) {
            continue;
        }
        if (types[i] == IR_AGG && locations[i].indirect) {
            move(s, mach_preg(locations[i].reg, 64), copies[i]);
            uses |= (uint64_t)1 << locations[i].reg;
            continue;
        }
        if (types[i] == IR_AGG) {
            uses |= select_load_parts(s, &locations[i], select_reg(s, arg));
            continue;
        }
        if (s->abi == &apple && i < callee->param_count) {
            ext = callee->params[i].ext;
        }
        load_value(s, mach_preg(locations[i].reg, width(arg->type)), arg, ext);
        uses |= (uint64_t)1 << locations[i].reg;
    }
    if (callee->result == IR_AGG && result.indirect) {
        move(s, mach_preg(result.reg, 64), result_address);
        uses |= (uint64_t)1 << result.reg;
    }
    free(types);
    free(locations);
    free(copies);
    f.kind = MACH_FUNC;
    call = select_emit(s, indirect ? A64_BLR : A64_BL, 1,
                       indirect ? &target : &f);
    call->uses = uses;
    call->defs = s->abi->caller_saved;
    if (inst->result != IR_NO_RESULT && callee->result == IR_AGG) {
        if (!result.indirect) {
            select_store_parts(s, &result,
                               select_result_slot(s, inst,
                                                  select_layout(s, callee->result_agg)));
        }
    } else if (inst->result != IR_NO_RESULT && select_is_float(inst->type)) {
        emit2(s, A64_FMOV, select_result(s, inst),
              mach_preg(s->abi->fp_result, width(inst->type)));
    } else if (inst->result != IR_NO_RESULT) {
        move(s, select_result(s, inst),
             mach_preg(s->abi->int_result, width(inst->type)));
    }
}

static void emit_ret(struct selector *s, const struct ir_inst *inst)
{
    struct mach_inst *ret;

    uint8_t result = select_is_float(inst->type) ? s->abi->fp_result
                                                  : s->abi->int_result;
    struct arg_location loc;
    uint64_t uses = 0;

    if (s->f->result == IR_AGG) {
        locate_result(s, s->f, &loc);
        if (loc.indirect) {
            copy_memory(s, s->result_address, select_reg(s, &inst->a),
                        select_layout(s, s->f->result_agg)->size);
        } else {
            uses = select_load_parts(s, &loc, select_reg(s, &inst->a));
        }
    } else if (inst->type != IR_VOID) {
        load_value(s, mach_preg(result, width(inst->type)), &inst->a,
                   IR_EXT_NONE);
        uses = (uint64_t)1 << result;
    }
    ret = select_emit(s, A64_RET, 0, NULL);
    ret->uses = uses;
}

static struct mach_operand memory(struct mach_operand base, enum ir_type type)
{
    struct mach_operand m = base;

    m.kind = MACH_MEM;
    m.base_vreg = base.kind == MACH_VREG;
    m.width = type == IR_I8    ? 8
              : type == IR_I16 ? 16
              : type == IR_I32 ? 32
                               : 64;
    m.value = 0;
    return m;
}

static bool match_scalar(const struct selector *s, const struct ir_inst *inst)
{
    enum ir_type type = inst->op == IR_STORE ? inst->a.type : inst->type;

    (void)s;
    return type != IR_AGG;
}

/* A slot is an offset from sp that frame layout fills in. */
static void emit_slot(struct selector *s, const struct ir_inst *inst)
{
    struct mach_operand slot = mach_imm(mach_slot_add(s->out, select_size(s, inst->of),
                                                      select_align(s, inst->of)));

    slot.kind = MACH_SLOT;
    emit3(s, A64_ADD, select_result(s, inst), mach_preg(SP, 64), slot);
}

/* The memory operand of a load or a store: the folded address that
   selection found, or the pointer register itself. */
static struct mach_operand address_of(struct selector *s,
                                      const struct ir_operand *pointer,
                                      enum ir_type type)
{
    struct mach_operand m;

    if (!s->has_address) {
        return memory(select_reg(s, pointer), type);
    }
    m = memory(select_reg(s, s->address.base), type);
    m.value = s->address.offset;
    if (s->address.index != NULL) {
        m.index_reg = s->address.index->as.temp;
        m.index_vreg = true;
        m.scale = (uint8_t)(1 << s->address.shift);
    }
    return m;
}

/* A load or a store adds an unsigned 12-bit offset scaled by the size, a
   signed 9-bit offset or an index to the base. The index shifts by 0 or
   by the scale of the size. A store with an index takes only zero from
   wzr or xzr, so that it reads two registers. */
static bool fits_address(const struct selector *s, const struct address *a,
                         const struct ir_inst *use)
{
    enum ir_type type = use->op == IR_STORE ? use->a.type : use->type;
    uint8_t scale = type == IR_I8    ? 0
                    : type == IR_I16 ? 1
                    : type == IR_I32 ? 2
                                     : 3;
    int64_t size = (int64_t)1 << scale;

    (void)s;
    if (a->index == NULL) {
        return (a->offset >= 0 && a->offset % size == 0 &&
                a->offset / size <= 4095) ||
               (a->offset >= -256 && a->offset <= 255);
    }
    if (a->offset != 0 || (a->shift != 0 && a->shift != scale)) {
        return false;
    }
    return use->op == IR_LOAD ||
           (use->a.kind == IR_INT && use->a.as.integer == 0);
}

static void emit_load(struct selector *s, const struct ir_inst *inst)
{
    emit2(s, A64_LDR, select_result(s, inst),
          address_of(s, &inst->a, inst->type));
}

/* A store of zero reads the zero register, wzr or xzr. */
static void emit_store(struct selector *s, const struct ir_inst *inst)
{
    struct mach_operand value =
        inst->a.kind == IR_INT && inst->a.as.integer == 0
            ? mach_imm(0)
            : select_reg(s, &inst->a);

    emit2(s, A64_STR, value, address_of(s, &inst->b, inst->a.type));
}

static const struct pattern patterns[] = {
    {IR_COPY, match_copy, emit_copy},
    {IR_COPY, match_float, emit_float_copy},
    {IR_FADD, NULL, emit_float_binary},
    {IR_FSUB, NULL, emit_float_binary},
    {IR_FMUL, NULL, emit_float_binary},
    {IR_FDIV, NULL, emit_float_binary},
    {IR_FNEG, NULL, emit_float_neg},
    {IR_SITOF, NULL, emit_float_convert},
    {IR_UITOF, NULL, emit_float_convert},
    {IR_FTOSI, NULL, emit_float_convert},
    {IR_FTOUI, NULL, emit_float_convert},
    {IR_FEXT, NULL, emit_float_convert},
    {IR_FTRUNC, NULL, emit_float_convert},
    {IR_FEQ, NULL, emit_float_compare},
    {IR_FNE, NULL, emit_float_compare},
    {IR_FLT, NULL, emit_float_compare},
    {IR_FLE, NULL, emit_float_compare},
    {IR_FGT, NULL, emit_float_compare},
    {IR_FGE, NULL, emit_float_compare},
    {IR_ADD, match_arith, emit_add_sub},
    {IR_SUB, match_arith, emit_add_sub},
    {IR_MUL, match_arith, emit_binary},
    {IR_AND, match_arith, emit_binary},
    {IR_OR, match_arith, emit_binary},
    {IR_XOR, match_not_bool, emit_not_bool},
    {IR_XOR, match_arith, emit_binary},
    {IR_NEG, match_arith, emit_unary},
    {IR_NOT, match_arith, emit_unary},
    {IR_EQ, match_compare, emit_set},
    {IR_NE, match_compare, emit_set},
    {IR_SLT, match_compare, emit_set},
    {IR_SLE, match_compare, emit_set},
    {IR_SGT, match_compare, emit_set},
    {IR_SGE, match_compare, emit_set},
    {IR_ULT, match_compare, emit_set},
    {IR_ULE, match_compare, emit_set},
    {IR_UGT, match_compare, emit_set},
    {IR_UGE, match_compare, emit_set},
    {IR_JUMP, NULL, emit_jump},
    {IR_BRANCH, match_fused, emit_fused_branch},
    {IR_BRANCH, NULL, emit_branch},
    {IR_CALL, match_call, emit_call},
    {IR_RET, NULL, emit_ret},
    {IR_SLOT, NULL, emit_slot},
    {IR_LOAD, match_scalar, emit_load},
    {IR_STORE, match_scalar, emit_store},
    {IR_PTRADD, NULL, emit_add_sub},
    {IR_MEMCOPY, NULL, emit_memcopy},
    {IR_SDIV, match_arith, emit_div},
    {IR_UDIV, match_arith, emit_div},
    {IR_SREM, match_arith, emit_div},
    {IR_UREM, match_arith, emit_div},
    {IR_SHL, match_arith, emit_shift},
    {IR_SHR_S, match_arith, emit_shift},
    {IR_SHR_U, match_arith, emit_shift},
    {IR_TRUNC, match_convert, emit_convert},
    {IR_SEXT, match_convert, emit_convert},
    {IR_ZEXT, match_convert, emit_convert},
    {IR_ADDR, NULL, emit_addr},
};

/* Printing */

static void print_base(struct text *out, const struct mach_operand *o)
{
    if (o->base_vreg) {
        text_appendf(out, "t%" PRIu32, o->reg);
    } else if (o->reg == SP) {
        text_append(out, "sp");
    } else {
        text_appendf(out, "x%" PRIu32, o->reg);
    }
}

/* The four forms: plain, with an offset, pre-indexed with a final
   exclamation mark, and post-indexed with the offset after the bracket. */
static void print_memory(struct text *out, const struct mach_operand *o)
{
    text_append(out, "[");
    print_base(out, o);
    if (o->index == INDEX_POST) {
        text_appendf(out, "], #%" PRId64, o->value);
        return;
    }
    if (o->scale != 0) {
        text_appendf(out, o->index_vreg ? ", t%" PRIu32 : ", x%" PRIu32,
                     o->index_reg);
        if (o->scale > 1) {
            text_appendf(out, ", lsl #%u", popcount((uint64_t)o->scale - 1));
        }
    }
    if (o->value != 0 || o->index == INDEX_PRE) {
        text_appendf(out, ", #%" PRId64, o->value);
    }
    text_append(out, o->index == INDEX_PRE ? "]!" : "]");
}

static void print_operand(struct text *out, const struct ir_module *m,
                          const struct names *names,
                          const struct mach_operand *o)
{

    switch (o->kind) {
    case MACH_VREG:
        text_appendf(out, "t%" PRIu32, o->reg);
        break;
    case MACH_PREG:
        if (o->reg == SP) {
            text_append(out, o->width == 64 ? "sp" : "wsp");
        } else if (o->reg >= V0) {
            text_appendf(out, "%c%" PRIu32, o->width == 64 ? 'd' : 's',
                         o->reg - V0);
        } else {
            text_appendf(out, "%c%" PRIu32, o->width == 64 ? 'x' : 'w',
                         o->reg);
        }
        break;
    case MACH_IMM:
        text_appendf(out, "#%" PRId64, o->value);
        break;
    case MACH_BLOCK:
    case MACH_FUNC:
    case MACH_GLOBAL:
    case MACH_NAME:
        mach_symbol(out, m, names, o);
        break;
    case MACH_SLOT:
        text_appendf(out, "slot%" PRId64, o->value);
        break;
    case MACH_MEM:
        print_memory(out, o);
        break;
    default:
        break;
    }
}

static const char *const cond_names[] = {
    [COND_EQ] = "eq", [COND_NE] = "ne", [COND_LT] = "lt", [COND_LE] = "le",
    [COND_GT] = "gt", [COND_GE] = "ge", [COND_LO] = "lo", [COND_LS] = "ls",
    [COND_HI] = "hi", [COND_HS] = "hs", [COND_MI] = "mi", [COND_PL] = "pl",
};

static void print(struct text *out, const struct ir_module *m,
                  const struct mach_inst *inst, const struct names *names)
{
    bool macho = names != NULL &&
                 target_info(names->target)->format == FORMAT_MACHO;
    size_t i;

    if (inst->op == A64_BCOND) {
        text_appendf(out, "b.%s ", cond_names[inst->operands[0].value]);
        print_operand(out, m, names, &inst->operands[1]);
        return;
    }
    if (inst->op >= A64_SEH_SAVE_FPLR_X) {
        /* A directive takes a register and a number without #. The
           directives end the opcode list. */
        text_append(out, opcodes[inst->op].name);
        for (i = 0; i < inst->count; i++) {
            text_append(out, i == 0 ? " " : ", ");
            if (inst->operands[i].kind == MACH_IMM) {
                text_appendf(out, "%" PRId64, inst->operands[i].value);
            } else {
                print_operand(out, m, names, &inst->operands[i]);
            }
        }
        return;
    }
    text_append(out, opcodes[inst->op].name);
    if ((inst->op == A64_LDR || inst->op == A64_STR) &&
        inst->operands[1].width < 32) {
        text_append(out, inst->operands[1].width == 8 ? "b" : "h");
    }
    for (i = 0; i < inst->count; i++) {
        const struct mach_operand *o = &inst->operands[i];
        text_append(out, i == 0 ? " " : ", ");
        if (o->kind == MACH_COND) {
            text_append(out, cond_names[o->value]);
        } else if (inst->op == A64_ADD &&
                   (o->kind == MACH_FUNC || o->kind == MACH_GLOBAL)) {
            /* The low 12 bits: sym@PAGEOFF on Mach-O, :lo12:sym on ELF,
               COFF and in the dumps. */
            text_append(out, macho ? "" : ":lo12:");
            print_operand(out, m, names, o);
            text_append(out, macho ? "@PAGEOFF" : "");
        } else if (inst->op == A64_LDRGOT && i == 1) {
            /* ldr x0, [x0, :got_lo12:sym] or [x0, sym@GOTPAGEOFF]. */
            text_append(out, "[");
            print_operand(out, m, names, o);
            text_append(out, macho ? ", " : ", :got_lo12:");
            print_operand(out, m, names, &inst->operands[2]);
            text_append(out, macho ? "@GOTPAGEOFF]" : "]");
            break;
        } else if (inst->op == A64_ADRP && i == 1 && o->got) {
            text_append(out, macho ? "" : ":got:");
            print_operand(out, m, names, o);
            text_append(out, macho ? "@GOTPAGE" : "");
        } else if (inst->op == A64_ADRP && i == 1) {
            print_operand(out, m, names, o);
            text_append(out, macho ? "@PAGE" : "");
        } else if (inst->op == A64_STR && i == 0 && o->kind == MACH_IMM) {
            text_append(out, inst->operands[1].width == 64 ? "xzr" : "wzr");
        } else if (inst->op == A64_CMP && i == 2 &&
                   inst->operands[1].kind != MACH_IMM) {
            text_append(out, opcodes[o->value].name);
        } else if (o->kind == MACH_IMM && i > 0 &&
                   inst->operands[i - 1].kind == MACH_IMM) {
            text_appendf(out, "lsl #%" PRId64, o->value);
        } else {
            print_operand(out, m, names, o);
        }
    }
}

/* Frames */

static struct mach_operand stack(int64_t offset, enum mach_index index)
{
    struct mach_operand m = mach_preg(SP, 64);

    m.kind = MACH_MEM;
    m.value = offset;
    m.index = index;
    return m;
}

/* Memory at sp plus the register index. */
static struct mach_operand stack_indexed(uint8_t index)
{
    struct mach_operand m = stack(0, INDEX_NONE);

    m.index_reg = index;
    m.scale = 1;
    return m;
}

/* ldr and str of 64 bits take an offset from sp that is a multiple of 8
   up to 32760. A larger offset goes into a register first: ldr uses the
   register it loads, and str the scratch register that it does not
   store. */
static bool fits_scaled(int64_t offset)
{
    return offset >= 0 && offset % 8 == 0 && offset / 8 <= 4095;
}

/* A float register cannot hold the offset, so its load goes through x16.
   A store names its value before its address, so the allocator loads a
   spilled float value before a spilled integer address into x16. */
static void load_spill(struct mach_block *b, uint8_t reg, int64_t offset)
{
    uint8_t index = reg >= V0 ? X16 : reg;
    struct mach_operand ops[2];

    ops[0] = mach_preg(reg, 64);
    ops[1] = stack(offset, INDEX_NONE);
    if (!fits_scaled(offset)) {
        load_into(b, mach_preg(index, 64), (uint64_t)offset);
        ops[1] = stack_indexed(index);
    }
    append(b, A64_LDR, 2, ops);
}

static void store_spill(struct mach_block *b, uint8_t reg, int64_t offset)
{
    uint8_t other = reg == X16 ? X17 : X16;
    struct mach_operand ops[2];

    ops[0] = mach_preg(reg, 64);
    ops[1] = stack(offset, INDEX_NONE);
    if (!fits_scaled(offset)) {
        load_into(b, mach_preg(other, 64), (uint64_t)offset);
        ops[1] = stack_indexed(other);
    }
    append(b, A64_STR, 2, ops);
}

/* The address of a slot is add dst, sp, #offset. An offset that no 12-bit
   immediate holds, shifted or not, goes into dst first. */
static void expand(struct mach_block *b, const struct mach_inst *inst)
{
    struct mach_operand ops[4];
    int64_t offset;

    if (inst->op != A64_ADD || inst->count != 3 ||
        inst->operands[1].kind != MACH_PREG || inst->operands[1].reg != SP ||
        inst->operands[2].kind != MACH_IMM || inst->operands[2].value <= 4095) {
        *mach_append(b) = *inst;
        return;
    }
    offset = inst->operands[2].value;
    ops[0] = inst->operands[0];
    ops[1] = inst->operands[1];
    if (fits_imm12(offset)) {
        append_imm12(b, A64_ADD, 2, ops, offset);
        return;
    }
    load_into(b, ops[0], (uint64_t)offset);
    ops[2] = ops[0];
    append(b, A64_ADD, 3, ops);
}

/* Move sp down by size bytes of the frame, through x16 when no 12-bit
   immediate holds it. Windows probes a frame of 4096 bytes or more with
   __chkstk first, which takes the size divided by 16 in x15. */
static void allocate_frame(struct mach_block *b, const struct frame *frame,
                           uint64_t bytes)
{
    struct mach_operand ops[4];
    int64_t size = (int64_t)bytes;

    if (frame->probe) {
        load_into(b, mach_preg(X15, 64), bytes / 16);
        memset(ops, 0, sizeof ops);
        ops[0].kind = MACH_NAME;
        ops[0].name = "__chkstk";
        append(b, A64_BL, 1, ops);
    }
    ops[0] = mach_preg(SP, 64);
    ops[1] = mach_preg(SP, 64);
    if (fits_imm12(size)) {
        append_imm12(b, A64_SUB, 2, ops, size);
        return;
    }
    load_into(b, mach_preg(X16, 64), bytes);
    ops[2] = mach_preg(X16, 64);
    append(b, A64_SUB, 3, ops);
}

static void resolve_slot(struct mach_operand *o, int64_t offset)
{
    o->kind = MACH_IMM;
    o->value = offset;
}

/* Append the unwind directive op with count operands to a prologue or an
   epilogue that carries directives. */
static void unwind(struct mach_block *b, const struct frame *frame,
                   enum a64_op op, size_t count,
                   const struct mach_operand *operands)
{
    if (frame->unwind) {
        append(b, op, count, operands);
    }
}

/* The bytes at the top of the frame that the saved registers take, as a
   multiple of 16, which keeps sp aligned. */
static uint64_t save_area(const struct frame *frame)
{
    uint64_t lowest = frame->size;
    size_t i;

    for (i = 0; i < frame->saved_count; i++) {
        if ((uint64_t)frame->saved_offset[i] < lowest) {
            lowest = (uint64_t)frame->saved_offset[i];
        }
    }
    return (frame->size - lowest + 15) / 16 * 16;
}

/* The directive of the save of register reg at offset from sp. */
static void unwind_save(struct mach_block *b, const struct frame *frame,
                        uint8_t reg, int64_t offset)
{
    struct mach_operand ops[2];

    ops[0] = mach_preg(reg, 64);
    ops[1] = mach_imm(offset);
    unwind(b, frame, reg >= V0 ? A64_SEH_SAVE_FREG : A64_SEH_SAVE_REG, 2, ops);
}

/* The frame record of x29 and x30 sits above the frame, and x29 points to
   it. Callee-saved registers take the top of the frame, slots its
   bottom.
   DESIGN: Windows unwinding maps each prologue instruction to one unwind
   code, and a save code holds an offset from sp of at most 504. With
   unwind data the prologue therefore allocates the save area first, saves
   into it and points x29 at the frame record. The rest of the frame
   follows in the body, which x29 allows, as Microsoft's ARM64 exception
   handling page states for a dedicated frame pointer. */
static void prologue(struct mach_block *b, const struct frame *frame)
{
    struct mach_operand ops[3];
    uint64_t area = save_area(frame);
    size_t i;

    if (!frame->needed) {
        return;
    }
    ops[0] = mach_preg(X29, 64);
    ops[1] = mach_preg(X30, 64);
    ops[2] = stack(-16, INDEX_PRE);
    append(b, A64_STP, 3, ops);
    ops[0] = mach_imm(16);
    unwind(b, frame, A64_SEH_SAVE_FPLR_X, 1, ops);
    ops[0] = mach_preg(X29, 64);
    ops[1] = mach_preg(SP, 64);
    if (!frame->unwind) {
        append(b, A64_MOV, 2, ops);
        if (frame->size > 0) {
            allocate_frame(b, frame, frame->size);
        }
        for (i = 0; i < frame->saved_count; i++) {
            store_spill(b, frame->saved[i], frame->saved_offset[i]);
        }
        return;
    }
    if (area == 0) {
        append(b, A64_MOV, 2, ops);
        unwind(b, frame, A64_SEH_SET_FP, 0, ops);
    } else {
        ops[0] = mach_preg(SP, 64);
        append_imm12(b, A64_SUB, 2, ops, (int64_t)area);
        ops[0] = mach_imm((int64_t)area);
        unwind(b, frame, A64_SEH_STACKALLOC, 1, ops);
        for (i = 0; i < frame->saved_count; i++) {
            int64_t offset = frame->saved_offset[i] -
                             (int64_t)(frame->size - area);
            store_spill(b, frame->saved[i], offset);
            unwind_save(b, frame, frame->saved[i], offset);
        }
        ops[0] = mach_preg(X29, 64);
        ops[1] = mach_preg(SP, 64);
        append_imm12(b, A64_ADD, 2, ops, (int64_t)area);
        ops[0] = mach_imm((int64_t)area);
        unwind(b, frame, A64_SEH_ADD_FP, 1, ops);
    }
    unwind(b, frame, A64_SEH_ENDPROLOGUE, 0, ops);
    if (frame->size > area) {
        allocate_frame(b, frame, frame->size - area);
    }
}

/* Move sp up by bytes. Each instruction gets its unwind directive: a nop
   for the load of x16 and the allocation for the add. */
static void free_stack(struct mach_block *b, const struct frame *frame,
                       uint64_t bytes)
{
    struct mach_operand ops[4];
    struct mach_block load;
    size_t i;

    ops[0] = mach_preg(SP, 64);
    ops[1] = mach_preg(SP, 64);
    if (fits_imm12((int64_t)bytes)) {
        append_imm12(b, A64_ADD, 2, ops, (int64_t)bytes);
    } else {
        memset(&load, 0, sizeof load);
        load_into(&load, mach_preg(X16, 64), bytes);
        for (i = 0; i < load.count; i++) {
            *mach_append(b) = load.insts[i];
            unwind(b, frame, A64_SEH_NOP, 0, ops);
        }
        free(load.insts);
        ops[2] = mach_preg(X16, 64);
        append(b, A64_ADD, 3, ops);
    }
    ops[0] = mach_imm((int64_t)bytes);
    unwind(b, frame, A64_SEH_STACKALLOC, 1, ops);
}

/* With unwind data the epilogue mirrors the prologue. It frees the body
   part of the frame and restores the saves. It then frees the save area
   and loads the frame record, each step with its directive. */
static void epilogue(struct mach_block *b, const struct frame *frame)
{
    struct mach_operand ops[3];
    uint64_t area = save_area(frame);
    size_t i;

    if (!frame->needed) {
        return;
    }
    if (frame->unwind) {
        unwind(b, frame, A64_SEH_STARTEPILOGUE, 0, ops);
        if (frame->size > area) {
            free_stack(b, frame, frame->size - area);
        }
        for (i = 0; i < frame->saved_count; i++) {
            int64_t offset = frame->saved_offset[i] -
                             (int64_t)(frame->size - area);
            load_spill(b, frame->saved[i], offset);
            unwind_save(b, frame, frame->saved[i], offset);
        }
        if (area > 0) {
            free_stack(b, frame, area);
        }
        ops[0] = mach_preg(X29, 64);
        ops[1] = mach_preg(X30, 64);
        ops[2] = stack(16, INDEX_POST);
        append(b, A64_LDP, 3, ops);
        ops[0] = mach_imm(16);
        unwind(b, frame, A64_SEH_SAVE_FPLR_X, 1, ops);
        unwind(b, frame, A64_SEH_ENDEPILOGUE, 0, ops);
        return;
    }
    for (i = 0; i < frame->saved_count; i++) {
        load_spill(b, frame->saved[i], frame->saved_offset[i]);
    }
    if (frame->size > 0) {
        ops[0] = mach_preg(SP, 64);
        ops[1] = mach_preg(X29, 64);
        append(b, A64_MOV, 2, ops);
    }
    ops[0] = mach_preg(X29, 64);
    ops[1] = mach_preg(X30, 64);
    ops[2] = stack(16, INDEX_POST);
    append(b, A64_LDP, 3, ops);
}

/* A parameter beyond x7 lies in the argument area of the caller, which
   starts 16 bytes above the frame record that x29 points to. */
static void stack_param(struct selector *s, int64_t offset,
                        struct mach_operand dst)
{
    const struct ir_function *f = s->f;
    struct mach_operand m = mach_preg(X29, 64);
    size_t i;

    m.kind = MACH_MEM;
    m.value = 16 + offset;
    m.width = dst.width;
    for (i = 0; i < f->param_count; i++) {
        if (f->params[i].temp == dst.reg) {
            m = memory(mach_preg(X29, 64), f->params[i].type);
            m.value = 16 + offset;
        }
    }
    s->out->stack_params = true;
    emit2(s, A64_LDR, dst, m);
}

static const struct target_desc desc = {
    .name = "arm64",
    .chapter = 15,
    .opcodes = opcodes,
    .patterns = patterns,
    .pattern_count = sizeof patterns / sizeof patterns[0],
    .abi = abi,
    .width = width,
    .move = move,
    .load = load,
    .print = print,
    .fits_address = fits_address,
    .stack_param = stack_param,
    .locate = locate,
    .load_float = load_float,
    .locate_result = locate_result,
    .load_bytes = load_bytes,
    .store_bytes = store_bytes,
    .slot_address = slot_address,
    .incoming_address = incoming_address,
    .copy_memory = copy_memory,
    .frame_limit = 0,
    .load_spill = load_spill,
    .store_spill = store_spill,
    .resolve_slot = resolve_slot,
    .expand = expand,
    .prologue = prologue,
    .epilogue = epilogue,
};

const struct target_desc *target_desc_arm64(void)
{
    return &desc;
}
