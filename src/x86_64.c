#include "select.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "regalloc.h"

/* The x86_64 back end: registers, opcodes and the pattern table of
   instruction selection. Register numbers follow the instruction encoding,
   from rax as 0 to r15 as 15, and xmm0 to xmm15 follow as 16 to 31. */

enum {
    RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13,
    R14, R15, XMM0, XMM1, XMM2, XMM3, XMM4, XMM5, XMM6, XMM7, XMM8, XMM9,
    XMM10, XMM11, XMM12, XMM13, XMM14, XMM15
};

enum x64_op {
    X64_MOV, X64_MOVABS, X64_ADD, X64_SUB, X64_IMUL, X64_IMUL3, X64_AND,
    X64_OR, X64_XOR, X64_NEG, X64_NOT, X64_CMP, X64_TEST, X64_SET, X64_JMP,
    X64_J, X64_CALL, X64_CALLR, X64_RET, X64_LEA, X64_PUSH, X64_POP, X64_CQTO,
    X64_CLTD, X64_IDIV, X64_DIV, X64_SHL, X64_SAR, X64_SHR, X64_MOVSX,
    X64_MOVZX, X64_MOVS, X64_ADDS, X64_SUBS, X64_MULS, X64_DIVS, X64_UCOMIS,
    X64_CVTSI2S, X64_CVTTS2SI, X64_CVTS2S, X64_MOVQX, X64_XORP, X64_ANDP,
    X64_ANDNP, X64_ORP, X64_MOVUPS, X64_CMOV, X64_MOVL, X64_SEH_PUSHREG,
    X64_SEH_STACKALLOC, X64_SEH_SAVEREG, X64_SEH_SAVEXMM, X64_SEH_ENDPROLOGUE
};

#define USE ROLE_USE
#define DEF ROLE_DEF

/* Most x86_64 instructions have two operands, and the first one is both
   read and written, as in add dst, src. */
static const struct mach_opcode opcodes[] = {
    [X64_MOV] = {"mov", {DEF, USE}, FLAG_MOVE},
    [X64_MOVABS] = {"movabs", {DEF}, 0},
    [X64_ADD] = {"add", {USE | DEF, USE}, 0},
    [X64_SUB] = {"sub", {USE | DEF, USE}, 0},
    [X64_IMUL] = {"imul", {USE | DEF, USE}, 0},
    [X64_IMUL3] = {"imul", {DEF, USE}, 0},
    [X64_AND] = {"and", {USE | DEF, USE}, 0},
    [X64_OR] = {"or", {USE | DEF, USE}, 0},
    [X64_XOR] = {"xor", {USE | DEF, USE}, 0},
    [X64_NEG] = {"neg", {USE | DEF}, 0},
    [X64_NOT] = {"not", {USE | DEF}, 0},
    [X64_CMP] = {"cmp", {USE, USE}, 0},
    [X64_TEST] = {"test", {USE, USE}, 0},
    [X64_SET] = {"set", {DEF}, 0},
    [X64_JMP] = {"jmp", {0}, FLAG_JUMP},
    [X64_J] = {"j", {0}, FLAG_BRANCH},
    [X64_CALL] = {"call", {0}, FLAG_CALL},
    [X64_CALLR] = {"call", {USE}, FLAG_CALL},
    [X64_RET] = {"ret", {0}, FLAG_RET},
    /* Unwind directives of a Windows prologue, lines of the assembly file
       without an instruction. */
    [X64_SEH_PUSHREG] = {".seh_pushreg", {0}, 0},
    [X64_SEH_STACKALLOC] = {".seh_stackalloc", {0}, 0},
    [X64_SEH_SAVEREG] = {".seh_savereg", {0}, 0},
    [X64_SEH_SAVEXMM] = {".seh_savexmm", {0}, 0},
    [X64_SEH_ENDPROLOGUE] = {".seh_endprologue", {0}, 0},
    [X64_LEA] = {"lea", {DEF}, 0},
    [X64_PUSH] = {"push", {USE}, 0},
    [X64_POP] = {"pop", {DEF}, 0},
    [X64_CQTO] = {"cqto", {0}, 0},
    [X64_CLTD] = {"cltd", {0}, 0},
    [X64_IDIV] = {"idiv", {USE}, 0},
    [X64_DIV] = {"div", {USE}, 0},
    [X64_SHL] = {"shl", {USE | DEF, USE}, 0},
    [X64_SAR] = {"sar", {USE | DEF, USE}, 0},
    [X64_SHR] = {"shr", {USE | DEF, USE}, 0},
    [X64_MOVSX] = {"movs", {DEF, USE}, 0},
    [X64_MOVZX] = {"movz", {DEF, USE}, 0},
    /* The SSE instructions end in sd or ss by the width of the float. */
    [X64_MOVS] = {"movs", {DEF, USE}, FLAG_MOVE},
    [X64_ADDS] = {"adds", {USE | DEF, USE}, 0},
    [X64_SUBS] = {"subs", {USE | DEF, USE}, 0},
    [X64_MULS] = {"muls", {USE | DEF, USE}, 0},
    [X64_DIVS] = {"divs", {USE | DEF, USE}, 0},
    [X64_UCOMIS] = {"ucomis", {USE, USE}, 0},
    [X64_CVTSI2S] = {"cvtsi2s", {DEF, USE}, 0},
    [X64_CVTTS2SI] = {"cvtts", {DEF, USE}, 0},
    [X64_CVTS2S] = {"cvts", {DEF, USE}, 0},
    [X64_MOVQX] = {"mov", {DEF, USE}, 0},
    [X64_XORP] = {"xorp", {USE | DEF, USE}, 0},
    [X64_ANDP] = {"andp", {USE | DEF, USE}, 0},
    [X64_ANDNP] = {"andnp", {USE | DEF, USE}, 0},
    [X64_ORP] = {"orp", {USE | DEF, USE}, 0},
    [X64_MOVUPS] = {"movups", {DEF, USE}, 0},
    [X64_CMOV] = {"cmov", {USE | DEF, USE}, 0},
    /* A 32-bit move that clears the upper 32 bits, even of its own
       register, so it is no move that register allocation may drop. */
    [X64_MOVL] = {"mov", {DEF, USE}, 0},
};

#define BIT(r) ((uint64_t)1 << (r))

static const uint8_t sysv_args[] = {RDI, RSI, RDX, RCX, R8, R9};
static const uint8_t windows_args[] = {RCX, RDX, R8, R9};
static const uint8_t sysv_fp_args[] = {
    XMM0, XMM1, XMM2, XMM3, XMM4, XMM5, XMM6, XMM7
};
static const uint8_t windows_fp_args[] = {XMM0, XMM1, XMM2, XMM3};

/* DESIGN: r10 and r11 are the scratch registers of spilled values, and
   rbp holds the frame pointer. Allocation prefers the caller-saved
   registers and uses the callee-saved ones last. */
static const uint8_t sysv_allocatable[] = {
    RAX, RCX, RDX, RSI, RDI, R8, R9, RBX, R12, R13, R14, R15
};
static const uint8_t windows_allocatable[] = {
    RAX, RCX, RDX, R8, R9, RBX, RSI, RDI, R12, R13, R14, R15
};

/* DESIGN: xmm14 and xmm15 are the float scratch registers under System V,
   where every xmm register is caller-saved. Windows preserves xmm6 to
   xmm15, so its float scratch registers are the caller-saved xmm4 and
   xmm5. */
static const uint8_t sysv_fp_allocatable[] = {
    XMM0, XMM1, XMM2, XMM3, XMM4, XMM5, XMM6, XMM7, XMM8, XMM9, XMM10,
    XMM11, XMM12, XMM13
};
static const uint8_t windows_fp_allocatable[] = {
    XMM0, XMM1, XMM2, XMM3, XMM6, XMM7, XMM8, XMM9, XMM10, XMM11, XMM12,
    XMM13, XMM14, XMM15
};

#define XMM_ALL (((uint64_t)0xffff) << XMM0)
#define XMM_WINDOWS_CALLEE_SAVED (((uint64_t)0x3ff) << XMM6)

