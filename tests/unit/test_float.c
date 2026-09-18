#include "check.h"
#include <stdlib.h>
#include <string.h>
#include "arena.h"
#include "ast.h"
#include "diagnostic.h"
#include "ir.h"
#include "lexer.h"
#include "lower.h"
#include "optimize.h"
#include "parser.h"
#include "regalloc.h"
#include "select.h"
#include "sema.h"
#include "types.h"

/* Compile source as module main through register allocation for target,
   and append the machine code of every function, or the error. */
static void run(const char *source, enum target target, struct text *out)
{
    struct arena arena = {0};
    struct diagnostics diags = {0};
    struct token_list tokens = {0};
    struct module *module = NULL;
    struct types types;
    struct ir_module ir;
    struct mach_function **functions = NULL;
    char error[200] = "";
    bool ok;
    size_t i;

    types_init(&types, &arena);
    ir_module_init(&ir, &arena, "main");
    if (!lex(source, strlen(source), &arena, &diags, &tokens) ||
        !parse(source, &tokens, &arena, &diags, &module) ||
        !sema_check(module, "main", NULL, NULL, 0, &types, &arena, &diags, true) ||
        !lower_module(module, "main", &ir, &diags, false)) {
        check_failures++;
        fprintf(stderr, "test source does not lower: %s\n%s\n",
                diags.count > 0 ? diags.items[0].message : "", source);
    } else {
        ir_optimize(&ir, "main");
        functions = calloc(ir.function_count + 1, sizeof *functions);
        ok = select_module(target, &ir, functions, error, sizeof error);
        for (i = 0; ok && i < ir.function_count; i++) {
            if (functions[i] != NULL) {
                ok = regalloc_function(target, functions[i], error,
                                       sizeof error);
            }
        }
        for (i = 0; ok && i < ir.function_count; i++) {
            if (functions[i] != NULL) {
                mach_print(out, target_desc(target), &ir, functions[i]);
            }
        }
        if (!ok) {
            text_append(out, error);
        }
        for (i = 0; i < ir.function_count; i++) {
            if (functions[i] != NULL) {
                mach_function_free(functions[i]);
                free(functions[i]);
            }
        }
        free(functions);
    }
    ir_module_free(&ir);
    token_list_free(&tokens);
    diagnostics_free(&diags);
    arena_free(&arena);
}

static void emits(const char *source, enum target target,
                  const char *expected)
{
    struct text out = {0};

    run(source, target, &out);
    CHECK_STR(text_cstr(&out), expected);
    text_free(&out);
}

static const char add64[] = "fn add(a: f64, b: f64) -> f64 {\n"
                            "    return a + b;\n"
                            "}\n";

static const char add32[] = "fn add(a: f32, b: f32) -> f32 {\n"
                            "    return a + b;\n"
                            "}\n";

/* A spilled float and a spilled pointer beyond the scaled offset of 32760
   bytes: the float loads through x16 before x16 receives the pointer. */
static void far_spills(void)
{
    struct text source = {0};
    struct text out = {0};
    int i;

    text_append(&source,
                "extern fn take(p: *int);\n"
                "fn big(p: *f64, x0: f64, x1: f64, x2: f64, x3: f64, x4: f64,\n"
                "       x5: f64, x6: f64, x7: f64, x8: f64) -> f64 {\n");
    for (i = 0; i < 4100; i++) {
        text_appendf(&source, "    let a%d = %d;\n    take(&a%d);\n", i, i, i);
    }
    text_append(&source, "    let s = x0 + x1 + x2 + x3 + x4 + x5 + x6 + x7;\n"
                         "    *p = x8;\n"
                         "    return s;\n"
                         "}\n");
    run(text_cstr(&source), TARGET_LINUX_ARM64, &out);
    CHECK(strstr(text_cstr(&out), "    mov x16, #32800\n"
                                  "    ldr d16, [sp, x16]\n"
                                  "    mov x16, #32808\n"
                                  "    ldr x16, [sp, x16]\n"
                                  "    str d16, [x16]\n") != NULL);
    text_free(&source);
    text_free(&out);
}

static const char keep[] = "extern fn tick() -> int;\n"
                           "fn keep(a: f64) -> f64 {\n"
                           "    tick();\n"
                           "    return -a;\n"
                           "}\n";

static const char memory[] = "fn halve(p: *f32) {\n"
                             "    *p = *p * 0.5;\n"
                             "}\n";

static const char compares[] = "fn lt(a: f64, b: f64) -> bool {\n"
                               "    return a < b;\n"
                               "}\n"
                               "fn eq(a: f32, b: f32) -> bool {\n"
                               "    return a == b;\n"
                               "}\n"
                               "fn ne(a: f64, b: f64) -> bool {\n"
                               "    return a != b;\n"
                               "}\n";

