#include "check.h"
#include "arena.h"
#include "ast.h"
#include "diagnostic.h"
#include "ir.h"
#include "lexer.h"
#include "lower.h"
#include "optimize.h"
#include "parser.h"
#include "sema.h"
#include "types.h"

/* Lower source as module main, optimize it and compare the printed IR.
   The verifier runs before and after the optimizer. */
static void optimizes(const char *source, const char *expected)
{
    struct arena arena = {0};
    struct diagnostics diags = {0};
    struct token_list tokens = {0};
    struct module *module = NULL;
    struct types types;
    struct ir_module ir;
    struct text out = {0};
    struct text errors = {0};

    types_init(&types, &arena);
    ir_module_init(&ir, &arena, "main");
    if (!lex(source, strlen(source), &arena, &diags, &tokens) ||
        !parse(source, &tokens, &arena, &diags, &module) ||
        !sema_check(module, "main", NULL, 0, &types, &arena, &diags) ||
        !lower_module(module, "main", &ir, &diags)) {
        check_failures++;
        fprintf(stderr, "test source does not lower: %s\n%s\n",
                diags.count > 0 ? diags.items[0].message : "", source);
    } else if (!ir_verify(&ir, &errors)) {
        check_failures++;
        fprintf(stderr, "lowered IR fails verification:\n%s",
                text_cstr(&errors));
    } else {
        ir_optimize(&ir, "main");
        ir_print(&out, &ir);
        CHECK_STR(text_cstr(&out), expected);
        if (!ir_verify(&ir, &errors)) {
            check_failures++;
            fprintf(stderr, "optimized IR fails verification:\n%s",
                    text_cstr(&errors));
        }
    }
    text_free(&out);
    text_free(&errors);
    ir_module_free(&ir);
    token_list_free(&tokens);
    diagnostics_free(&diags);
    arena_free(&arena);
}

/* A module compiled on its own keeps each of its functions, and the bodies
   of other modules become declarations. */
static void one_module(void)
{
    struct arena arena = {0};
    struct ir_module m;
    struct ir_function *lib;
    struct ir_function *unused;
    struct ir_function *main_fn;
    struct ir_block *b;
    struct text out = {0};
    struct ir_operand arg;
    uint32_t v;

    ir_module_init(&m, &arena, "main");
    lib = ir_function_add(&m, "com.example.lib", "seven", IR_I64, IR_NO_AGG);
    ir_param_add(lib, IR_I64, IR_NO_AGG);
    b = ir_block_add(lib);
    ir_ret(lib, b, IR_I64, ir_int_op(IR_I64, 7));
    unused = ir_function_add(&m, "main", "unused", IR_I64, IR_NO_AGG);
    b = ir_block_add(unused);
    ir_ret(unused, b, IR_I64, ir_int_op(IR_I64, 1));
    main_fn = ir_function_add(&m, "main", "main", IR_I64, IR_NO_AGG);
    b = ir_block_add(main_fn);
    arg = ir_int_op(IR_I64, 3);
    v = ir_call(main_fn, b, IR_I64, ir_func_op(lib), &arg, 1);
    ir_ret(main_fn, b, IR_I64, ir_temp_op(main_fn, v));
    ir_optimize_module(&m, "main");
    ir_print(&out, &m);
    CHECK_STR(text_cstr(&out),
              "extern fn com.example.lib.seven(i64) -> i64\n"
              "fn main.unused() -> i64 {\n"
              "b0:\n"
              "    ret i64 1\n"
              "}\n"
              "fn main.main() -> i64 {\n"
              "b0:\n"
              "    %0 = call i64 @com.example.lib.seven(3)\n"
              "    ret i64 %0\n"
              "}\n");
    text_free(&out);
    ir_module_free(&m);
    arena_free(&arena);
}

/* Three blocks where b1 and b2 jump to each other. A search for the end
   of the jumps that stops after a fixed number of steps would alternate
   between b1 and b2 forever. */
static void jump_cycle(void)
{
    struct arena arena = {0};
    struct ir_module m;
    struct ir_function *f;
    struct ir_block *b0, *b1, *b2;
    struct text out = {0};

    ir_module_init(&m, &arena, "main");
    f = ir_function_add(&m, "main", "loop", IR_VOID, IR_NO_AGG);
    b0 = ir_block_add(f);
    b1 = ir_block_add(f);
    b2 = ir_block_add(f);
    ir_jump(f, b0, b1);
    ir_jump(f, b1, b2);
    ir_jump(f, b2, b1);
    ir_optimize(&m, "main");
    ir_print(&out, &m);
    CHECK_STR(text_cstr(&out), "fn main.loop() {\n"
                               "b0:\n"
                               "    jump b1\n"
                               "b1:\n"
                               "    jump b1\n"
                               "}\n");
    text_free(&out);
    ir_module_free(&m);
    arena_free(&arena);
}