static const struct abi sysv = {
    .int_args = sysv_args,
    .int_arg_count = 6,
    .int_result = RAX,
    .caller_saved = BIT(RAX) | BIT(RCX) | BIT(RDX) | BIT(RSI) | BIT(RDI) |
                    BIT(R8) | BIT(R9) | BIT(R10) | BIT(R11) | XMM_ALL,
    .callee_saved = BIT(RBX) | BIT(R12) | BIT(R13) | BIT(R14) | BIT(R15),
    .allocatable = sysv_allocatable,
    .allocatable_count = sizeof sysv_allocatable,
    .scratch = {R10, R11},
    .shadow_space = 0,
    .probe_stack = false,
    .fp_args = sysv_fp_args,
    .fp_arg_count = 8,
    .fp_result = XMM0,
    .fp_allocatable = sysv_fp_allocatable,
    .fp_allocatable_count = sizeof sysv_fp_allocatable,
    .fp_scratch = {XMM14, XMM15},
    .fp_save_size = 8,
};
static const struct abi windows = {
    .int_args = windows_args,
    .int_arg_count = 4,
    .int_result = RAX,
    .caller_saved = BIT(RAX) | BIT(RCX) | BIT(RDX) | BIT(R8) | BIT(R9) |
                    BIT(R10) | BIT(R11) | (XMM_ALL & ~XMM_WINDOWS_CALLEE_SAVED),
    .callee_saved = BIT(RBX) | BIT(RSI) | BIT(RDI) | BIT(R12) | BIT(R13) |
                    BIT(R14) | BIT(R15) | XMM_WINDOWS_CALLEE_SAVED,
    .allocatable = windows_allocatable,
    .allocatable_count = sizeof windows_allocatable,
    .scratch = {R10, R11},
    .shadow_space = 32,
    .probe_stack = true,
    .fp_args = windows_fp_args,
    .fp_arg_count = 4,
    .fp_result = XMM0,
    .fp_allocatable = windows_fp_allocatable,
    .fp_allocatable_count = sizeof windows_fp_allocatable,
    .fp_scratch = {XMM4, XMM5},
    .fp_save_size = 16,
};

static const struct abi *abi(enum convention convention)
{
    return convention == CONVENTION_WINDOWS_X64 ? &windows : &sysv;
}

static uint8_t width(enum ir_type type)
{
    switch (type) {
    case IR_I8: return 8;
    case IR_I16: return 16;
    case IR_I32:
    case IR_F32: return 32;
    default: return 64;
    }
}

static struct mach_inst *emit1(struct selector *s, enum x64_op op,
                               struct mach_operand a)
{
    return select_emit(s, (uint16_t)op, 1, &a);
}

static void emit2(struct selector *s, enum x64_op op, struct mach_operand a,
                  struct mach_operand b)
{
    struct mach_operand ops[2];
    ops[0] = a;
    ops[1] = b;
    select_emit(s, (uint16_t)op, 2, ops);
}

/* A move between two registers, with movsd or movss for floats. */
static void move(struct selector *s, struct mach_operand dst,
                 struct mach_operand src)
{
    bool fp = dst.kind == MACH_VREG ? s->out->fp[dst.reg] : dst.reg >= XMM0;

    emit2(s, fp ? X64_MOVS : X64_MOV, dst, src);
}

static void move_float(struct selector *s, struct mach_operand dst,
                       struct mach_operand src)
{
    emit2(s, X64_MOVS, dst, src);
}

static int64_t signed_value(uint64_t v, uint8_t w)
{
    switch (w) {
    case 8: return (int8_t)(uint8_t)v;
    case 16: return (int16_t)(uint16_t)v;
    case 32: return (int32_t)(uint32_t)v;
    default: return (int64_t)v;
    }
}

/* An immediate of a 64-bit instruction is 32 bits, sign-extended. Smaller
   instructions take an immediate of their own width. */
static bool fits_imm(const struct ir_operand *o, uint8_t w)
{
    int64_t v = signed_value(o->as.integer, w);
    return o->kind == IR_INT && (w < 64 || (v >= INT32_MIN && v <= INT32_MAX));
}

static void load(struct selector *s, struct mach_operand dst, uint64_t value)
{
    int64_t v = signed_value(value, dst.width);

    if (dst.width == 64 && (v < INT32_MIN || v > INT32_MAX)) {
        emit2(s, X64_MOVABS, dst, mach_imm(v));
    } else {
        emit2(s, X64_MOV, dst, mach_imm(v));
    }
}

/* Put operand a into register r, unless a is r already. */
static void load_into(struct selector *s, struct mach_operand r,
                      const struct ir_operand *a)
{
    if (a->kind == IR_INT) {
        load(s, r, a->as.integer);
    } else if (a->as.temp != r.reg) {
        move(s, r, select_reg(s, a));
    }
}

/* Patterns */

static bool is_int_type(enum ir_type type)
{
    return type == IR_I8 || type == IR_I16 || type == IR_I32 ||
           type == IR_I64 || type == IR_PTR;
}

/* x86_64 has instructions for every integer width. */
static bool match_arith(const struct selector *s, const struct ir_inst *inst)
{
    (void)s;
    return is_int_type(inst->type);
}

static bool match_copy(const struct selector *s, const struct ir_inst *inst)
{
    (void)s;
    return is_int_type(inst->type);
}

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
    return is_int_type(inst->a.type);
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

static void emit_copy(struct selector *s, const struct ir_inst *inst)
{
    load_into(s, select_result(s, inst), &inst->a);
}

/* r = a op b becomes mov a, r and op b, r in two-operand form. When r is
   b itself, the mov would destroy b. A commutative operation then applies
   a to r, and another one saves b in a new register first. */
static void two_operand(struct selector *s, const struct ir_inst *inst,
                        enum x64_op op, bool commutative)
{
    struct mach_operand r = select_result(s, inst);
    struct mach_operand b;

    if (fits_imm(&inst->b, r.width)) {
        load_into(s, r, &inst->a);
        emit2(s, op, r, mach_imm(signed_value(inst->b.as.integer, r.width)));
        return;
    }
    if (inst->b.kind == IR_TEMP && inst->b.as.temp == inst->result) {
        if (commutative) {
            b = fits_imm(&inst->a, r.width)
                    ? mach_imm(signed_value(inst->a.as.integer, r.width))
                    : select_reg(s, &inst->a);
            emit2(s, op, r, b);
            return;
        }
        b = select_new_vreg(s, r.width);
        move(s, b, select_reg(s, &inst->b));
    } else {
        b = select_reg(s, &inst->b);
    }
    load_into(s, r, &inst->a);
    emit2(s, op, r, b);
}

static void emit_binary(struct selector *s, const struct ir_inst *inst)
{
    switch (inst->op) {
    case IR_ADD: two_operand(s, inst, X64_ADD, true); break;
    case IR_SUB: two_operand(s, inst, X64_SUB, false); break;
    case IR_AND: two_operand(s, inst, X64_AND, true); break;
    case IR_OR: two_operand(s, inst, X64_OR, true); break;
    default: two_operand(s, inst, X64_XOR, true); break;
    }
}

static struct mach_operand widened(struct mach_operand o, uint8_t w)
{
    if (o.kind == MACH_VREG || o.kind == MACH_PREG) {
        o.width = w;
    }
    return o;
}

/* imul has a three-operand form with an immediate. It has no two-operand
   form for 8 bits, so 8-bit and 16-bit products use the 32-bit
   registers, whose low bits hold the same product. */
static void emit_mul(struct selector *s, const struct ir_inst *inst)
{
    struct mach_operand r = select_result(s, inst);
    struct mach_operand ops[3];
    uint8_t w = r.width < 32 ? 32 : r.width;

    if (!fits_imm(&inst->b, r.width)) {
        struct mach_operand b;
        if (inst->b.kind == IR_TEMP && inst->b.as.temp == inst->result) {
            b = widened(select_reg(s, &inst->a), w);
        } else {
            b = widened(select_reg(s, &inst->b), w);
            if (inst->a.kind == IR_INT) {
                load(s, widened(r, w), inst->a.as.integer);
            } else if (inst->a.as.temp != inst->result) {
                move(s, widened(r, w), widened(select_reg(s, &inst->a), w));
            }
        }
        emit2(s, X64_IMUL, widened(r, w), b);
        return;
    }
    ops[0] = widened(r, w);
    ops[1] = widened(select_reg(s, &inst->a), w);
    ops[2] = mach_imm(signed_value(inst->b.as.integer, r.width));
    select_emit(s, X64_IMUL3, 3, ops);
}

