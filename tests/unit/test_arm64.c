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

static const char eleven[] =
    "extern fn take(a: u8, b: i16, c: int, d: int, e: int, f: int, g: int,\n"
    "               h: int, i: i8, j: i32, k: u8) -> int;\n"
    "fn calls(x: u8, y: i16) -> int {\n"
    "    return take(x, y, 3, 4, 5, 6, 7, 8, -1, 9, 200);\n"
    "}\n"
    "fn last(a: u8, b: i16, c: int, d: int, e: int, f: int, g: int, h: int,\n"
    "        i: i8, j: i32, k: u8) -> int {\n"
    "    return c + j as int + k as int;\n"
    "}\n";

static const char variadic[] =
    "extern fn printf(format: *byte, ...) -> i32;\n"
    "fn show(p: *byte, v: int) {\n"
    "    printf(p, v, 7);\n"
    "}\n";

/* The address of a function: adrp gives its 4 KB page, and add the low
   12 bits. */
static void page_address(void)
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
    CHECK(select_module(TARGET_LINUX_ARM64, &m, functions, error,
                        sizeof error));
    CHECK_STR(error, "");
    for (i = 0; i < 2 && functions[i] != NULL; i++) {
        CHECK(regalloc_function(TARGET_LINUX_ARM64, functions[i], error,
                                sizeof error));
        mach_print(&out, target_desc(TARGET_LINUX_ARM64), &m, functions[i]);
        mach_function_free(functions[i]);
        free(functions[i]);
    }
    CHECK_STR(text_cstr(&out), "main.helper:\n"
                               "b0:\n"
                               "    mov x0, #1\n"
                               "    ret\n"
                               "main.f:\n"
                               "b0:\n"
                               "    adrp x0, main.helper\n"
                               "    add x0, x0, :lo12:main.helper\n"
                               "    ret\n");
    text_free(&out);
    ir_module_free(&m);
    arena_free(&arena);
}

/* Append the instructions of b to out, one per line, and free them. */
static void print_block(struct text *out, const struct target_desc *target,
                        struct mach_block *b)
{
    struct ir_module m;
    size_t i;

    memset(&m, 0, sizeof m);
    for (i = 0; i < b->count; i++) {
        target->print(out, &m, &b->insts[i], NULL);
        text_append(out, "\n");
    }
    free(b->insts);
    memset(b, 0, sizeof *b);
}

/* Frame offsets beyond the immediates. One frame size fits a 12-bit
   immediate shifted by 12, and another goes through x16. Saved registers
   and spill slots lie beyond the scaled offset limit of 32760 bytes. */
static void large_frames(void)
{
    const struct target_desc *target = target_desc(TARGET_LINUX_ARM64);
    struct mach_block b;
    struct frame frame;
    struct text out = {0};

    memset(&b, 0, sizeof b);
    memset(&frame, 0, sizeof frame);
    frame.needed = true;
    frame.size = 8192;
    target->prologue(&b, &frame);
    frame.size = 70000;
    target->prologue(&b, &frame);
    frame.size = 65536;
    frame.saved[0] = 19;
    frame.saved_offset[0] = 65528;
    frame.saved_count = 1;
    target->prologue(&b, &frame);
    target->epilogue(&b, &frame);
    target->load_spill(&b, 16, 32760);
    target->load_spill(&b, 16, 40000);
    target->store_spill(&b, 16, 40000);
    target->store_spill(&b, 17, 40000);
    print_block(&out, target, &b);
    CHECK_STR(text_cstr(&out), "stp x29, x30, [sp, #-16]!\n"
                               "mov x29, sp\n"
                               "sub sp, sp, #2, lsl #12\n"
                               "stp x29, x30, [sp, #-16]!\n"
                               "mov x29, sp\n"
                               "movz x16, #4464\n"
                               "movk x16, #1, lsl #16\n"
                               "sub sp, sp, x16\n"
                               "stp x29, x30, [sp, #-16]!\n"
                               "mov x29, sp\n"
                               "sub sp, sp, #16, lsl #12\n"
                               "mov x16, #65528\n"
                               "str x19, [sp, x16]\n"
                               "mov x19, #65528\n"
                               "ldr x19, [sp, x19]\n"
                               "mov sp, x29\n"
                               "ldp x29, x30, [sp], #16\n"
                               "ldr x16, [sp, #32760]\n"
                               "mov x16, #40000\n"
                               "ldr x16, [sp, x16]\n"
                               "mov x17, #40000\n"
                               "str x16, [sp, x17]\n"
                               "mov x16, #40000\n"
                               "str x17, [sp, x16]\n");
    text_free(&out);
}