static const char conversions[] = "fn s2f(a: i8) -> f32 {\n"
                                  "    return a as f32;\n"
                                  "}\n"
                                  "fn u2f(a: u64) -> f64 {\n"
                                  "    return a as f64;\n"
                                  "}\n"
                                  "fn f2s(a: f64) -> i32 {\n"
                                  "    return a as i32;\n"
                                  "}\n"
                                  "fn f2u(a: f32) -> u64 {\n"
                                  "    return a as u64;\n"
                                  "}\n"
                                  "fn widen(a: f32) -> f64 {\n"
                                  "    return a as f64;\n"
                                  "}\n";

void test_float(void)
{
    far_spills();
    /* Float parameters arrive in xmm0 and xmm1, and the result leaves in
       xmm0. */
    emits(add64, TARGET_LINUX_X86_64,
          "main.add:\n"
          "b0:\n"
          "    addsd %xmm1, %xmm0\n"
          "    ret\n");
    emits(add32, TARGET_WINDOWS_X86_64,
          "main.add:\n"
          "b0:\n"
          "    addss %xmm1, %xmm0\n"
          "    ret\n");
    /* ARM64 uses d0 and d1, or s0 and s1 for 32 bits. */
    emits(add64, TARGET_MACOS_ARM64,
          "main.add:\n"
          "b0:\n"
          "    fadd d0, d0, d1\n"
          "    ret\n");
    emits(add32, TARGET_LINUX_ARM64,
          "main.add:\n"
          "b0:\n"
          "    fadd s0, s0, s1\n"
          "    ret\n");

    /* Negation: fneg on ARM64, xorpd with the sign bit on x86_64. A float
       kept across a call lives in d8 on ARM64 and xmm6 on Windows, and
       System V, which preserves no xmm register, spills it. */
    emits(keep, TARGET_MACOS_ARM64,
          "main.keep:\n"
          "b0:\n"
          "    stp x29, x30, [sp, #-16]!\n"
          "    mov x29, sp\n"
          "    sub sp, sp, #16\n"
          "    str d8, [sp, #8]\n"
          "    fmov d8, d0\n"
          "    bl tick\n"
          "    fneg d0, d8\n"
          "    ldr d8, [sp, #8]\n"
          "    mov sp, x29\n"
          "    ldp x29, x30, [sp], #16\n"
          "    ret\n");
    emits(keep, TARGET_WINDOWS_X86_64,
          "main.keep:\n"
          "b0:\n"
          "    pushq %rbp\n"
          "    .seh_pushreg %rbp\n"
          "    movq %rsp, %rbp\n"
          "    subq $48, %rsp\n"
          "    .seh_stackalloc 48\n"
          "    movups %xmm6, 32(%rsp)\n"
          "    .seh_savexmm %xmm6, 32\n"
          "    .seh_endprologue\n"
          "    movsd %xmm0, %xmm6\n"
          "    call tick\n"
          "    movabsq $-9223372036854775808, %rax\n"
          "    movq %rax, %xmm0\n"
          "    movsd %xmm6, %xmm1\n"
          "    xorpd %xmm0, %xmm1\n"
          "    movsd %xmm1, %xmm0\n"
          "    movups 32(%rsp), %xmm6\n"
          "    addq $48, %rsp\n"
          "    popq %rbp\n"
          "    ret\n");
    emits(keep, TARGET_LINUX_X86_64,
          "main.keep:\n"
          "b0:\n"
          "    pushq %rbp\n"
          "    movq %rsp, %rbp\n"
          "    subq $16, %rsp\n"
          "    movsd %xmm0, %xmm14\n"
          "    movsd %xmm14, (%rsp)\n"
          "    call tick\n"
          "    movabsq $-9223372036854775808, %rax\n"
          "    movq %rax, %xmm0\n"
          "    movsd (%rsp), %xmm14\n"
          "    movsd %xmm14, %xmm1\n"
          "    xorpd %xmm0, %xmm1\n"
          "    movsd %xmm1, %xmm0\n"
          "    movq %rbp, %rsp\n"
          "    popq %rbp\n"
          "    ret\n");

    /* A float constant goes in as its bits, and loads and stores move
       floats to and from memory. */
    emits(memory, TARGET_LINUX_ARM64,
          "main.halve:\n"
          "b0:\n"
          "    ldr s18, [x0]\n"
          "    movz w9, #16128, lsl #16\n"
          "    fmov s19, w9\n"
          "    fmul s18, s18, s19\n"
          "    str s18, [x0]\n"
          "    ret\n");
    emits(memory, TARGET_LINUX_X86_64,
          "main.halve:\n"
          "b0:\n"
          "    movss (%rdi), %xmm0\n"
          "    movl $1056964608, %eax\n"
          "    movd %eax, %xmm1\n"
          "    mulss %xmm1, %xmm0\n"
          "    movss %xmm0, (%rdi)\n"
          "    ret\n");

    /* ucomisd sets CF and ZF like an unsigned comparison and all three of
       ZF, PF and CF for NaN. a < b tests b above a, and equality also
       needs PF clear. */
    emits(compares, TARGET_LINUX_X86_64,
          "main.lt:\n"
          "b0:\n"
          "    ucomisd %xmm0, %xmm1\n"
          "    seta %al\n"
          "    ret\n"
          "main.eq:\n"
          "b0:\n"
          "    ucomiss %xmm1, %xmm0\n"
          "    sete %al\n"
          "    setnp %cl\n"
          "    andb %cl, %al\n"
          "    ret\n"
          "main.ne:\n"
          "b0:\n"
          "    ucomisd %xmm1, %xmm0\n"
          "    setne %al\n"
          "    setp %cl\n"
          "    orb %cl, %al\n"
          "    ret\n");
    /* fcmp sets N for less, Z for equal and C and V for NaN. mi, ls, gt
       and ge are false for NaN, and ne is true. */
    emits(compares, TARGET_MACOS_ARM64,
          "main.lt:\n"
          "b0:\n"
          "    fcmp d0, d1\n"
          "    cset w0, mi\n"
          "    ret\n"
          "main.eq:\n"
          "b0:\n"
          "    fcmp s0, s1\n"
          "    cset w0, eq\n"
          "    ret\n"
          "main.ne:\n"
          "b0:\n"
          "    fcmp d0, d1\n"
          "    cset w0, ne\n"
          "    ret\n");

    /* ARM64 converts with one instruction each, after extending 8-bit
       and 16-bit integers. */
    emits(conversions, TARGET_LINUX_ARM64,
          "main.s2f:\n"
          "b0:\n"
          "    sxtb w9, w0\n"
          "    scvtf s0, w9\n"
          "    ret\n"
          "main.u2f:\n"
          "b0:\n"
          "    ucvtf d0, x0\n"
          "    ret\n"
          "main.f2s:\n"
          "b0:\n"
          "    fcvtzs w0, d0\n"
          "    ret\n"
          "main.f2u:\n"
          "b0:\n"
          "    fcvtzu x0, s0\n"
          "    ret\n"
          "main.widen:\n"
          "b0:\n"
          "    fcvt d0, s0\n"
          "    ret\n");
    /* SSE2 converts only signed integers. An unsigned 64-bit value above
       2^63 converts from its half, and a mask from its sign bit picks the
       result. A float of 2^63 or more converts after subtracting 2^63,
       and cmovae picks that result. */
    emits(conversions, TARGET_LINUX_X86_64,
          "main.s2f:\n"
          "b0:\n"
          "    movsbl %dil, %eax\n"
          "    cvtsi2ssl %eax, %xmm0\n"
          "    ret\n"
          "main.u2f:\n"
          "b0:\n"
          "    movq %rdi, %rax\n"
          "    shrq $1, %rax\n"
          "    movl %edi, %ecx\n"
          "    andl $1, %ecx\n"
          "    orq %rcx, %rax\n"
          "    cvtsi2sdq %rax, %xmm0\n"
          "    addsd %xmm0, %xmm0\n"
          "    cvtsi2sdq %rdi, %xmm1\n"
          "    sarq $63, %rdi\n"
          "    movq %rdi, %xmm2\n"
          "    andpd %xmm2, %xmm0\n"
          "    andnpd %xmm1, %xmm2\n"
          "    orpd %xmm2, %xmm0\n"
          "    ret\n"
          "main.f2s:\n"
          "b0:\n"
          "    cvttsd2si %xmm0, %eax\n"
          "    ret\n"
          "main.f2u:\n"
          "b0:\n"
          "    cvttss2si %xmm0, %rax\n"
          "    movl $1593835520, %ecx\n"
          "    movd %ecx, %xmm1\n"
          "    movss %xmm0, %xmm2\n"
          "    subss %xmm1, %xmm2\n"
          "    cvttss2si %xmm2, %rcx\n"
          "    movabsq $-9223372036854775808, %rdx\n"
          "    xorq %rdx, %rcx\n"
          "    ucomiss %xmm1, %xmm0\n"
          "    cmovaeq %rcx, %rax\n"
          "    ret\n"
          "main.widen:\n"
          "b0:\n"
          "    cvtss2sd %xmm0, %xmm0\n"
          "    ret\n");

    /* A u32 converts as a 64-bit integer after movl clears the upper
       bits of its register. */
    emits("fn u32f(a: u32) -> f64 {\n"
          "    return a as f64;\n"
          "}\n",
          TARGET_LINUX_X86_64,
          "main.u32f:\n"
          "b0:\n"
          "    movl %edi, %eax\n"
          "    cvtsi2sdq %rax, %xmm0\n"
          "    ret\n");
}