/* Extend a value of w bits to the 32-bit register dst, or move it when w
   is 32 or 64 bits already. */
static void extend_into(struct selector *s, struct mach_operand dst,
                        const struct ir_operand *value, bool is_signed)
{
    uint8_t w = width(value->type);

    if (value->kind == IR_INT) {
        uint64_t v = value->as.integer;
        load(s, dst, is_signed ? (uint64_t)signed_value(v, w) : v);
    } else if (w < 32) {
        emit2(s, is_signed ? X64_MOVSX : X64_MOVZX, dst, select_reg(s, value));
    } else {
        move(s, dst, select_reg(s, value));
    }
}

/* Division takes the dividend in rdx:rax and leaves the quotient in rax
   and the remainder in rdx. cqto and cltd sign-extend rax into rdx, and an
   unsigned division clears rdx. 8-bit and 16-bit operands divide as 32-bit
   values after an extension. */
static void emit_div(struct selector *s, const struct ir_inst *inst)
{
    bool is_signed = inst->op == IR_SDIV || inst->op == IR_SREM;
    bool remainder = inst->op == IR_SREM || inst->op == IR_UREM;
    struct mach_operand r = select_result(s, inst);
    uint8_t w = r.width < 32 ? 32 : r.width;
    struct mach_operand divisor;
    struct mach_inst *div;

    if (inst->b.kind == IR_TEMP && r.width >= 32) {
        divisor = select_reg(s, &inst->b);
    } else {
        divisor = select_new_vreg(s, w);
        extend_into(s, divisor, &inst->b, is_signed);
    }
    extend_into(s, mach_preg(RAX, w), &inst->a, is_signed);
    if (is_signed) {
        select_emit(s, w == 64 ? X64_CQTO : X64_CLTD, 0, NULL)->uses =
            BIT(RAX);
        s->b->insts[s->b->count - 1].defs = BIT(RDX);
    } else {
        load(s, mach_preg(RDX, w), 0);
    }
    div = emit1(s, is_signed ? X64_IDIV : X64_DIV, divisor);
    div->uses = BIT(RAX) | BIT(RDX);
    div->defs = BIT(RAX) | BIT(RDX);
    move(s, r, mach_preg(remainder ? RDX : RAX, r.width));
}

/* A variable shift count must be in cl. */
static void emit_shift(struct selector *s, const struct ir_inst *inst)
{
    enum x64_op op = inst->op == IR_SHL     ? X64_SHL
                     : inst->op == IR_SHR_S ? X64_SAR
                                            : X64_SHR;
    struct mach_operand r = select_result(s, inst);

    if (inst->b.kind == IR_INT) {
        load_into(s, r, &inst->a);
        emit2(s, op, r, mach_imm((int64_t)(inst->b.as.integer & 0xff)));
        return;
    }
    move(s, mach_preg(RCX, 8), widened(select_reg(s, &inst->b), 8));
    load_into(s, r, &inst->a);
    emit2(s, op, r, mach_preg(RCX, 8));
}

/* trunc keeps the low bits with a move of the smaller width. sext uses
   movsx. zext uses movzx, and from 32 bits a 32-bit move, whose result the
   processor zero-extends to 64 bits. */
static void emit_convert(struct selector *s, const struct ir_inst *inst)
{
    struct mach_operand r = select_result(s, inst);
    uint8_t from = width(inst->a.type);

    if (inst->a.kind == IR_INT) {
        load(s, r, inst->op == IR_SEXT
                       ? (uint64_t)signed_value(inst->a.as.integer, from)
                       : inst->a.as.integer);
    } else if (inst->op == IR_TRUNC) {
        move(s, r, widened(select_reg(s, &inst->a), r.width));
    } else if (inst->op == IR_ZEXT && from == 32) {
        emit2(s, X64_MOVL, widened(r, 32), select_reg(s, &inst->a));
    } else {
        emit2(s, inst->op == IR_SEXT ? X64_MOVSX : X64_MOVZX, r,
              select_reg(s, &inst->a));
    }
}

static bool match_convert(const struct selector *s, const struct ir_inst *inst)
{
    (void)s;
    return is_int_type(inst->type) && is_int_type(inst->a.type);
}

/* The address of a function or a global, relative to rip. A C function
   under System V loads its address from the GOT entry. */
static void emit_addr(struct selector *s, const struct ir_inst *inst)
{
    struct mach_operand symbol = mach_imm(inst->a.as.index);

    symbol.kind = inst->a.kind == IR_FUNC ? MACH_FUNC : MACH_GLOBAL;
    symbol.pc_relative = true;
    symbol.got = select_uses_got(s, &inst->a);
    emit2(s, symbol.got ? X64_MOV : X64_LEA, select_result(s, inst), symbol);
}

/* Floats */

static struct mach_operand cond(enum mach_cond c);

static bool match_float(const struct selector *s, const struct ir_inst *inst)
{
    (void)s;
    return select_is_float(inst->type);
}

static uint64_t float_bits(enum ir_type type, double value)
{
    uint64_t pattern;

    if (type == IR_F32) {
        float narrow = (float)value;
        uint32_t word;
        memcpy(&word, &narrow, sizeof word);
        return word;
    }
    memcpy(&pattern, &value, sizeof pattern);
    return pattern;
}

/* A float constant has no immediate form. Its bits go into an integer
   register and from there with movq or movd into the xmm register. */
static void load_float(struct selector *s, struct mach_operand dst,
                       enum ir_type type, double value)
{
    struct mach_operand bits = select_new_vreg(s, width(type));

    load(s, bits, float_bits(type, value));
    emit2(s, X64_MOVQX, dst, bits);
}

static void emit_float_copy(struct selector *s, const struct ir_inst *inst)
{
    move_float(s, select_result(s, inst), select_reg(s, &inst->a));
}

/* r = a op b in two-operand form, as two_operand does for integers. */
static void emit_float_binary(struct selector *s, const struct ir_inst *inst)
{
    enum x64_op op = inst->op == IR_FADD   ? X64_ADDS
                     : inst->op == IR_FSUB ? X64_SUBS
                     : inst->op == IR_FMUL ? X64_MULS
                                           : X64_DIVS;
    bool commutative = op == X64_ADDS || op == X64_MULS;
    struct mach_operand r = select_result(s, inst);
    struct mach_operand b;

    if (inst->b.kind == IR_TEMP && inst->b.as.temp == inst->result) {
        if (commutative) {
            emit2(s, op, r, select_reg(s, &inst->a));
            return;
        }
        b = select_new_fp_vreg(s, r.width);
        move_float(s, b, select_reg(s, &inst->b));
    } else {
        b = select_reg(s, &inst->b);
    }
    if (inst->a.kind != IR_TEMP || inst->a.as.temp != inst->result) {
        move_float(s, r, select_reg(s, &inst->a));
    }
    emit2(s, op, r, b);
}

/* SSE has no negation. xorpd flips the sign bit with a mask, which
   arrives through an integer register like a constant. */
static void emit_float_neg(struct selector *s, const struct ir_inst *inst)
{
    struct mach_operand r = select_result(s, inst);
    struct mach_operand bits = select_new_vreg(s, r.width);
    struct mach_operand mask = select_new_fp_vreg(s, r.width);

    load(s, bits, (uint64_t)1 << (r.width - 1));
    emit2(s, X64_MOVQX, mask, bits);
    if (inst->a.kind != IR_TEMP || inst->a.as.temp != inst->result) {
        move_float(s, r, select_reg(s, &inst->a));
    }
    emit2(s, X64_XORP, r, mask);
}