/* Windows probes a frame of 4096 bytes or more with __chkstk, which takes
   the size divided by 16 in x15. Each instruction of the prologue and the
   epilogue has one unwind directive. The prologue allocates the save area
   before the saves, which keeps their offsets small, and points x29 at the
   frame record. The rest of the frame follows x29 in the body. */
static void probe(void)
{
    const struct target_desc *target = target_desc(TARGET_WINDOWS_ARM64);
    struct mach_block b;
    struct frame frame;
    struct text out = {0};

    memset(&b, 0, sizeof b);
    memset(&frame, 0, sizeof frame);
    frame.needed = true;
    frame.size = 8192;
    frame.probe = true;
    frame.unwind = true;
    frame.convention = CONVENTION_WINDOWS_ARM64;
    frame.saved[0] = 19;
    frame.saved_offset[0] = 8184;
    frame.saved[1] = 32 + 8;
    frame.saved_offset[1] = 8176;
    frame.saved_count = 2;
    target->prologue(&b, &frame);
    target->epilogue(&b, &frame);
    print_block(&out, target, &b);
    CHECK_STR(text_cstr(&out), "stp x29, x30, [sp, #-16]!\n"
                               ".seh_save_fplr_x 16\n"
                               "sub sp, sp, #16\n"
                               ".seh_stackalloc 16\n"
                               "str x19, [sp, #8]\n"
                               ".seh_save_reg x19, 8\n"
                               "str d8, [sp]\n"
                               ".seh_save_freg d8, 0\n"
                               "add x29, sp, #16\n"
                               ".seh_add_fp 16\n"
                               ".seh_endprologue\n"
                               "mov x15, #511\n"
                               "bl __chkstk\n"
                               "mov x16, #8176\n"
                               "sub sp, sp, x16\n"
                               ".seh_startepilogue\n"
                               "mov x16, #8176\n"
                               ".seh_nop\n"
                               "add sp, sp, x16\n"
                               ".seh_stackalloc 8176\n"
                               "ldr x19, [sp, #8]\n"
                               ".seh_save_reg x19, 8\n"
                               "ldr d8, [sp]\n"
                               ".seh_save_freg d8, 0\n"
                               "add sp, sp, #16\n"
                               ".seh_stackalloc 16\n"
                               "ldp x29, x30, [sp], #16\n"
                               ".seh_save_fplr_x 16\n"
                               ".seh_endepilogue\n");
    text_free(&out);
    /* A frame without saves sets x29 with mov and frees the frame with one
       add. */
    memset(&frame, 0, sizeof frame);
    frame.needed = true;
    frame.size = 48;
    frame.unwind = true;
    frame.convention = CONVENTION_WINDOWS_ARM64;
    target->prologue(&b, &frame);
    target->epilogue(&b, &frame);
    print_block(&out, target, &b);
    CHECK_STR(text_cstr(&out), "stp x29, x30, [sp, #-16]!\n"
                               ".seh_save_fplr_x 16\n"
                               "mov x29, sp\n"
                               ".seh_set_fp\n"
                               ".seh_endprologue\n"
                               "sub sp, sp, #48\n"
                               ".seh_startepilogue\n"
                               "add sp, sp, #48\n"
                               ".seh_stackalloc 48\n"
                               "ldp x29, x30, [sp], #16\n"
                               ".seh_save_fplr_x 16\n"
                               ".seh_endepilogue\n");
    text_free(&out);
}

/* A slot address beyond a 12-bit offset loads the offset into its
   register first. The entry block computes all 520 slot addresses, and
   the addresses that live across the calls spill. */
static void far_slots(void)
{
    struct text source = {0};
    struct text out = {0};
    int i;

    text_append(&source, "extern fn take(p: *int);\nfn big() -> int {\n");
    for (i = 0; i < 520; i++) {
        text_appendf(&source, "    let x%d = %d;\n    take(&x%d);\n", i, i, i);
    }
    text_append(&source, "    return x0;\n}\n");
    run(text_cstr(&source), TARGET_LINUX_ARM64, &out);
    CHECK(strstr(text_cstr(&out), "    mov x16, #8320\n"
                                  "    sub sp, sp, x16\n") != NULL);
    CHECK(strstr(text_cstr(&out), "    add x16, sp, #4088\n"
                                  "    str x16, [sp, #8168]\n"
                                  "    add x16, sp, #1, lsl #12\n"
                                  "    str x16, [sp, #8176]\n"
                                  "    mov x16, #4104\n"
                                  "    add x16, sp, x16\n") != NULL);
    text_free(&source);
    text_free(&out);
}