void test_optimize(void)
{
    one_module();
    jump_cycle();
    /* The function scale of chapter 1. */
    optimizes("fn scale(x: int) -> int {\n"
              "    let k = 2 + 4;\n"
              "    return x * k;\n"
              "}\n",
              "fn main.scale(%0: i64) -> i64 {\n"
              "b0:\n"
              "    %1 = mul i64 %0, 6\n"
              "    ret i64 %1\n"
              "}\n");

    /* Folding wraps and truncates as the target does. */
    /* An operation on a target-sized type wraps at a width that only the
       back end knows, so the optimizer leaves it. */
    optimizes("fn f() -> c_long {\n"
              "    let x: c_long = 65536;\n"
              "    return x * 70000;\n"
              "}\n",
              "fn main.f() -> clong {\n"
              "b0:\n"
              "    %0 = mul clong 65536, 70000\n"
              "    ret clong %0\n"
              "}\n");
    /* A size is symbolic. The optimizer never folds it, and the operations
       around it stay for the back end. */
    optimizes("struct H { tag: u8, n: i32 }\n"
              "fn f() -> int {\n"
              "    let n = size_of(H);\n"
              "    return n * 1 + 4 - 4;\n"
              "}\n",
              "type main.H = struct { tag: i8, n: i32 }\n"
              "fn main.f() -> i64 {\n"
              "b0:\n"
              "    %0 = add i64 size_of main.H, 4\n"
              "    %1 = sub i64 %0, 4\n"
              "    ret i64 %1\n"
              "}\n");
    optimizes("fn f1() -> int { return 7 * 6 - 2; }\n"
              "fn f2() -> u8 { let b: u8 = 250; return b + 10; }\n"
              "fn f3() -> int { return -9 / 2 + -9 % 2; }\n"
              "fn f4() -> i32 { return (-16 >> 2) as i32; }\n"
              "fn f5() -> u32 { let s: u32 = 4000000000; return s >> 4; }\n"
              "fn f6() -> int { return (1.5 * 2.0) as int + (3.9 as int); }\n"
              "fn f7() -> bool { return 3 < 4 && 2.0 != 2.0; }\n",
              "fn main.f1() -> i64 {\nb0:\n    ret i64 40\n}\n"
              "fn main.f2() -> i8 {\nb0:\n    ret i8 4\n}\n"
              "fn main.f3() -> i64 {\nb0:\n    ret i64 -5\n}\n"
              "fn main.f4() -> i32 {\nb0:\n    ret i32 -4\n}\n"
              "fn main.f5() -> i32 {\nb0:\n    ret i32 250000000\n}\n"
              "fn main.f6() -> i64 {\nb0:\n    ret i64 6\n}\n"
              "fn main.f7() -> i8 {\nb0:\n    ret i8 0\n}\n");

    /* Undefined cases stay for the target to execute. */
    optimizes("fn u1() -> int { let z = 0; return 1 / z; }\n"
              "fn u2() -> int { let n = 64; return 1 << n; }\n"
              "fn u3() -> i8 { let m: i8 = -128; return m / -1; }\n"
              "fn u4() -> i32 { let x = 30000000000.0; return x as i32; }\n",
              "fn main.u1() -> i64 {\nb0:\n"
              "    %0 = sdiv i64 1, 0\n    ret i64 %0\n}\n"
              "fn main.u2() -> i64 {\nb0:\n"
              "    %0 = shl i64 1, 64\n    ret i64 %0\n}\n"
              "fn main.u3() -> i8 {\nb0:\n"
              "    %0 = sdiv i8 -128, -1\n    ret i8 %0\n}\n"
              "fn main.u4() -> i32 {\nb0:\n"
              "    %0 = ftosi i32 30000000000\n    ret i32 %0\n}\n");

    /* Identities, strength reduction and constants on the right. */
    optimizes("fn p1(x: int) -> int {\n"
              "    return (x + 0) * 8 + 2 * x - x * 1;\n"
              "}\n"
              "fn p2(x: u16) -> u16 {\n"
              "    return (x & 0) | (x ^ 0) + (x >> 0) / 1;\n"
              "}\n",
              "fn main.p1(%0: i64) -> i64 {\n"
              "b0:\n"
              "    %1 = shl i64 %0, 3\n"
              "    %2 = shl i64 %0, 1\n"
              "    %3 = add i64 %1, %2\n"
              "    %4 = sub i64 %3, %0\n"
              "    ret i64 %4\n"
              "}\n"
              "fn main.p2(%0: i16 zeroext) -> i16 {\n"
              "b0:\n"
              "    %1 = add i16 %0, %0\n"
              "    ret i16 %1\n"
              "}\n");

    /* A variable assigned several times stays, and the add moves into the
       assignment. */
    optimizes("fn g(n: int) -> int {\n"
              "    let i = 0;\n"
              "    while i < n do {\n"
              "        i += 1;\n"
              "    }\n"
              "    if i > 3 {\n"
              "        return 1;\n"
              "    } else {\n"
              "        return 0;\n"
              "    }\n"
              "}\n"
              "fn l1(x: int) -> int {\n"
              "    let a = x;\n"
              "    a = a + 1;\n"
              "    let b = a;\n"
              "    return b * 2;\n"
              "}\n",
              "fn main.g(%0: i64) -> i64 {\n"
              "b0:\n"
              "    %1 = copy i64 0\n"
              "    jump b1\n"
              "b1:\n"
              "    %2 = slt i8 %1, %0\n"
              "    branch %2, b2, b3\n"
              "b2:\n"
              "    %1 = add i64 %1, 1\n"
              "    jump b1\n"
              "b3:\n"
              "    %3 = sgt i8 %1, 3\n"
              "    branch %3, b4, b5\n"
              "b4:\n"
              "    ret i64 1\n"
              "b5:\n"
              "    ret i64 0\n"
              "}\n"
              "fn main.l1(%0: i64) -> i64 {\n"
              "b0:\n"
              "    %1 = add i64 %0, 1\n"
              "    %2 = shl i64 %1, 1\n"
              "    ret i64 %2\n"
              "}\n");

    /* Jumps to blocks that only jump go straight to the target. */
    optimizes("fn count(n: int) -> int {\n"
              "    let i = 0;\n"
              "    do {\n"
              "        i += 1;\n"
              "        if i == 5 {\n"
              "            continue;\n"
              "        }\n"
              "        if i > 8 {\n"
              "            break;\n"
              "        }\n"
              "    } while i < n\n"
              "    return i;\n"
              "}\n",
              "fn main.count(%0: i64) -> i64 {\n"
              "b0:\n"
              "    %1 = copy i64 0\n"
              "    jump b1\n"
              "b1:\n"
              "    %1 = add i64 %1, 1\n"
              "    %2 = eq i8 %1, 5\n"
              "    branch %2, b2, b4\n"
              "b2:\n"
              "    %3 = slt i8 %1, %0\n"
              "    branch %3, b1, b3\n"
              "b3:\n"
              "    ret i64 %1\n"
              "b4:\n"
              "    %4 = sgt i8 %1, 8\n"
              "    branch %4, b3, b2\n"
              "}\n");

    /* Blocks that jump to each other end the search for a jump target. */
    optimizes("fn spin() {\n"
              "    while true do {\n"
              "    }\n"
              "}\n",
              "fn main.spin() {\n"
              "b0:\n"
              "    jump b1\n"
              "b1:\n"
              "    jump b1\n"
              "}\n");

    /* An i64 converts to f32 in one rounding step. 2^60 + 2^36 + 1 lies
       just above the midpoint of two f32 values and rounds up, while
       rounding through f64 first lands on the midpoint and rounds down. */
    optimizes("fn r() -> f32 {\n"
              "    return 1152921573326323713 as f32;\n"
              "}\n",
              "fn main.r() -> f32 {\n"
              "b0:\n"
              "    ret f32 1.1529216420458004e+18\n"
              "}\n");

    /* Memory is never assumed to keep a value between two instructions. */
    optimizes("fn bump(x: i8) -> i16 {\n"
              "    let p = &x;\n"
              "    *p -= -128;\n"
              "    return x as i16 + (x as u8 as i16);\n"
              "}\n",
              "fn main.bump(%0: i8 signext) -> i16 {\n"
              "b0:\n"
              "    %1 = slot i8\n"
              "    store i8 %0, %1\n"
              "    %2 = load i8 %1\n"
              "    %3 = sub i8 %2, -128\n"
              "    store i8 %3, %1\n"
              "    %4 = load i8 %1\n"
              "    %5 = sext i16 %4\n"
              "    %6 = load i8 %1\n"
              "    %7 = zext i16 %6\n"
              "    %8 = add i16 %5, %7\n"
              "    ret i16 %8\n"
              "}\n");

    /* With a main, only what main reaches stays in the program. */
    optimizes("extern fn putchar(c: i32) -> i32;\n"
              "fn helper() -> int { return 1; }\n"
              "fn used() -> int { return 2; }\n"
              "fn main() -> int { return used(); }\n",
              "fn main.used() -> i64 {\n"
              "b0:\n"
              "    ret i64 2\n"
              "}\n"
              "fn main.main() -> i64 {\n"
              "b0:\n"
              "    %0 = call i64 @main.used()\n"
              "    ret i64 %0\n"
              "}\n");
}