/* ucomisd sets the flags of an unsigned comparison: CF for below, ZF for
   equal, and ZF, PF and CF together for NaN. a > b and a >= b are the
   conditions a and ae, which NaN fails, and a < b compares b with a.
   Equality needs PF clear as well, and inequality accepts PF set. */
static void emit_float_compare(struct selector *s, const struct ir_inst *inst)
{
    struct mach_operand r = select_result(s, inst);
    bool swap = inst->op == IR_FLT || inst->op == IR_FLE;
    struct mach_operand a = select_reg(s, swap ? &inst->b : &inst->a);
    struct mach_operand b = select_reg(s, swap ? &inst->a : &inst->b);
    struct mach_operand parity;

    emit2(s, X64_UCOMIS, a, b);
    switch (inst->op) {
    case IR_FGT:
    case IR_FLT:
        emit2(s, X64_SET, r, cond(COND_HI));
        break;
    case IR_FGE:
    case IR_FLE:
        emit2(s, X64_SET, r, cond(COND_HS));
        break;
    default:
        parity = select_new_vreg(s, 8);
        emit2(s, X64_SET, r, cond(inst->op == IR_FEQ ? COND_EQ : COND_NE));
        emit2(s, X64_SET, parity, cond(inst->op == IR_FEQ ? COND_NP : COND_P));
        emit2(s, inst->op == IR_FEQ ? X64_AND : X64_OR, r, parity);
        break;
    }
}


/* A u64 of 2^63 or more has no signed conversion. The value (a >> 1) |
   (a & 1) converts and doubles. The sign bit of a, spread into a mask,
   picks between that result and the plain signed conversion. */
static void unsigned_to_float(struct selector *s, struct mach_operand r,
                              struct mach_operand a)
{
    struct mach_operand half = select_new_vreg(s, 64);
    struct mach_operand low = select_new_vreg(s, 32);
    struct mach_operand large = select_new_fp_vreg(s, r.width);
    struct mach_operand small = select_new_fp_vreg(s, r.width);
    struct mach_operand mask = select_new_fp_vreg(s, r.width);
    struct mach_operand sign = select_new_vreg(s, 64);

    move(s, half, a);
    emit2(s, X64_SHR, half, mach_imm(1));
    move(s, low, widened(a, 32));
    emit2(s, X64_AND, low, mach_imm(1));
    emit2(s, X64_OR, half, widened(low, 64));
    emit2(s, X64_CVTSI2S, large, half);
    emit2(s, X64_ADDS, large, large);
    emit2(s, X64_CVTSI2S, small, a);
    move(s, sign, a);
    emit2(s, X64_SAR, sign, mach_imm(63));
    emit2(s, X64_MOVQX, mask, sign);
    emit2(s, X64_ANDP, large, mask);
    emit2(s, X64_ANDNP, mask, small);
    emit2(s, X64_ORP, large, mask);
    move_float(s, r, large);
}

/* A float of 2^63 or more has no signed conversion to u64. It converts
   after subtracting 2^63, the sign bit of the result comes back with xor,
   and cmovae picks that value when the float is not below 2^63. */
static void float_to_unsigned(struct selector *s, struct mach_operand r,
                              struct mach_operand f)
{
    struct mach_operand bits = select_new_vreg(s, f.width);
    struct mach_operand limit = select_new_fp_vreg(s, f.width);
    struct mach_operand reduced = select_new_fp_vreg(s, f.width);
    struct mach_operand high = select_new_vreg(s, 64);
    struct mach_operand sign = select_new_vreg(s, 64);
    struct mach_operand ops[3];

    emit2(s, X64_CVTTS2SI, r, f);
    load(s, bits,
         float_bits(f.width == 64 ? IR_F64 : IR_F32, 9223372036854775808.0));
    emit2(s, X64_MOVQX, limit, bits);
    move_float(s, reduced, f);
    emit2(s, X64_SUBS, reduced, limit);
    emit2(s, X64_CVTTS2SI, high, reduced);
    load(s, sign, (uint64_t)1 << 63);
    emit2(s, X64_XOR, high, sign);
    emit2(s, X64_UCOMIS, f, limit);
    ops[0] = r;
    ops[1] = high;
    ops[2] = cond(COND_HS);
    select_emit(s, X64_CMOV, 3, ops);
}

/* cvtsi2sd converts a signed 32-bit or 64-bit integer and cvttsd2si
   truncates toward zero. Integers of 8 or 16 bits extend to 32 bits and
   a u32 to 64 bits first. A float to an unsigned integer below 64 bits
   converts to the next wider signed integer. */
static void emit_float_convert(struct selector *s, const struct ir_inst *inst)
{
    struct mach_operand r = select_result(s, inst);
    uint8_t from = width(inst->a.type);
    struct mach_operand wide;

    switch (inst->op) {
    case IR_SITOF:
    case IR_UITOF:
        if (inst->op == IR_UITOF && from == 64) {
            unsigned_to_float(s, r, select_reg(s, &inst->a));
        } else if (inst->op == IR_UITOF && from == 32) {
            wide = select_new_vreg(s, 64);
            emit2(s, X64_MOVL, widened(wide, 32), select_reg(s, &inst->a));
            emit2(s, X64_CVTSI2S, r, wide);
        } else if (from < 32) {
            wide = select_new_vreg(s, 32);
            extend_into(s, wide, &inst->a, inst->op == IR_SITOF);
            emit2(s, X64_CVTSI2S, r, wide);
        } else {
            emit2(s, X64_CVTSI2S, r, select_reg(s, &inst->a));
        }
        break;
    case IR_FTOSI:
    case IR_FTOUI:
        if (inst->op == IR_FTOUI && r.width == 64) {
            float_to_unsigned(s, r, select_reg(s, &inst->a));
        } else if (r.width < 32 || (inst->op == IR_FTOUI && r.width == 32)) {
            wide = select_new_vreg(s, r.width == 32 ? 64 : 32);
            emit2(s, X64_CVTTS2SI, wide, select_reg(s, &inst->a));
            move(s, r, widened(wide, r.width));
        } else {
            emit2(s, X64_CVTTS2SI, r, select_reg(s, &inst->a));
        }
        break;
    default:
        emit2(s, X64_CVTS2S, r, select_reg(s, &inst->a));
        break;
    }
}

static void emit_unary(struct selector *s, const struct ir_inst *inst)
{
    struct mach_operand r = select_result(s, inst);

    load_into(s, r, &inst->a);
    emit1(s, inst->op == IR_NEG ? X64_NEG : X64_NOT, r);
}

static void compare(struct selector *s, const struct ir_inst *inst)
{
    struct mach_operand a = select_reg(s, &inst->a);

    if (fits_imm(&inst->b, a.width)) {
        emit2(s, X64_CMP, a, mach_imm(signed_value(inst->b.as.integer,
                                                   a.width)));
    } else {
        emit2(s, X64_CMP, a, select_reg(s, &inst->b));
    }
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
    emit2(s, X64_SET, select_result(s, inst), cond(select_cond(inst->op)));
}

static void jump(struct selector *s, const struct ir_operand *target)
{
    if (!select_is_next(s, target)) {
        emit1(s, X64_JMP, block(target));
    }
}

static void emit_jump(struct selector *s, const struct ir_inst *inst)
{
    jump(s, &inst->a);
}

static void conditional(struct selector *s, const struct ir_inst *inst,
                        enum mach_cond c)
{
    if (select_is_next(s, &inst->b)) {
        emit2(s, X64_J, cond(select_negate(c)), block(&inst->c));
        return;
    }
    emit2(s, X64_J, cond(c), block(&inst->b));
    jump(s, &inst->c);
}

static void emit_fused_branch(struct selector *s, const struct ir_inst *inst)
{
    compare(s, s->fused);
    conditional(s, inst, select_cond(s->fused->op));
}

static void emit_branch(struct selector *s, const struct ir_inst *inst)
{
    struct mach_operand c = select_reg(s, &inst->a);

    emit2(s, X64_TEST, c, c);
    conditional(s, inst, COND_NE);
}

static struct mach_operand stack(int64_t offset);
static void copy_memory(struct selector *s, struct mach_operand dst,
                        struct mach_operand src, uint64_t size);
