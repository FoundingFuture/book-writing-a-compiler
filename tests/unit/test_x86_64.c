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

/* A Windows frame of a page or more probes its pages with __chkstk. The
   unwind directives describe the push, the allocation and each save
   relative to rsp, and the epilogue frees the frame with add. */
static void probe(void)
{
    const struct target_desc *target = target_desc(TARGET_WINDOWS_X86_64);
    struct ir_module m;
    struct mach_block b;
    struct frame frame;
    struct text out = {0};
    size_t i;

    memset(&m, 0, sizeof m);
    memset(&b, 0, sizeof b);
    memset(&frame, 0, sizeof frame);
    frame.needed = true;
    frame.size = 8192;
    frame.probe = true;
    frame.unwind = true;
    frame.convention = CONVENTION_WINDOWS_X64;
    /* rbx is register 3 and xmm6 register 22, as in src/x86_64.c. */
    frame.saved[0] = 3;
    frame.saved_offset[0] = 8184;
    frame.saved[1] = 22;
    frame.saved_offset[1] = 8160;
    frame.saved_count = 2;
    target->prologue(&b, &frame);
    target->epilogue(&b, &frame);
    for (i = 0; i < b.count; i++) {
        target->print(&out, &m, &b.insts[i], NULL);
        text_append(&out, "\n");
    }
    CHECK_STR(text_cstr(&out), "pushq %rbp\n"
                               ".seh_pushreg %rbp\n"
                               "movq %rsp, %rbp\n"
                               "movl $8192, %eax\n"
                               "call __chkstk\n"
                               "subq %rax, %rsp\n"
                               ".seh_stackalloc 8192\n"
                               "movq %rbx, 8184(%rsp)\n"
                               ".seh_savereg %rbx, 8184\n"
                               "movups %xmm6, 8160(%rsp)\n"
                               ".seh_savexmm %xmm6, 8160\n"
                               ".seh_endprologue\n"
                               "movq 8184(%rsp), %rbx\n"
                               "movups 8160(%rsp), %xmm6\n"
                               "addq $8192, %rsp\n"
                               "popq %rbp\n");
    free(b.insts);
    text_free(&out);
}

/* The address of a function relative to rip. */
static void rip_relative(void)
{
    struct arena arena = {0};
    struct ir_module m;
    struct ir_function *helper;
    struct ir_function *f;
    struct mach_function *functions[2] = {NULL, NULL};
    struct text out = {0};
    char error[200] = "";
    uint32_t address;
    size_t i;

    ir_module_init(&m, &arena, "main");
    helper = ir_function_add(&m, "main", "helper", IR_I64, IR_NO_AGG);
    ir_ret(helper, ir_block_add(helper), IR_I64, ir_int_op(IR_I64, 1));
    f = ir_function_add(&m, "main", "f", IR_PTR, IR_NO_AGG);
    address = ir_addr(f, ir_block_add(f), ir_func_op(helper));
    ir_ret(f, f->blocks[0], IR_PTR, ir_temp_op(f, address));
    CHECK(select_module(TARGET_LINUX_X86_64, &m, functions, error,
                        sizeof error));
    for (i = 0; i < 2; i++) {
        CHECK(regalloc_function(TARGET_LINUX_X86_64, functions[i], error,
                                sizeof error));
        mach_print(&out, target_desc(TARGET_LINUX_X86_64), &m, functions[i]);
        mach_function_free(functions[i]);
        free(functions[i]);
    }
    CHECK_STR(text_cstr(&out), "main.helper:\n"
                               "b0:\n"
                               "    movq $1, %rax\n"
                               "    ret\n"
                               "main.f:\n"
                               "b0:\n"
                               "    leaq main.helper(%rip), %rax\n"
                               "    ret\n");
    text_free(&out);
    ir_module_free(&m);
    arena_free(&arena);
}

static const char fnptr[] = "extern fn abs(x: i32) -> i32;\n"
                            "fn apply(f: fn(i32) -> i32, x: i32) -> i32 {\n"
                            "    return f(x);\n"
                            "}\n"
                            "fn pick() -> fn(i32) -> i32 {\n"
                            "    return abs;\n"
                            "}\n";

