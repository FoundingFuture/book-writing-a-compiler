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
        !sema_check(module, "main", NULL, 0, &types, &arena, &diags) ||
        !lower_module(module, "main", &ir, &diags)) {
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

static void allocates(const char *source, enum target target,
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

static const char twice[] = "extern fn putchar(c: i32) -> i32;\n"
                            "fn twice(c: i32) -> i32 {\n"
                            "    putchar(c);\n"
                            "    putchar(c);\n"
                            "    return c;\n"
                            "}\n";

static const char cell[] = "fn cell(a: int, b: int) -> int {\n"
                           "    let x = a;\n"
                           "    let p = &x;\n"
                           "    *p = *p + b;\n"
                           "    return x;\n"
                           "}\n";

void test_regalloc(void)
{
    /* The instructions of the assembly listing in chapter 1. */
    allocates(scale, TARGET_MACOS_ARM64,
              "main.scale:\n"
              "b0:\n"
              "    mov x9, #6\n"
              "    mul x0, x0, x9\n"
              "    ret\n"
              "main.main:\n"
              "b0:\n"
              "    stp x29, x30, [sp, #-16]!\n"
              "    mov x29, sp\n"
              "    mov x0, #7\n"
              "    bl main.scale\n"
              "    ldp x29, x30, [sp], #16\n"
              "    ret\n");
    allocates(scale, TARGET_LINUX_X86_64,
              "main.scale:\n"
              "b0:\n"
              "    imulq $6, %rdi, %rax\n"
              "    ret\n"
              "main.main:\n"
              "b0:\n"
              "    pushq %rbp\n"
              "    movq %rsp, %rbp\n"
              "    movq $7, %rdi\n"
              "    call main.scale\n"
              "    popq %rbp\n"
              "    ret\n");
    /* A Windows caller reserves the shadow store for four arguments. */
    allocates(scale, TARGET_WINDOWS_X86_64,
              "main.scale:\n"
              "b0:\n"
              "    imulq $6, %rcx, %rax\n"
              "    ret\n"
              "main.main:\n"
              "b0:\n"
              "    pushq %rbp\n"
              "    .seh_pushreg %rbp\n"
              "    movq %rsp, %rbp\n"
              "    subq $32, %rsp\n"
              "    .seh_stackalloc 32\n"
              "    .seh_endprologue\n"
              "    movq $7, %rcx\n"
              "    call main.scale\n"
              "    addq $32, %rsp\n"
              "    popq %rbp\n"
              "    ret\n");

    /* Intervals across a loop. */
    allocates("fn g(n: int) -> int {\n"
              "    let i = 0;\n"
              "    while i < n do {\n"
              "        i += 1;\n"
              "    }\n"
              "    if i > 3 {\n"
              "        return 1;\n"
              "    } else {\n"
              "        return 0;\n"
              "    }\n"
              "}\n",
              TARGET_LINUX_ARM64,
              "main.g:\n"
              "b0:\n"
              "    mov x9, #0\n"
              "b1:\n"
              "    cmp x9, x0\n"
              "    b.ge b3\n"
              "b2:\n"
              "    add x9, x9, #1\n"
              "    b b1\n"
              "b3:\n"
              "    cmp x9, #3\n"
              "    b.le b5\n"
              "b4:\n"
              "    mov x0, #1\n"
              "    ret\n"
              "b5:\n"
              "    mov x0, #0\n"
              "    ret\n");

    /* A value live across a call goes to a callee-saved register, which
       the prologue saves. */
    allocates(twice, TARGET_MACOS_ARM64,
              "main.twice:\n"
              "b0:\n"
              "    stp x29, x30, [sp, #-16]!\n"
              "    mov x29, sp\n"
              "    sub sp, sp, #16\n"
              "    str x19, [sp, #8]\n"
              "    mov w19, w0\n"
              "    mov w0, w19\n"
              "    bl putchar\n"
              "    mov w0, w19\n"
              "    bl putchar\n"
              "    mov w0, w19\n"
              "    ldr x19, [sp, #8]\n"
              "    mov sp, x29\n"
              "    ldp x29, x30, [sp], #16\n"
              "    ret\n");
    allocates(twice, TARGET_LINUX_X86_64,
              "main.twice:\n"
              "b0:\n"
              "    pushq %rbp\n"
              "    movq %rsp, %rbp\n"
              "    subq $16, %rsp\n"
              "    movq %rbx, 8(%rsp)\n"
              "    movl %edi, %ebx\n"
              "    movl %ebx, %edi\n"
              "    call putchar\n"
              "    movl %ebx, %edi\n"
              "    call putchar\n"
              "    movl %ebx, %eax\n"
              "    movq 8(%rsp), %rbx\n"
              "    movq %rbp, %rsp\n"
              "    popq %rbp\n"
              "    ret\n");

    /* The address-taken local x lives in a stack slot. */
    allocates(cell, TARGET_MACOS_ARM64,
              "main.cell:\n"
              "b0:\n"
              "    stp x29, x30, [sp, #-16]!\n"
              "    mov x29, sp\n"
              "    sub sp, sp, #16\n"
              "    add x9, sp, #0\n"
              "    str x0, [x9]\n"
              "    ldr x10, [x9]\n"
              "    add x10, x10, x1\n"
              "    str x10, [x9]\n"
              "    ldr x0, [x9]\n"
              "    mov sp, x29\n"
              "    ldp x29, x30, [sp], #16\n"
              "    ret\n");
    allocates(cell, TARGET_LINUX_X86_64,
              "main.cell:\n"
              "b0:\n"
              "    pushq %rbp\n"
              "    movq %rsp, %rbp\n"
              "    subq $16, %rsp\n"
              "    leaq (%rsp), %rax\n"
              "    movq %rdi, (%rax)\n"
              "    movq (%rax), %rcx\n"
              "    addq %rsi, %rcx\n"
              "    movq %rcx, (%rax)\n"
              "    movq (%rax), %rax\n"
              "    movq %rbp, %rsp\n"
              "    popq %rbp\n"
              "    ret\n");
}