static void slot_address(struct selector *s, struct mach_operand dst,
                         uint32_t slot);

/* The eightbytes of a System V aggregate of at most 16 bytes. Each one
   goes to an integer register unless all of its members are floats. An
   eightbyte without a member is NO_CLASS and takes no register. Returns
   false for a larger aggregate or one with a field off its alignment,
   whose class is MEMORY. */
static bool sysv_classify(const struct layout *agg, struct arg_location *loc,
                          bool fp[2])
{
    size_t count = (size_t)(agg->size + 7) / 8;
    size_t parts = 0;
    size_t k;
    size_t i;

    if (agg->size > 16 || agg->size == 0 || agg->unaligned) {
        return false;
    }
    for (k = 0; k < count; k++) {
        bool used = false;
        fp[parts] = true;
        for (i = 0; i < agg->member_count; i++) {
            if (agg->members[i].offset / 8 == k) {
                used = true;
                fp[parts] = fp[parts] && select_is_float(agg->members[i].type);
            }
        }
        if (!used) {
            continue;
        }
        loc->parts[parts].offset = (uint8_t)(8 * k);
        loc->parts[parts].bytes = (uint8_t)(agg->size - 8 * k < 8
                                                ? agg->size - 8 * k
                                                : 8);
        parts++;
    }
    loc->part_count = parts;
    return true;
}

/* Windows passes an aggregate of 1, 2, 4 or 8 bytes like an integer of
   that size, and any other aggregate as a pointer to a copy. */
static bool windows_by_value(const struct layout *agg)
{
    return agg->size == 1 || agg->size == 2 || agg->size == 4 ||
           agg->size == 8;
}

static void locate_result(const struct selector *s,
                          const struct ir_function *f,
                          struct arg_location *out)
{
    bool fp[2];
    size_t ints = 0;
    size_t floats = 0;
    size_t k;

    memset(out, 0, sizeof *out);
    out->copy = -1;
    out->reg = select_is_float(f->result) ? s->abi->fp_result
                                          : s->abi->int_result;
    if (f->result != IR_AGG) {
        return;
    }
    if (s->abi == &sysv && sysv_classify(select_layout(s, f->result_agg), out, fp)) {
        static const uint8_t int_results[] = {RAX, RDX};
        static const uint8_t fp_results[] = {XMM0, XMM1};
        for (k = 0; k < out->part_count; k++) {
            out->parts[k].reg = fp[k] ? fp_results[floats++]
                                      : int_results[ints++];
        }
        return;
    }
    if (s->abi == &windows && windows_by_value(select_layout(s, f->result_agg))) {
        out->part_count = 1;
        out->parts[0].reg = RAX;
        out->parts[0].bytes = (uint8_t)select_layout(s, f->result_agg)->size;
        out->parts[0].offset = 0;
        return;
    }
    memset(out, 0, sizeof *out);
    out->indirect = true;
    out->reg = s->abi == &sysv ? RDI : RCX;
    out->copy = RAX;
}

/* DESIGN: System V counts integer and float arguments separately, and
   every argument after its registers goes to the stack in 8 bytes, the
   first one lowest. An aggregate of class MEMORY, or one whose eightbytes
   find no registers, goes to the stack as a whole. Windows gives argument
   i the register at position i of its class and the stack above the
   shadow store from the fifth on. A memory result takes the first integer
   register on both. */
static void locate(const struct selector *s, const struct ir_function *callee,
                   const enum ir_type *types, size_t count,
                   struct arg_location *out)
{
    const struct abi *a = s->abi;
    struct arg_location result;
    size_t ints = 0;
    size_t floats = 0;
    int64_t offset = a->shadow_space;
    size_t i;
    size_t k;

    locate_result(s, callee, &result);
    if (result.indirect) {
        ints = 1;
    }
    for (i = 0; i < count; i++) {
        bool fp = select_is_float(types[i]);
        size_t position = i + ints * (a == &windows);
        const struct layout *agg =
            i < callee->param_count ? select_layout(s, callee->params[i].agg) : NULL;
        bool reg_fp[2];
        memset(&out[i], 0, sizeof out[i]);
        out[i].copy = -1;
        if (types[i] == IR_AGG && a == &sysv &&
            sysv_classify(agg, &out[i], reg_fp)) {
            size_t need_fp = 0;
            for (k = 0; k < out[i].part_count; k++) {
                need_fp += reg_fp[k];
            }
            if (ints + out[i].part_count - need_fp <= a->int_arg_count &&
                floats + need_fp <= a->fp_arg_count) {
                for (k = 0; k < out[i].part_count; k++) {
                    out[i].parts[k].reg = reg_fp[k] ? a->fp_args[floats++]
                                                    : a->int_args[ints++];
                }
                continue;
            }
            out[i].part_count = 0;
        }
        if (types[i] == IR_AGG && a == &sysv) {
            out[i].stack = true;
            out[i].size = agg->size;
            offset = (offset + 7) / 8 * 8;
            out[i].offset = offset;
            offset += (int64_t)(agg->size + 7) / 8 * 8;
        } else if (types[i] == IR_AGG && position < a->int_arg_count &&
                   windows_by_value(agg)) {
            out[i].part_count = 1;
            out[i].parts[0].reg = a->int_args[position];
            out[i].parts[0].bytes = (uint8_t)agg->size;
        } else if (types[i] == IR_AGG && windows_by_value(agg)) {
            out[i].stack = true;
            out[i].size = agg->size;
            out[i].offset = offset;
            offset += 8;
        } else if (types[i] == IR_AGG && position < a->int_arg_count) {
            out[i].indirect = true;
            out[i].reg = a->int_args[position];
        } else if (types[i] == IR_AGG) {
            out[i].indirect = true;
            out[i].stack = true;
            out[i].offset = offset;
            offset += 8;
        } else if (a == &windows && position < a->int_arg_count) {
            out[i].reg = fp ? a->fp_args[position] : a->int_args[position];
            if (fp && i >= callee->param_count) {
                out[i].copy = a->int_args[position];
            }
        } else if (a == &sysv && fp && floats < a->fp_arg_count) {
            out[i].reg = a->fp_args[floats++];
        } else if (a == &sysv && !fp && ints < a->int_arg_count) {
            out[i].reg = a->int_args[ints++];
        } else {
            out[i].stack = true;
            out[i].offset = offset;
            offset += 8;
        }
    }
}

/* A value into the register of its class, a float constant through an
   integer register. */
static void load_value(struct selector *s, struct mach_operand reg,
                       const struct ir_operand *value)
{
    if (value->kind == IR_INT) {
        load(s, reg, value->as.integer);
    } else if (select_is_float(value->type)) {
        move_float(s, reg, select_reg(s, value));
    } else {
        move(s, reg, select_reg(s, value));
    }
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
    struct mach_operand place;

    if (loc->indirect) {
        slot_address(s, address, mach_slot_add(s->out, agg->size, 16));
        copy_memory(s, address, value, agg->size);
        if (loc->stack) {
            place = stack(loc->offset);
            emit2(s, X64_MOV, place, address);
        }
        return address;
    }
    emit2(s, X64_LEA, address, stack(loc->offset));
    copy_memory(s, address, value, agg->size);
    return address;
}