void test_x86_64(void)
{
    probe();
    rip_relative();

    /* A 16-byte xmm save starts at a multiple of 16, which the unwind
       code of the save requires. rbx takes 8 bytes above it. */
    {
        struct text out = {0};
        run("extern fn g() -> f64;\n"
            "fn f(a: int, x: f64) -> f64 {\n"
            "    let y = g();\n"
            "    return y + x + (a as f64);\n"
            "}\n",
            TARGET_WINDOWS_X86_64, &out);
        CHECK(strstr(text_cstr(&out), "movups %xmm6, 32(%rsp)\n") != NULL);
        CHECK(strstr(text_cstr(&out), ".seh_savexmm %xmm6, 32\n") != NULL);
        CHECK(strstr(text_cstr(&out), ".seh_savereg %rbx, 56\n") != NULL);
        text_free(&out);
    }

    /* Division leaves the quotient in rax and the remainder in rdx. */
    emits("fn div(a: int, b: int) -> int {\n"
          "    return a / b + a % b;\n"
          "}\n",
          TARGET_LINUX_X86_64,
          "main.div:\n"
          "b0:\n"
          "    movq %rdi, %rax\n"
          "    cqto\n"
          "    idivq %rsi\n"
          "    movq %rax, %rcx\n"
          "    movq %rdi, %rax\n"
          "    cqto\n"
          "    idivq %rsi\n"
          "    movq %rcx, %rax\n"
          "    addq %rdx, %rax\n"
          "    ret\n");

    /* A variable shift count goes into cl, and unsigned division clears
       rdx. */
    emits("fn ushift(a: u32, b: u32) -> u32 {\n"
          "    return (a >> b) / (b << 2);\n"
          "}\n",
          TARGET_LINUX_X86_64,
          "main.ushift:\n"
          "b0:\n"
          "    movb %sil, %cl\n"
          "    movl %edi, %eax\n"
          "    shrl %cl, %eax\n"
          "    shll $2, %esi\n"
          "    movl $0, %edx\n"
          "    divl %esi\n"
          "    ret\n");

    /* 8-bit arithmetic and the extensions to 64 bits. */
    emits("fn bytes(a: u8, b: i8) -> int {\n"
          "    return (a * 3) as int + (b + 1) as int;\n"
          "}\n",
          TARGET_LINUX_X86_64,
          "main.bytes:\n"
          "b0:\n"
          "    imull $3, %edi, %eax\n"
          "    movzbq %al, %rax\n"
          "    addb $1, %sil\n"
          "    movsbq %sil, %rcx\n"
          "    addq %rcx, %rax\n"
          "    ret\n");

    /* movl %eax, %eax clears the upper 32 bits and stays. */
    emits("extern fn g() -> int;\n"
          "fn z() -> u64 {\n"
          "    return g() as u32 as u64;\n"
          "}\n",
          TARGET_LINUX_X86_64,
          "main.z:\n"
          "b0:\n"
          "    pushq %rbp\n"
          "    movq %rsp, %rbp\n"
          "    call g\n"
          "    movl %eax, %eax\n"
          "    popq %rbp\n"
          "    ret\n");

    /* A call through a function pointer reads the address from a register.
       A PIE takes the address of a C function from the GOT under System
       V, and Windows links C functions statically. */
    emits(fnptr, TARGET_LINUX_X86_64,
          "main.apply:\n"
          "b0:\n"
          "    pushq %rbp\n"
          "    movq %rsp, %rbp\n"
          "    movq %rdi, %rax\n"
          "    movl %esi, %edi\n"
          "    call *%rax\n"
          "    popq %rbp\n"
          "    ret\n"
          "main.pick:\n"
          "b0:\n"
          "    movq abs@GOTPCREL(%rip), %rax\n"
          "    ret\n");
    emits(fnptr, TARGET_WINDOWS_X86_64,
          "main.apply:\n"
          "b0:\n"
          "    pushq %rbp\n"
          "    .seh_pushreg %rbp\n"
          "    movq %rsp, %rbp\n"
          "    subq $32, %rsp\n"
          "    .seh_stackalloc 32\n"
          "    .seh_endprologue\n"
          "    movq %rcx, %rax\n"
          "    movl %edx, %ecx\n"
          "    call *%rax\n"
          "    addq $32, %rsp\n"
          "    popq %rbp\n"
          "    ret\n"
          "main.pick:\n"
          "b0:\n"
          "    leaq abs(%rip), %rax\n"
          "    ret\n");

    /* System V callers extend register arguments of 8 and 16 bits to 32
       bits, which clang callees rely on. Windows callees extend them. */
    emits("extern fn widen(s: i16, u: u8, n: int) -> i32;\n"
          "fn pass(x: i16, y: u8) -> i32 {\n"
          "    return widen(x, y, 7) + widen(-2, 200, 1);\n"
          "}\n",
          TARGET_LINUX_X86_64,
          "main.pass:\n"
          "b0:\n"
          "    pushq %rbp\n"
          "    movq %rsp, %rbp\n"
          "    subq $16, %rsp\n"
          "    movq %rbx, 8(%rsp)\n"
          "    movswl %di, %edi\n"
          "    movzbl %sil, %esi\n"
          "    movq $7, %rdx\n"
          "    call widen\n"
          "    movl %eax, %ebx\n"
          "    movl $-2, %edi\n"
          "    movl $200, %esi\n"
          "    movq $1, %rdx\n"
          "    call widen\n"
          "    movl %ebx, %ecx\n"
          "    addl %eax, %ecx\n"
          "    movl %ecx, %eax\n"
          "    movq 8(%rsp), %rbx\n"
          "    movq %rbp, %rsp\n"
          "    popq %rbp\n"
          "    ret\n");
}
