#include "check.h"
#include <stdlib.h>
#include "arena.h"
#include "ast.h"
#include "diagnostic.h"
#include "ir.h"
#include "lexer.h"
#include "lower.h"
#include "optimize.h"
#include "parser.h"
#include "select.h"
#include "sema.h"
#include "types.h"

/* Compile source as module main, optimize it and select code for target.
   Returns the printed machine code of all functions, or the error. */
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
    size_t i;

    types_init(&types, &arena);
    ir_module_init(&ir, &arena, "main");
    if (!lex(source, strlen(source), &arena, &diags, &tokens) ||
        !parse(source, &tokens, &arena, &diags, &module) ||
        !sema_check(module, "main", NULL, 0, &types, &arena, &diags) ||
        !lower_module(module, "main", &ir, &diags)) {
        check_failures++;
        fprintf(stderr, "test source does not lower: %s\n%s\n",
                diags.count > 0 ? diags.items[0].message : "", source);
    } else {
        ir_optimize(&ir, "main");
        functions = calloc(ir.function_count + 1, sizeof *functions);
        if (select_module(target, &ir, functions, error, sizeof error)) {
            for (i = 0; i < ir.function_count; i++) {
                if (functions[i] != NULL) {
                    mach_print(out, target_desc(target), &ir, functions[i]);
                }
            }
        } else {
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

static void selects(const char *source, enum target target,
                    const char *expected)
{
    struct text out = {0};

    run(source, target, &out);
    CHECK_STR(text_cstr(&out), expected);
    text_free(&out);
}

static const char scale[] = "fn scale(x: int) -> int {\n"
                            "    let k = 2 + 4;\n"
                            "    return x * k;\n"
                            "}\n"
                            "fn main() -> int {\n"
                            "    return scale(7);\n"
                            "}\n";

static const char loop[] = "fn g(n: int) -> int {\n"
                           "    let i = 0;\n"
                           "    while i < n do {\n"
                           "        i += 1;\n"
                           "    }\n"
                           "    if i > 3 {\n"
                           "        return 1;\n"
                           "    } else {\n"
                           "        return 0;\n"
                           "    }\n"
                           "}\n";

static const char ops[] = "extern fn putchar(c: i32) -> i32;\n"
                          "fn arith(x: int, y: int) -> int {\n"
                          "    let big = x + 5000000000;\n"
                          "    let neg = y - -7;\n"
                          "    return -big * ~neg;\n"
                          "}\n"
                          "fn bits(m: i32) -> i32 {\n"
                          "    return (m & -2) | (m ^ 65536);\n"
                          "}\n"
                          "fn flip(n: int) -> int {\n"
                          "    let i = n;\n"
                          "    while i > 0 do {\n"
                          "        i = 10 - i;\n"
                          "    }\n"
                          "    return i;\n"
                          "}\n"
                          "fn calls(flag: bool, n: i32) -> i32 {\n"
                          "    if flag {\n"
                          "        putchar(n);\n"
                          "    }\n"
                          "    return putchar(-1);\n"
                          "}\n";

static const char cell[] = "fn cell(a: int, b: int) -> int {\n"
                           "    let x = a;\n"
                           "    let p = &x;\n"
                           "    *p = *p + b;\n"
                           "    return x;\n"
                           "}\n"
                           "fn bytes(p: *bool) -> *bool {\n"
                           "    if *p {\n"
                           "        *p = false;\n"
                           "    }\n"
                           "    return &p[8];\n"
                           "}\n";

static const char compare[] = "fn cmp(a: i32, b: i32) -> bool {\n"
                              "    return !(a < b) == (a != b);\n"
                              "}\n";

static const char widen[] = "fn widen(x: c_long, w: c_wchar) -> int {\n"
                            "    return x as int + w as int;\n"
                            "}\n";

static const char aligned[] = "packed struct Tight { a: i8, b: i32 }\n"
                              "struct Wide align(16) { a: i8 }\n"
                              "fn tight(t: Tight, b: int) -> int {\n"
                              "    return t.b as int + b;\n"
                              "}\n"
                              "fn wide(a: int, w: Wide, b: int) -> int {\n"
                              "    return a + w.a as int + b;\n"
                              "}\n";

void test_select(void)
{
    /* System V passes a struct with a field off its alignment in memory,
       and the empty second eightbyte of Wide takes no register. */
    selects(aligned, TARGET_LINUX_X86_64,
            "main.tight:\n"
            "b0:\n"
            "    leaq 16(%rbp), %t0\n"
            "    movq %rdi, %t1\n"
            "    movl 1(%t0), %t3\n"
            "    movslq %t3, %t4\n"
            "    movq %t4, %t5\n"
            "    addq %t1, %t5\n"
            "    movq %t5, %rax\n"
            "    ret\n"
            "main.wide:\n"
            "b0:\n"
            "    movq %rdi, %t0\n"
            "    leaq slot0, %t1\n"
            "    movq %rsi, (%t1)\n"
            "    movq %rdx, %t2\n"
            "    movb (%t1), %t3\n"
            "    movsbq %t3, %t4\n"
            "    movq %t0, %t5\n"
            "    addq %t4, %t5\n"
            "    movq %t5, %t6\n"
            "    addq %t2, %t6\n"
            "    movq %t6, %rax\n"
            "    ret\n");
    /* AAPCS64 starts a 16-aligned aggregate at an even register, and Apple
       does not. */
    selects(aligned, TARGET_LINUX_ARM64,
            "main.tight:\n"
            "b0:\n"
            "    add t0, sp, slot0\n"
            "    str w0, [t0]\n"
            "    lsr t6, x0, #32\n"
            "    strb t6, [t0, #4]\n"
            "    mov t1, x1\n"
            "    ldr t3, [t0, #1]\n"
            "    sxtw t4, t3\n"
            "    add t5, t4, t1\n"
            "    mov x0, t5\n"
            "    ret\n"
            "main.wide:\n"
            "b0:\n"
            "    mov t0, x0\n"
            "    add t1, sp, slot0\n"
            "    str x2, [t1]\n"
            "    str x3, [t1, #8]\n"
            "    mov t2, x4\n"
            "    ldrb t3, [t1]\n"
            "    sxtb t4, t3\n"
            "    add t5, t0, t4\n"
            "    add t6, t5, t2\n"
            "    mov x0, t6\n"
            "    ret\n");
    selects(aligned, TARGET_MACOS_ARM64,
            "main.tight:\n"
            "b0:\n"
            "    add t0, sp, slot0\n"
            "    str w0, [t0]\n"
            "    lsr t6, x0, #32\n"
            "    strb t6, [t0, #4]\n"
            "    mov t1, x1\n"
            "    ldr t3, [t0, #1]\n"
            "    sxtw t4, t3\n"
            "    add t5, t4, t1\n"
            "    mov x0, t5\n"
            "    ret\n"
            "main.wide:\n"
            "b0:\n"
            "    mov t0, x0\n"
            "    add t1, sp, slot0\n"
            "    str x1, [t1]\n"
            "    str x2, [t1, #8]\n"
            "    mov t2, x3\n"
            "    ldrb t3, [t1]\n"
            "    sxtb t4, t3\n"
            "    add t5, t0, t4\n"
            "    add t6, t5, t2\n"
            "    mov x0, t6\n"
            "    ret\n");
    /* c_long is 64 bits and c_wchar 32 bits on Linux, so the conversions
       are a copy and an extension. wchar_t is a signed int on Linux x86_64
       and macOS and an unsigned int on Linux ARM64. On Windows c_long and
       c_wchar are 32 and 16 bits, wchar_t is unsigned, and both extend. */
    selects(widen, TARGET_LINUX_X86_64,
            "main.widen:\n"
            "b0:\n"
            "    movq %rdi, %t0\n"
            "    movl %esi, %t1\n"
            "    movslq %t1, %t2\n"
            "    movq %t0, %t3\n"
            "    addq %t2, %t3\n"
            "    movq %t3, %rax\n"
            "    ret\n");
    selects(widen, TARGET_LINUX_ARM64,
            "main.widen:\n"
            "b0:\n"
            "    mov t0, x0\n"
            "    mov t1, w1\n"
            "    mov t2, t1\n"
            "    add t3, t0, t2\n"
            "    mov x0, t3\n"
            "    ret\n");
    selects(widen, TARGET_MACOS_ARM64,
            "main.widen:\n"
            "b0:\n"
            "    mov t0, x0\n"
            "    mov t1, w1\n"
            "    sxtw t2, t1\n"
            "    add t3, t0, t2\n"
            "    mov x0, t3\n"
            "    ret\n");
    selects(widen, TARGET_WINDOWS_X86_64,
            "main.widen:\n"
            "b0:\n"
            "    movl %ecx, %t0\n"
            "    movw %dx, %t1\n"
            "    movslq %t0, %t2\n"
            "    movzwq %t1, %t3\n"
            "    movq %t2, %t4\n"
            "    addq %t3, %t4\n"
            "    movq %t4, %rax\n"
            "    ret\n");
    selects(widen, TARGET_WINDOWS_ARM64,
            "main.widen:\n"
            "b0:\n"
            "    mov t0, w0\n"
            "    mov t1, w1\n"
            "    sxtw t2, t0\n"
            "    uxth t3, t1\n"
            "    add t4, t2, t3\n"
            "    mov x0, t4\n"
            "    ret\n");
    selects(scale, TARGET_MACOS_ARM64,
            "main.scale:\n"
            "b0:\n"
            "    mov t0, x0\n"
            "    mov t2, #6\n"
            "    mul t1, t0, t2\n"
            "    mov x0, t1\n"
            "    ret\n"
            "main.main:\n"
            "b0:\n"
            "    mov x0, #7\n"
            "    bl main.scale\n"
            "    mov t0, x0\n"
            "    mov x0, t0\n"
            "    ret\n");
    selects(scale, TARGET_LINUX_X86_64,
            "main.scale:\n"
            "b0:\n"
            "    movq %rdi, %t0\n"
            "    imulq $6, %t0, %t1\n"
            "    movq %t1, %rax\n"
            "    ret\n"
            "main.main:\n"
            "b0:\n"
            "    movq $7, %rdi\n"
            "    call main.scale\n"
            "    movq %rax, %t0\n"
            "    movq %t0, %rax\n"
            "    ret\n");
    /* Windows passes the first integer argument in rcx. */
    selects(scale, TARGET_WINDOWS_X86_64,
            "main.scale:\n"
            "b0:\n"
            "    movq %rcx, %t0\n"
            "    imulq $6, %t0, %t1\n"
            "    movq %t1, %rax\n"
            "    ret\n"
            "main.main:\n"
            "b0:\n"
            "    movq $7, %rcx\n"
            "    call main.scale\n"
            "    movq %rax, %t0\n"
            "    movq %t0, %rax\n"
            "    ret\n");

    /* A comparison that only a branch uses becomes flags and a jump. */
    selects(loop, TARGET_LINUX_ARM64,
            "main.g:\n"
            "b0:\n"
            "    mov t0, x0\n"
            "    mov t1, #0\n"
            "b1:\n"
            "    cmp t1, t0\n"
            "    b.ge b3\n"
            "b2:\n"
            "    add t1, t1, #1\n"
            "    b b1\n"
            "b3:\n"
            "    cmp t1, #3\n"
            "    b.le b5\n"
            "b4:\n"
            "    mov x0, #1\n"
            "    ret\n"
            "b5:\n"
            "    mov x0, #0\n"
            "    ret\n");
    selects(loop, TARGET_MACOS_X86_64,
            "main.g:\n"
            "b0:\n"
            "    movq %rdi, %t0\n"
            "    movq $0, %t1\n"
            "b1:\n"
            "    cmpq %t0, %t1\n"
            "    jge b3\n"
            "b2:\n"
            "    addq $1, %t1\n"
            "    jmp b1\n"
            "b3:\n"
            "    cmpq $3, %t1\n"
            "    jle b5\n"
            "b4:\n"
            "    movq $1, %rax\n"
            "    ret\n"
            "b5:\n"
            "    movq $0, %rax\n"
            "    ret\n");

    /* A comparison as a value, and bool values of 32 or 8 bits. */
    selects(compare, TARGET_MACOS_ARM64,
            "main.cmp:\n"
            "b0:\n"
            "    mov t0, w0\n"
            "    mov t1, w1\n"
            "    cmp t0, t1\n"
            "    cset t2, lt\n"
            "    eor t3, t2, #1\n"
            "    cmp t0, t1\n"
            "    cset t4, ne\n"
            "    uxtb t6, t3\n"
            "    cmp t6, t4, uxtb\n"
            "    cset t5, eq\n"
            "    mov w0, t5\n"
            "    ret\n");
    selects(compare, TARGET_LINUX_X86_64,
            "main.cmp:\n"
            "b0:\n"
            "    movl %edi, %t0\n"
            "    movl %esi, %t1\n"
            "    cmpl %t1, %t0\n"
            "    setl %t2\n"
            "    movb %t2, %t3\n"
            "    xorb $1, %t3\n"
            "    cmpl %t1, %t0\n"
            "    setne %t4\n"
            "    cmpb %t4, %t3\n"
            "    sete %t5\n"
            "    movb %t5, %al\n"
            "    ret\n");

    /* Immediates beyond the instruction limits, two-operand forms and
       calls of a C function. */
    selects(ops, TARGET_MACOS_ARM64,
            "main.arith:\n"
            "b0:\n"
            "    mov t0, x0\n"
            "    mov t1, x1\n"
            "    movz t7, #61952\n"
            "    movk t7, #10757, lsl #16\n"
            "    movk t7, #1, lsl #32\n"
            "    add t2, t0, t7\n"
            "    add t3, t1, #7\n"
            "    neg t4, t2\n"
            "    mvn t5, t3\n"
            "    mul t6, t4, t5\n"
            "    mov x0, t6\n"
            "    ret\n"
            "main.bits:\n"
            "b0:\n"
            "    mov t0, w0\n"
            "    and t1, t0, #-2\n"
            "    eor t2, t0, #65536\n"
            "    orr t3, t1, t2\n"
            "    mov w0, t3\n"
            "    ret\n"
            "main.flip:\n"
            "b0:\n"
            "    mov t0, x0\n"
            "    mov t1, t0\n"
            "b1:\n"
            "    cmp t1, #0\n"
            "    b.le b3\n"
            "b2:\n"
            "    mov t3, #10\n"
            "    sub t1, t3, t1\n"
            "    b b1\n"
            "b3:\n"
            "    mov x0, t1\n"
            "    ret\n"
            "main.calls:\n"
            "b0:\n"
            "    mov t0, w0\n"
            "    mov t1, w1\n"
            "    cbz t0, b2\n"
            "b1:\n"
            "    mov w0, t1\n"
            "    bl putchar\n"
            "    mov t2, w0\n"
            "b2:\n"
            "    mov w0, #-1\n"
            "    bl putchar\n"
            "    mov t3, w0\n"
            "    mov w0, t3\n"
            "    ret\n");
    selects(ops, TARGET_LINUX_X86_64,
            "main.arith:\n"
            "b0:\n"
            "    movq %rdi, %t0\n"
            "    movq %rsi, %t1\n"
            "    movabsq $5000000000, %t7\n"
            "    movq %t0, %t2\n"
            "    addq %t7, %t2\n"
            "    movq %t1, %t3\n"
            "    subq $-7, %t3\n"
            "    movq %t2, %t4\n"
            "    negq %t4\n"
            "    movq %t3, %t5\n"
            "    notq %t5\n"
            "    movq %t4, %t6\n"
            "    imulq %t5, %t6\n"
            "    movq %t6, %rax\n"
            "    ret\n"
            "main.bits:\n"
            "b0:\n"
            "    movl %edi, %t0\n"
            "    movl %t0, %t1\n"
            "    andl $-2, %t1\n"
            "    movl %t0, %t2\n"
            "    xorl $65536, %t2\n"
            "    movl %t1, %t3\n"
            "    orl %t2, %t3\n"
            "    movl %t3, %eax\n"
            "    ret\n"
            "main.flip:\n"
            "b0:\n"
            "    movq %rdi, %t0\n"
            "    movq %t0, %t1\n"
            "b1:\n"
            "    cmpq $0, %t1\n"
            "    jle b3\n"
            "b2:\n"
            "    movq %t1, %t3\n"
            "    movq $10, %t1\n"
            "    subq %t3, %t1\n"
            "    jmp b1\n"
            "b3:\n"
            "    movq %t1, %rax\n"
            "    ret\n"
            "main.calls:\n"
            "b0:\n"
            "    movb %dil, %t0\n"
            "    movl %esi, %t1\n"
            "    testb %t0, %t0\n"
            "    je b2\n"
            "b1:\n"
            "    movl %t1, %edi\n"
            "    call putchar\n"
            "    movl %eax, %t2\n"
            "b2:\n"
            "    movl $-1, %edi\n"
            "    call putchar\n"
            "    movl %eax, %t3\n"
            "    movl %t3, %eax\n"
            "    ret\n");

    /* An address-taken local lives in a stack slot. */
    selects(cell, TARGET_MACOS_ARM64,
            "main.cell:\n"
            "b0:\n"
            "    mov t0, x0\n"
            "    mov t1, x1\n"
            "    add t2, sp, slot0\n"
            "    str t0, [t2]\n"
            "    ldr t3, [t2]\n"
            "    add t4, t3, t1\n"
            "    str t4, [t2]\n"
            "    ldr t5, [t2]\n"
            "    mov x0, t5\n"
            "    ret\n"
            "main.bytes:\n"
            "b0:\n"
            "    mov t0, x0\n"
            "    ldrb t1, [t0]\n"
            "    cbz t1, b2\n"
            "b1:\n"
            "    strb wzr, [t0]\n"
            "b2:\n"
            "    add t2, t0, #8\n"
            "    mov x0, t2\n"
            "    ret\n");
    selects(cell, TARGET_LINUX_X86_64,
            "main.cell:\n"
            "b0:\n"
            "    movq %rdi, %t0\n"
            "    movq %rsi, %t1\n"
            "    leaq slot0, %t2\n"
            "    movq %t0, (%t2)\n"
            "    movq (%t2), %t3\n"
            "    movq %t3, %t4\n"
            "    addq %t1, %t4\n"
            "    movq %t4, (%t2)\n"
            "    movq (%t2), %t5\n"
            "    movq %t5, %rax\n"
            "    ret\n"
            "main.bytes:\n"
            "b0:\n"
            "    movq %rdi, %t0\n"
            "    movb (%t0), %t1\n"
            "    testb %t1, %t1\n"
            "    je b2\n"
            "b1:\n"
            "    movb $0, (%t0)\n"
            "b2:\n"
            "    movq %t0, %t2\n"
            "    addq $8, %t2\n"
            "    movq %t2, %rax\n"
            "    ret\n");

    /* Division by a constant on both processors. */
    selects("fn d(x: int) -> int { return x / 3; }\n", TARGET_MACOS_ARM64,
            "main.d:\n"
            "b0:\n"
            "    mov t0, x0\n"
            "    mov t2, #3\n"
            "    sdiv t1, t0, t2\n"
            "    mov x0, t1\n"
            "    ret\n");
    selects("fn d(x: int) -> int { return x / 3; }\n", TARGET_LINUX_X86_64,
            "main.d:\n"
            "b0:\n"
            "    movq %rdi, %t0\n"
            "    movq $3, %t2\n"
            "    movq %t0, %rax\n"
            "    cqto\n"
            "    idivq %t2\n"
            "    movq %rax, %t1\n"
            "    movq %t1, %rax\n"
            "    ret\n");
    selects("fn f(x: f64) -> f64 { return x + 1.0; }\n", TARGET_MACOS_ARM64,
            "main.f:\n"
            "b0:\n"
            "    fmov t0, d0\n"
            "    movz t3, #16368, lsl #48\n"
            "    fmov t2, t3\n"
            "    fadd t1, t0, t2\n"
            "    fmov d0, t1\n"
            "    ret\n");
    selects("fn five(a: int, b: int, c: int, d: int, e: int) -> int {\n"
            "    return e;\n"
            "}\n",
            TARGET_WINDOWS_X86_64,
            "main.five:\n"
            "b0:\n"
            "    movq 48(%rbp), %t4\n"
            "    movq %t4, %rax\n"
            "    ret\n");
    selects("fn five(a: int, b: int, c: int, d: int, e: int, f: int, g: int,\n"
            "        h: int, i: int) -> int {\n"
            "    return i;\n"
            "}\n",
            TARGET_MACOS_ARM64,
            "main.five:\n"
            "b0:\n"
            "    ldr t8, [x29, #16]\n"
            "    mov x0, t8\n"
            "    ret\n");
}