/* Stack arguments go to the area at the bottom of the frame, then the
   register arguments into their registers. A variadic call under System V
   sets al to the number of float registers in use. Windows passes a
   variadic float in the integer register of its position as well. */
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
    uint64_t outgoing = s->abi->shadow_space;
    size_t floats = 0;
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
        uint8_t w = width(arg->type);
        struct mach_operand slot = stack(locations[i].offset);
        if (types[i] == IR_AGG && (locations[i].indirect ||
                                   locations[i].stack)) {
            copies[i] = copy_argument(s, &locations[i],
                                      select_layout(s, callee->params[i].agg),
                                      select_reg(s, arg));
        }
        if (types[i] == IR_AGG && locations[i].stack) {
            uint64_t end = (uint64_t)locations[i].offset +
                           (locations[i].indirect
                                ? 8
                                : (select_layout(s, callee->params[i].agg)->size + 7) / 8 * 8);
            if (end > outgoing) {
                outgoing = end;
            }
        }
        if (types[i] == IR_AGG || !locations[i].stack) {
            continue;
        }
        slot.width = w;
        if (select_is_float(arg->type)) {
            move_float(s, slot, select_reg(s, arg));
        } else {
            emit2(s, X64_MOV, slot,
                  fits_imm(arg, w) ? mach_imm(signed_value(arg->as.integer, w))
                                   : select_reg(s, arg));
        }
        if ((uint64_t)locations[i].offset + 8 > outgoing) {
            outgoing = (uint64_t)locations[i].offset + 8;
        }
    }
    if (outgoing > s->out->outgoing) {
        s->out->outgoing = outgoing;
    }
    for (i = 0; i < inst->arg_count; i++) {
        const struct ir_operand *arg = &inst->args[i];
        struct mach_operand reg = mach_preg(locations[i].reg, width(arg->type));
        if (locations[i].stack) {
            continue;
        }
        if (types[i] == IR_AGG && locations[i].indirect) {
            move(s, mach_preg(locations[i].reg, 64), copies[i]);
            uses |= BIT(locations[i].reg);
            continue;
        }
        if (types[i] == IR_AGG) {
            uses |= select_load_parts(s, &locations[i], select_reg(s, arg));
            continue;
        }
        /* DESIGN: clang callees on System V read an 8-bit or 16-bit
           register argument as 32 bits, so the caller extends it. */
        if (s->abi == &sysv && i < callee->param_count &&
            callee->params[i].ext != IR_EXT_NONE) {
            extend_into(s, mach_preg(locations[i].reg, 32), arg,
                        callee->params[i].ext == IR_EXT_SIGN);
        } else {
            load_value(s, reg, arg);
        }
        uses |= BIT(locations[i].reg);
        if (locations[i].reg >= XMM0) {
            floats++;
        }
        if (locations[i].copy >= 0) {
            emit2(s, X64_MOVQX, mach_preg((uint32_t)locations[i].copy, 64),
                  mach_preg(locations[i].reg, 64));
            uses |= BIT(locations[i].copy);
        }
    }
    if (callee->result == IR_AGG && result.indirect) {
        move(s, mach_preg(result.reg, 64), result_address);
        uses |= BIT(result.reg);
    }
    if (callee->variadic && s->abi == &sysv) {
        load(s, mach_preg(RAX, 32), floats);
        uses |= BIT(RAX);
    }
    free(types);
    free(locations);
    free(copies);
    f.kind = MACH_FUNC;
    call = emit1(s, indirect ? X64_CALLR : X64_CALL, indirect ? target : f);
    call->uses = uses;
    call->defs = s->abi->caller_saved;
    if (inst->result != IR_NO_RESULT && callee->result == IR_AGG) {
        if (!result.indirect) {
            select_store_parts(s, &result,
                               select_result_slot(s, inst,
                                                  select_layout(s, callee->result_agg)));
        }
    } else if (inst->result != IR_NO_RESULT && select_is_float(inst->type)) {
        move_float(s, select_result(s, inst),
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
            move(s, mach_preg((uint32_t)loc.copy, 64), s->result_address);
            uses = BIT(loc.copy);
        } else {
            uses = select_load_parts(s, &loc, select_reg(s, &inst->a));
        }
    } else if (inst->type != IR_VOID) {
        load_value(s, mach_preg(result, width(inst->type)), &inst->a);
        uses = BIT(result);
    }
    ret = select_emit(s, X64_RET, 0, NULL);
    ret->uses = uses;
}

static struct mach_operand memory(struct mach_operand base, enum ir_type type)
{
    struct mach_operand m = base;

    m.kind = MACH_MEM;
    m.base_vreg = base.kind == MACH_VREG;
    m.width = width(type);
    m.value = 0;
    return m;
}

static bool match_scalar(const struct selector *s, const struct ir_inst *inst)
{
    enum ir_type type = inst->op == IR_STORE ? inst->a.type : inst->type;

    (void)s;
    return type != IR_AGG;
}

/* A slot is a memory operand on rsp that frame layout fills in, and lea
   takes its address. */
static void emit_slot(struct selector *s, const struct ir_inst *inst)
{
    struct mach_operand slot = mach_imm(mach_slot_add(s->out, select_size(s, inst->of),
                                                      select_align(s, inst->of)));

    slot.kind = MACH_SLOT;
    emit2(s, X64_LEA, select_result(s, inst), slot);
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

/* A displacement holds 32 bits and a scale is 1, 2, 4 or 8. A store with
   an index takes only an immediate value, so that no instruction reads
   more than two registers. */
static bool fits_address(const struct selector *s, const struct address *a,
                         const struct ir_inst *use)
{
    (void)s;
    if (a->offset < INT32_MIN || a->offset > INT32_MAX) {
        return false;
    }
    if (a->index != NULL && use->op == IR_STORE) {
        return fits_imm(&use->a, width(use->a.type));
    }
    return true;
}

static void emit_load(struct selector *s, const struct ir_inst *inst)
{
    emit2(s, select_is_float(inst->type) ? X64_MOVS : X64_MOV,
          select_result(s, inst), address_of(s, &inst->a, inst->type));
}

/* A constant that fits the immediate is stored without a register. */
static void emit_store(struct selector *s, const struct ir_inst *inst)
{
    uint8_t w = width(inst->a.type);
    bool fp = select_is_float(inst->a.type);
    struct mach_operand value =
        fits_imm(&inst->a, w) ? mach_imm(signed_value(inst->a.as.integer, w))
                              : select_reg(s, &inst->a);

    emit2(s, fp ? X64_MOVS : X64_MOV, address_of(s, &inst->b, inst->a.type),
          value);
}

/* A parameter on the stack sits above the saved rbp and the return
   address, at the offset that locate gives it in the argument area. */
static void stack_param(struct selector *s, int64_t offset,
                        struct mach_operand dst)
{
    struct mach_operand m = mach_preg(RBP, 64);

    m.kind = MACH_MEM;
    m.width = dst.width;
    m.value = 16 + offset;
    s->out->stack_params = true;
    emit2(s, s->out->fp[dst.reg] ? X64_MOVS : X64_MOV, dst, m);
}

/* Memory of width bits at offset after the address in register base. */
static struct mach_operand memory_at(struct mach_operand base, int64_t offset,
                                     uint8_t bits)
{
    struct mach_operand m = base;

    m.kind = MACH_MEM;
    m.base_vreg = base.kind == MACH_VREG;
    m.width = bits;
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
        uses |= BIT(s->abi->int_args[i]);
    }
    if (s->abi->shadow_space > s->out->outgoing) {
        s->out->outgoing = s->abi->shadow_space;
    }
    memset(&f, 0, sizeof f);
    f.kind = MACH_NAME;
    f.name = name;
    call = emit1(s, X64_CALL, f);
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
        uint8_t bits = left >= 8 ? 64 : left >= 4 ? 32 : left >= 2 ? 16 : 8;
        struct mach_operand t = select_new_vreg(s, bits);
        emit2(s, X64_MOV, t, memory_at(src, (int64_t)offset, bits));
        emit2(s, X64_MOV, memory_at(dst, (int64_t)offset, bits), t);
        offset += bits / 8;
    }
}

static bool is_float_register(const struct selector *s,
                              struct mach_operand o)
{
    return o.kind == MACH_VREG ? s->out->fp[o.reg] : o.reg >= XMM0;
}

/* Load bytes of an aggregate at offset after base into register dst. An
   integer part of 3, 5, 6 or 7 bytes combines its pieces with shifts. */