static const char fnptr[] = "extern fn abs(x: i32) -> i32;\n"
                            "fn apply(f: fn(i32) -> i32, x: i32) -> i32 {\n"
                            "    return f(x);\n"
                            "}\n"
                            "fn pick() -> fn(i32) -> i32 {\n"
                            "    return abs;\n"
                            "}\n";

void test_arm64(void)
{
    /* blr calls the address in a register. The address of a C function
       comes from the GOT on Linux and macOS, and from adrp and add on
       Windows. The dumps print the ELF form. */
    emits(fnptr, TARGET_LINUX_ARM64,
          "main.apply:\n"
          "b0:\n"
          "    stp x29, x30, [sp, #-16]!\n"
          "    mov x29, sp\n"
          "    mov x9, x0\n"
          "    mov w0, w1\n"
          "    blr x9\n"
          "    ldp x29, x30, [sp], #16\n"
          "    ret\n"
          "main.pick:\n"
          "b0:\n"
          "    adrp x0, :got:abs\n"
          "    ldr x0, [x0, :got_lo12:abs]\n"
          "    ret\n");
    emits(fnptr, TARGET_WINDOWS_ARM64,
          "main.apply:\n"
          "b0:\n"
          "    stp x29, x30, [sp, #-16]!\n"
          "    .seh_save_fplr_x 16\n"
          "    mov x29, sp\n"
          "    .seh_set_fp\n"
          "    .seh_endprologue\n"
          "    mov x9, x0\n"
          "    mov w0, w1\n"
          "    blr x9\n"
          "    .seh_startepilogue\n"
          "    ldp x29, x30, [sp], #16\n"
          "    .seh_save_fplr_x 16\n"
          "    .seh_endepilogue\n"
          "    ret\n"
          "main.pick:\n"
          "b0:\n"
          "    adrp x0, abs\n"
          "    add x0, x0, :lo12:abs\n"
          "    ret\n");
    large_frames();
    probe();
    far_slots();
    page_address();
    /* An 8-bit value extends before a comparison, because the upper bits
       of its w register are unknown. */
    emits("fn small(a: u8, b: u8) -> bool {\n"
          "    return a + b < 10;\n"
          "}\n",
          TARGET_MACOS_ARM64,
          "main.small:\n"
          "b0:\n"
          "    add w9, w0, w1\n"
          "    uxtb w9, w9\n"
          "    cmp w9, #10\n"
          "    cset w0, lo\n"
          "    ret\n");

    /* A register operand extends inside cmp. */
    emits("fn less(a: i16, b: i16) -> bool {\n"
          "    return a < b;\n"
          "}\n",
          TARGET_LINUX_ARM64,
          "main.less:\n"
          "b0:\n"
          "    sxth w9, w0\n"
          "    cmp w9, w1, sxth\n"
          "    cset w0, lt\n"
          "    ret\n");

    /* A negative constant compares with cmn. */
    emits("fn above(a: i32) -> bool {\n"
          "    return a > -5;\n"
          "}\n",
          TARGET_LINUX_ARM64,
          "main.above:\n"
          "b0:\n"
          "    cmn w0, #5\n"
          "    cset w0, gt\n"
          "    ret\n");
    /* sdiv and udiv leave the quotient, and msub computes the remainder
       a - q * b. */
    emits("fn divs(a: int, b: int) -> int {\n"
          "    return a / b + a % b;\n"
          "}\n",
          TARGET_MACOS_ARM64,
          "main.divs:\n"
          "b0:\n"
          "    sdiv x9, x0, x1\n"
          "    sdiv x10, x0, x1\n"
          "    msub x10, x10, x1, x0\n"
          "    add x0, x9, x10\n"
          "    ret\n");

    /* 8-bit operands extend to 32 bits, and a constant divisor needs a
       register. */
    emits("fn bytes(a: u8, b: i8) -> i32 {\n"
          "    return (a / 3) as i32 + (b % b) as i32;\n"
          "}\n",
          TARGET_LINUX_ARM64,
          "main.bytes:\n"
          "b0:\n"
          "    uxtb w9, w0\n"
          "    mov w10, #3\n"
          "    udiv w9, w9, w10\n"
          "    uxtb w9, w9\n"
          "    sxtb w10, w1\n"
          "    sxtb w11, w1\n"
          "    sdiv w12, w10, w11\n"
          "    msub w10, w12, w11, w10\n"
          "    sxtb w10, w10\n"
          "    add w0, w9, w10\n"
          "    ret\n");
    /* Shifts take the count in a register or as a constant. */
    emits("fn shifts(a: int, n: int) -> int {\n"
          "    return (a << n) + (a >> n) + ((a as u64 >> n as u64) as int);\n"
          "}\n",
          TARGET_MACOS_ARM64,
          "main.shifts:\n"
          "b0:\n"
          "    lsl x9, x0, x1\n"
          "    asr x10, x0, x1\n"
          "    add x9, x9, x10\n"
          "    lsr x10, x0, x1\n"
          "    add x0, x9, x10\n"
          "    ret\n");

    /* A right shift of 8 or 16 bits extends first. A constant count of
       the register width or more goes into a register. */
    emits("fn narrow(a: i8, b: u16, c: i32) -> i32 {\n"
          "    return ((a >> 2) as i32) + ((b >> 3) as i32) + (c << 40);\n"
          "}\n",
          TARGET_LINUX_ARM64,
          "main.narrow:\n"
          "b0:\n"
          "    sxtb w9, w0\n"
          "    asr w9, w9, #2\n"
          "    sxtb w9, w9\n"
          "    uxth w10, w1\n"
          "    lsr w10, w10, #3\n"
          "    uxth w10, w10\n"
          "    add w9, w9, w10\n"
          "    mov w10, #40\n"
          "    lsl w10, w2, w10\n"
          "    add w0, w9, w10\n"
          "    ret\n");
    /* Logical immediates, 12-bit immediates shifted by 12, and a constant
       that fits neither. */
    emits("fn imms(a: int) -> int {\n"
          "    let m = (a & 0xff00) | 0x5555555555555555;\n"
          "    let k = (m ^ 0x1234) + 0x3000;\n"
          "    if k < 0x7000 {\n"
          "        return k - 0x2000;\n"
          "    }\n"
          "    return k;\n"
          "}\n",
          TARGET_MACOS_ARM64,
          "main.imms:\n"
          "b0:\n"
          "    and x9, x0, #65280\n"
          "    orr x9, x9, #6148914691236517205\n"
          "    mov x10, #4660\n"
          "    eor x9, x9, x10\n"
          "    add x9, x9, #3, lsl #12\n"
          "    cmp x9, #7, lsl #12\n"
          "    b.ge b2\n"
          "b1:\n"
          "    sub x0, x9, #2, lsl #12\n"
          "    ret\n"
          "b2:\n"
          "    mov x0, x9\n"
          "    ret\n");
    /* Addressing modes: an index shifted by the scale of the size, and
       offsets that are positive and scaled or small and negative. A store
       of zero with an index reads wzr. */
    emits("fn clear(p: *i16, n: int) {\n"
          "    let i = 0;\n"
          "    while i < n do {\n"
          "        p[i] = 0;\n"
          "        i += 1;\n"
          "    }\n"
          "}\n"
          "fn around(p: *int, q: *i32, i: int) -> int {\n"
          "    return p[1] + p[-1] + q[i] as int;\n"
          "}\n",
          TARGET_MACOS_ARM64,
          "main.clear:\n"
          "b0:\n"
          "    mov x9, #0\n"
          "b1:\n"
          "    cmp x9, x1\n"
          "    b.ge b3\n"
          "b2:\n"
          "    strh wzr, [x0, x9, lsl #1]\n"
          "    add x9, x9, #1\n"
          "    b b1\n"
          "b3:\n"
          "    ret\n"
          "main.around:\n"
          "b0:\n"
          "    ldr x9, [x0, #8]\n"
          "    ldr x10, [x0, #-8]\n"
          "    add x9, x9, x10\n"
          "    ldr w10, [x1, x2, lsl #2]\n"
          "    sxtw x10, w10\n"
          "    add x0, x9, x10\n"
          "    ret\n");
    /* Apple: a stack argument takes its own size at its own alignment,
       and the caller extends a register argument of 8 or 16 bits. */
    emits(eleven, TARGET_MACOS_ARM64,
          "main.calls:\n"
          "b0:\n"
          "    stp x29, x30, [sp, #-16]!\n"
          "    mov x29, sp\n"
          "    sub sp, sp, #16\n"
          "    mov w9, #255\n"
          "    strb w9, [sp]\n"
          "    mov w9, #9\n"
          "    str w9, [sp, #4]\n"
          "    mov w9, #200\n"
          "    strb w9, [sp, #8]\n"
          "    uxtb w0, w0\n"
          "    sxth w1, w1\n"
          "    mov x2, #3\n"
          "    mov x3, #4\n"
          "    mov x4, #5\n"
          "    mov x5, #6\n"
          "    mov x6, #7\n"
          "    mov x7, #8\n"
          "    bl take\n"
          "    mov sp, x29\n"
          "    ldp x29, x30, [sp], #16\n"
          "    ret\n"
          "main.last:\n"
          "b0:\n"
          "    stp x29, x30, [sp, #-16]!\n"
          "    mov x29, sp\n"
          "    ldr w9, [x29, #20]\n"
          "    ldrb w10, [x29, #24]\n"
          "    sxtw x9, w9\n"
          "    add x9, x2, x9\n"
          "    uxtb w10, w10\n"
          "    add x0, x9, x10\n"
          "    ldp x29, x30, [sp], #16\n"
          "    ret\n");

    /* AAPCS64: every stack argument takes 8 bytes, and the callee extends
       narrow arguments itself. */
    emits(eleven, TARGET_LINUX_ARM64,
          "main.calls:\n"
          "b0:\n"
          "    stp x29, x30, [sp, #-16]!\n"
          "    mov x29, sp\n"
          "    sub sp, sp, #32\n"
          "    mov w9, #255\n"
          "    strb w9, [sp]\n"
          "    mov w9, #9\n"
          "    str w9, [sp, #8]\n"
          "    mov w9, #200\n"
          "    strb w9, [sp, #16]\n"
          "    mov x2, #3\n"
          "    mov x3, #4\n"
          "    mov x4, #5\n"
          "    mov x5, #6\n"
          "    mov x6, #7\n"
          "    mov x7, #8\n"
          "    bl take\n"
          "    mov sp, x29\n"
          "    ldp x29, x30, [sp], #16\n"
          "    ret\n"
          "main.last:\n"
          "b0:\n"
          "    stp x29, x30, [sp, #-16]!\n"
          "    mov x29, sp\n"
          "    ldr w9, [x29, #24]\n"
          "    ldrb w10, [x29, #32]\n"
          "    sxtw x9, w9\n"
          "    add x9, x2, x9\n"
          "    uxtb w10, w10\n"
          "    add x0, x9, x10\n"
          "    ldp x29, x30, [sp], #16\n"
          "    ret\n");

    /* Apple passes every variadic argument on the stack in 8 bytes. */
    emits(variadic, TARGET_MACOS_ARM64,
          "main.show:\n"
          "b0:\n"
          "    stp x29, x30, [sp, #-16]!\n"
          "    mov x29, sp\n"
          "    sub sp, sp, #16\n"
          "    str x1, [sp]\n"
          "    mov x9, #7\n"
          "    str x9, [sp, #8]\n"
          "    bl printf\n"
          "    mov sp, x29\n"
          "    ldp x29, x30, [sp], #16\n"
          "    ret\n");
    emits(variadic, TARGET_WINDOWS_ARM64,
          "main.show:\n"
          "b0:\n"
          "    stp x29, x30, [sp, #-16]!\n"
          "    .seh_save_fplr_x 16\n"
          "    mov x29, sp\n"
          "    .seh_set_fp\n"
          "    .seh_endprologue\n"
          "    mov x2, #7\n"
          "    bl printf\n"
          "    .seh_startepilogue\n"
          "    ldp x29, x30, [sp], #16\n"
          "    .seh_save_fplr_x 16\n"
          "    .seh_endepilogue\n"
          "    ret\n");

    /* A zero extension from 32 bits is a move of the w register into
       itself, which clears the upper bits and must stay. */
    emits("fn z(x: int) -> u64 {\n"
          "    return x as u32 as u64;\n"
          "}\n",
          TARGET_LINUX_ARM64,
          "main.z:\n"
          "b0:\n"
          "    mov w0, w0\n"
          "    ret\n");
}