static void load_bytes(struct selector *s, struct mach_operand dst,
                       struct mach_operand base, int64_t offset,
                       unsigned bytes)
{
    unsigned low = bytes >= 4 ? 4 : 2;
    struct mach_operand rest;

    if (is_float_register(s, dst)) {
        move_float(s, widened(dst, (uint8_t)(bytes * 8)),
                   memory_at(base, offset, (uint8_t)(bytes * 8)));
    } else if (bytes == 8 || bytes == 4) {
        emit2(s, X64_MOV, widened(dst, (uint8_t)(bytes * 8)),
              memory_at(base, offset, (uint8_t)(bytes * 8)));
    } else if (bytes == 2 || bytes == 1) {
        emit2(s, X64_MOVZX, widened(dst, 32),
              memory_at(base, offset, (uint8_t)(bytes * 8)));
    } else {
        load_bytes(s, dst, base, offset, low);
        rest = select_new_vreg(s, 64);
        load_bytes(s, rest, base, offset + low, bytes - low);
        emit2(s, X64_SHL, rest, mach_imm(8 * low));
        emit2(s, X64_OR, widened(dst, 64), rest);
    }
}

/* Store the low bytes of register src at offset after base. */
static void store_bytes(struct selector *s, struct mach_operand src,
                        struct mach_operand base, int64_t offset,
                        unsigned bytes)
{
    unsigned low = bytes >= 4 ? 4 : 2;
    struct mach_operand rest;

    if (is_float_register(s, src)) {
        move_float(s, memory_at(base, offset, (uint8_t)(bytes * 8)),
                   widened(src, (uint8_t)(bytes * 8)));
    } else if (bytes == 8 || bytes == 4 || bytes == 2 || bytes == 1) {
        emit2(s, X64_MOV, memory_at(base, offset, (uint8_t)(bytes * 8)),
              widened(src, (uint8_t)(bytes * 8)));
    } else {
        store_bytes(s, src, base, offset, low);
        rest = select_new_vreg(s, 64);
        move(s, rest, widened(src, 64));
        emit2(s, X64_SHR, rest, mach_imm(8 * low));
        store_bytes(s, rest, base, offset + low, bytes - low);
    }
}

static void slot_address(struct selector *s, struct mach_operand dst,
                         uint32_t slot)
{
    struct mach_operand o = mach_imm(slot);

    o.kind = MACH_SLOT;
    emit2(s, X64_LEA, widened(dst, 64), o);
}

/* The address of an aggregate in the argument area of the caller. */
static void incoming_address(struct selector *s, struct mach_operand dst,
                             int64_t offset)
{
    s->out->stack_params = true;
    emit2(s, X64_LEA, widened(dst, 64),
          memory_at(mach_preg(RBP, 64), 16 + offset, 64));
}

static void emit_memcopy(struct selector *s, const struct ir_inst *inst)
{
    copy_memory(s, select_reg(s, &inst->a), select_reg(s, &inst->b),
                select_size(s, inst->of));
}

static void emit_ptradd(struct selector *s, const struct ir_inst *inst)
{
    two_operand(s, inst, X64_ADD, true);
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
    {IR_ADD, match_arith, emit_binary},
    {IR_SUB, match_arith, emit_binary},
    {IR_MUL, match_arith, emit_mul},
    {IR_AND, match_arith, emit_binary},
    {IR_OR, match_arith, emit_binary},
    {IR_XOR, match_not_bool, emit_binary},
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
    {IR_PTRADD, NULL, emit_ptradd},
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

/* Printing in AT&T syntax: the source comes before the destination, a
   register has the prefix % and an immediate the prefix $. */

static const char *const names64[] = {
    "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
};
static const char *const names32[] = {
    "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi",
    "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d",
};
static const char *const names16[] = {
    "ax", "cx", "dx", "bx", "sp", "bp", "si", "di",
    "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w",
};
static const char *const names8[] = {
    "al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil",
    "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b",
};

static const char *const cond_names[] = {
    [COND_EQ] = "e", [COND_NE] = "ne", [COND_LT] = "l", [COND_LE] = "le",
    [COND_GT] = "g", [COND_GE] = "ge", [COND_LO] = "b", [COND_LS] = "be",
    [COND_HI] = "a", [COND_HS] = "ae", [COND_P] = "p", [COND_NP] = "np",
};

static const char *const xmm_names[] = {
    "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
    "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15",
};

static void print_register(struct text *out, uint32_t reg, bool vreg)
{
    if (vreg) {
        text_appendf(out, "%%t%" PRIu32, reg);
    } else {
        text_appendf(out, "%%%s", names64[reg]);
    }
}

static void print_operand(struct text *out, const struct ir_module *m,
                          const struct names *names,
                          const struct mach_operand *o)
{

    switch (o->kind) {
    case MACH_VREG:
        text_appendf(out, "%%t%" PRIu32, o->reg);
        break;
    case MACH_PREG:
        text_appendf(out, "%%%s",
                     o->reg >= XMM0   ? xmm_names[o->reg - XMM0]
                     : o->width == 64 ? names64[o->reg]
                     : o->width == 32 ? names32[o->reg]
                     : o->width == 16 ? names16[o->reg]
                                      : names8[o->reg]);
        break;
    case MACH_IMM:
        text_appendf(out, "$%" PRId64, o->value);
        break;
    case MACH_BLOCK:
    case MACH_NAME:
        mach_symbol(out, m, names, o);
        break;
    case MACH_FUNC:
        mach_symbol(out, m, names, o);
        text_append(out, o->got           ? "@GOTPCREL(%rip)"
                         : o->pc_relative ? "(%rip)"
                                          : "");
        break;
    case MACH_SLOT:
        text_appendf(out, "slot%" PRId64, o->value);
        break;
    case MACH_MEM:
        if (o->value != 0) {
            text_appendf(out, "%" PRId64, o->value);
        }
        text_append(out, "(");
        print_register(out, o->reg, o->base_vreg);
        if (o->scale != 0) {
            text_append(out, ",");
            print_register(out, o->index_reg, o->index_vreg);
            text_appendf(out, ",%u", o->scale);
        }
        text_append(out, ")");
        break;
    case MACH_GLOBAL:
        mach_symbol(out, m, names, o);
        text_append(out, "(%rip)");
        break;
    default:
        break;
    }
}

/* sd for a 64-bit float, ss for a 32-bit one. */
static char float_suffix(uint8_t width)
{
    return width == 64 ? 'd' : 's';
}

/* The size suffix comes from the first register or memory operand. */
static char width_suffix(uint8_t width)
{
    return width == 64 ? 'q' : width == 32 ? 'l' : width == 16 ? 'w' : 'b';
}

static char suffix(const struct mach_inst *inst)
{
    size_t i;

    for (i = 0; i < inst->count; i++) {
        const struct mach_operand *o = &inst->operands[i];
        if (o->kind == MACH_VREG || o->kind == MACH_PREG ||
            o->kind == MACH_MEM) {
            return width_suffix(o->width);
        }
    }
    return 'q';
}

static void print(struct text *out, const struct ir_module *m,
                  const struct mach_inst *inst, const struct names *names)
{
    size_t i;
    bool first = true;

    switch (inst->op) {
    case X64_SEH_PUSHREG:
    case X64_SEH_STACKALLOC:
    case X64_SEH_SAVEREG:
    case X64_SEH_SAVEXMM:
    case X64_SEH_ENDPROLOGUE:
        /* A directive takes a register and a number without $. */
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
    case X64_SET:
        text_appendf(out, "set%s ", cond_names[inst->operands[1].value]);
        print_operand(out, m, names, &inst->operands[0]);
        return;
    case X64_J:
        text_appendf(out, "j%s ", cond_names[inst->operands[0].value]);
        print_operand(out, m, names, &inst->operands[1]);
        return;
    case X64_CMOV:
        text_appendf(out, "cmov%s%c ", cond_names[inst->operands[2].value],
                     width_suffix(inst->operands[0].width));
        print_operand(out, m, names, &inst->operands[1]);
        text_append(out, ", ");
        print_operand(out, m, names, &inst->operands[0]);
        return;
    case X64_CALLR:
        text_append(out, "call *");
        print_operand(out, m, names, &inst->operands[0]);
        return;
    case X64_JMP:
    case X64_CALL:
    case X64_RET:
    case X64_CQTO:
    case X64_CLTD:
        text_append(out, opcodes[inst->op].name);
        break;
    case X64_MOVSX:
    case X64_MOVZX:
        /* movsbq and movzwl name the source width and the target width. */
        text_appendf(out, "%s%c%c", opcodes[inst->op].name,
                     width_suffix(inst->operands[1].width),
                     width_suffix(inst->operands[0].width));
        break;
    case X64_MOVS:
    case X64_ADDS:
    case X64_SUBS:
    case X64_MULS:
    case X64_DIVS:
    case X64_UCOMIS:
    case X64_XORP:
    case X64_ANDP:
    case X64_ANDNP:
    case X64_ORP:
        text_appendf(out, "%s%c", opcodes[inst->op].name,
                     float_suffix(inst->operands[0].width));
        break;
    case X64_CVTSI2S:
        /* cvtsi2sdq names the float width and the integer width. */
        text_appendf(out, "%s%c%c", opcodes[inst->op].name,
                     float_suffix(inst->operands[0].width),
                     width_suffix(inst->operands[1].width));
        break;
    case X64_CVTTS2SI:
        text_appendf(out, "%s%c2si", opcodes[inst->op].name,
                     float_suffix(inst->operands[1].width));
        break;
    case X64_CVTS2S:
        text_appendf(out, "%s%c2s%c", opcodes[inst->op].name,
                     float_suffix(inst->operands[1].width),
                     float_suffix(inst->operands[0].width));
        break;
    case X64_MOVQX:
        text_append(out, inst->operands[0].width == 64 ? "movq" : "movd");
        break;
    case X64_MOVUPS:
        text_append(out, opcodes[inst->op].name);
        break;
    default:
        text_appendf(out, "%s%c", opcodes[inst->op].name, suffix(inst));
        break;
    }
    for (i = inst->count; i-- > 0;) {
        text_append(out, first ? " " : ", ");
        print_operand(out, m, names, &inst->operands[i]);
        first = false;
    }
}

/* Frames */

static struct mach_operand stack(int64_t offset)
{
    struct mach_operand m = mach_preg(RSP, 64);

    m.kind = MACH_MEM;
    m.value = offset;
    return m;
}

static void append(struct mach_block *b, enum x64_op op, size_t count,
                   const struct mach_operand *operands);

/* Windows requires a frame of a page or more to probe its pages first.
   __chkstk takes the size in rax and leaves every register but r10 and
   r11 unchanged. */
static void allocate_frame(struct mach_block *b, const struct frame *frame)
{
    struct mach_operand ops[2];
    struct mach_inst *call;

    if (!frame->probe) {
        ops[0] = mach_preg(RSP, 64);
        ops[1] = mach_imm((int64_t)frame->size);
        append(b, X64_SUB, 2, ops);
        return;
    }
    ops[0] = mach_preg(RAX, 32);
    ops[1] = mach_imm((int64_t)frame->size);
    append(b, X64_MOV, 2, ops);
    memset(ops, 0, sizeof ops);
    ops[0].kind = MACH_NAME;
    ops[0].name = "__chkstk";
    append(b, X64_CALL, 1, ops);
    call = &b->insts[b->count - 1];
    call->uses = BIT(RAX);
    ops[0] = mach_preg(RSP, 64);
    ops[1] = mach_preg(RAX, 64);
    append(b, X64_SUB, 2, ops);
}

static void append(struct mach_block *b, enum x64_op op, size_t count,
                   const struct mach_operand *operands)
{
    struct mach_inst *inst = mach_append(b);

    inst->op = (uint16_t)op;
    inst->count = (uint8_t)count;
    memcpy(inst->operands, operands, count * sizeof *operands);
}

/* A spilled float takes 8 bytes and moves with movsd. */
static void load_spill(struct mach_block *b, uint8_t reg, int64_t offset)
{
    struct mach_operand ops[2];

    ops[0] = mach_preg(reg, 64);
    ops[1] = stack(offset);
    append(b, reg >= XMM0 ? X64_MOVS : X64_MOV, 2, ops);
}

static void store_spill(struct mach_block *b, uint8_t reg, int64_t offset)
{
    struct mach_operand ops[2];

    ops[0] = stack(offset);
    ops[1] = mach_preg(reg, 64);
    append(b, reg >= XMM0 ? X64_MOVS : X64_MOV, 2, ops);
}

/* Windows preserves all 128 bits of xmm6 to xmm15, so a callee saves them
   with movups. */
static void save_register(struct mach_block *b, uint8_t reg, int64_t offset,
                          bool restore)
{
    struct mach_operand ops[2];

    if (reg < XMM0) {
        if (restore) {
            load_spill(b, reg, offset);
        } else {
            store_spill(b, reg, offset);
        }
        return;
    }
    ops[restore ? 1 : 0] = stack(offset);
    ops[restore ? 0 : 1] = mach_preg(reg, 64);
    append(b, X64_MOVUPS, 2, ops);
}

static void resolve_slot(struct mach_operand *o, int64_t offset)
{
    *o = stack(offset);
}

/* Append the unwind directive op with count operands to a prologue that
   carries directives. */
static void unwind(struct mach_block *b, const struct frame *frame,
                   enum x64_op op, size_t count,
                   const struct mach_operand *operands)
{
    if (frame->unwind) {
        append(b, op, count, operands);
    }
}

/* rbp points to the saved rbp above the frame. Callee-saved registers
   take the top of the frame, slots its bottom.
   DESIGN: the Windows unwind data names no frame register. With one, each
   save offset would count from rbp, above the saves, and a save offset is
   unsigned. Anti has no alloca, so rsp alone locates the frame. If Anti
   gets dynamic stack allocation, the Windows prologue must set a frame
   register and the epilogue must switch to lea. */
static void prologue(struct mach_block *b, const struct frame *frame)
{
    struct mach_operand ops[2];
    size_t i;

    if (!frame->needed) {
        return;
    }
    ops[0] = mach_preg(RBP, 64);
    append(b, X64_PUSH, 1, ops);
    unwind(b, frame, X64_SEH_PUSHREG, 1, ops);
    ops[1] = mach_preg(RSP, 64);
    append(b, X64_MOV, 2, ops);
    if (frame->size > 0) {
        allocate_frame(b, frame);
        ops[0] = mach_imm((int64_t)frame->size);
        unwind(b, frame, X64_SEH_STACKALLOC, 1, ops);
    }
    for (i = 0; i < frame->saved_count; i++) {
        save_register(b, frame->saved[i], frame->saved_offset[i], false);
        ops[0] = mach_preg(frame->saved[i], 64);
        ops[1] = mach_imm(frame->saved_offset[i]);
        unwind(b, frame,
               frame->saved[i] >= XMM0 ? X64_SEH_SAVEXMM : X64_SEH_SAVEREG,
               2, ops);
    }
    unwind(b, frame, X64_SEH_ENDPROLOGUE, 0, ops);
}

/* DESIGN: Windows x64 unwinding recognises an epilogue by its code. It
   is add rsp, or lea rsp from the frame register of the unwind data, then
   pops and ret. The Windows unwind data names no frame register, so the
   Windows epilogue frees the frame with add. */
static void epilogue(struct mach_block *b, const struct frame *frame)
{
    struct mach_operand ops[2];
    size_t i;

    if (!frame->needed) {
        return;
    }
    for (i = 0; i < frame->saved_count; i++) {
        save_register(b, frame->saved[i], frame->saved_offset[i], true);
    }
    if (frame->size > 0) {
        ops[0] = mach_preg(RSP, 64);
        ops[1] = mach_preg(RBP, 64);
        if (frame->unwind) {
            ops[1] = mach_imm((int64_t)frame->size);
            append(b, X64_ADD, 2, ops);
        } else {
            append(b, X64_MOV, 2, ops);
        }
    }
    ops[0] = mach_preg(RBP, 64);
    append(b, X64_POP, 1, ops);
}

static const struct target_desc desc = {
    .name = "x86_64",
    .chapter = 14,
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
    /* DESIGN: sub and memory displacements hold signed 32 bits. A larger
       frame is an error rather than a movabs sequence, see docs/decisions.md. */
    .frame_limit = 0x7fffffff,
    .load_spill = load_spill,
    .store_spill = store_spill,
    .resolve_slot = resolve_slot,
    .expand = NULL,
    .prologue = prologue,
    .epilogue = epilogue,
};

const struct target_desc *target_desc_x86_64(void)
{
    return &desc;
}
