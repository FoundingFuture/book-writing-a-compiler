#include "check.h"
#include "arena.h"
#include "ir.h"

static void printed(const struct ir_module *m, const char *expected)
{
    struct text out = {0};
    ir_print(&out, m);
    CHECK_STR(text_cstr(&out), expected);
    text_free(&out);
}

static void verified(const struct ir_module *m, const char *expected_errors)
{
    struct text errors = {0};
    bool ok = ir_verify(m, &errors);
    CHECK(ok == (expected_errors[0] == '\0'));
    CHECK_STR(text_cstr(&errors), expected_errors);
    text_free(&errors);
}

/* The function scale of chapter 1 after constant folding. */
static void scale(void)
{
    struct arena arena = {0};
    struct ir_module m;
    struct ir_function *f;
    struct ir_block *b0;
    uint32_t x;
    uint32_t product;

    ir_module_init(&m, &arena, "main");
    f = ir_function_add(&m, "main", "scale", IR_I64, IR_NO_AGG);
    x = ir_param_add(f, IR_I64, IR_NO_AGG);
    b0 = ir_block_add(f);
    product = ir_binary(f, b0, IR_MUL, IR_I64, ir_temp_op(f, x),
                        ir_int_op(IR_I64, 6));
    ir_ret(f, b0, IR_I64, ir_temp_op(f, product));
    printed(&m, "fn main.scale(%0: i64) -> i64 {\n"
                "b0:\n"
                "    %1 = mul i64 %0, 6\n"
                "    ret i64 %1\n"
                "}\n");
    verified(&m, "");
    ir_module_free(&m);
    arena_free(&arena);
}

/* A loop that sums 0 to n - 1, with two temporaries assigned many times. */
static void loop(void)
{
    struct arena arena = {0};
    struct ir_module m;
    struct ir_function *f;
    struct ir_block *entry, *test, *body, *done;
    uint32_t n, total, i, less;

    ir_module_init(&m, &arena, "main");
    f = ir_function_add(&m, "main", "sum", IR_I64, IR_NO_AGG);
    n = ir_param_add(f, IR_I64, IR_NO_AGG);
    entry = ir_block_add(f);
    test = ir_block_add(f);
    body = ir_block_add(f);
    done = ir_block_add(f);
    total = ir_unary(f, entry, IR_COPY, IR_I64, ir_int_op(IR_I64, 0));
    i = ir_unary(f, entry, IR_COPY, IR_I64, ir_int_op(IR_I64, 0));
    ir_jump(f, entry, test);
    less = ir_binary(f, test, IR_SLT, IR_I8, ir_temp_op(f, i),
                     ir_temp_op(f, n));
    ir_branch(f, test, ir_temp_op(f, less), body, done);
    ir_assign(f, body, total,
              ir_temp_op(f, ir_binary(f, body, IR_ADD, IR_I64,
                                      ir_temp_op(f, total), ir_temp_op(f, i))));
    ir_assign(f, body, i,
              ir_temp_op(f, ir_binary(f, body, IR_ADD, IR_I64,
                                      ir_temp_op(f, i), ir_int_op(IR_I64, 1))));
    ir_jump(f, body, test);
    ir_ret(f, done, IR_I64, ir_temp_op(f, total));
    printed(&m, "fn main.sum(%0: i64) -> i64 {\n"
                "b0:\n"
                "    %1 = copy i64 0\n"
                "    %2 = copy i64 0\n"
                "    jump b1\n"
                "b1:\n"
                "    %3 = slt i8 %2, %0\n"
                "    branch %3, b2, b3\n"
                "b2:\n"
                "    %4 = add i64 %1, %2\n"
                "    %1 = copy i64 %4\n"
                "    %5 = add i64 %2, 1\n"
                "    %2 = copy i64 %5\n"
                "    jump b1\n"
                "b3:\n"
                "    ret i64 %1\n"
                "}\n");
    verified(&m, "");
    ir_module_free(&m);
    arena_free(&arena);
}

/* A str value in a stack slot, passed to an extern C function. The slot
   names its type and the length field its offset, never a number. */
static void memory(void)
{
    struct arena arena = {0};
    struct ir_module m;
    struct ir_function *puts_fn, *f;
    struct ir_global *text;
    struct ir_block *b0;
    struct ir_operand arg;
    struct ir_field fields[2];
    uint32_t str;
    uint32_t slot, bytes, len_field, ptr, status, wide;
    static const uint8_t hi[] = {'h', 'i', 0};

    ir_module_init(&m, &arena, "main");
    memset(fields, 0, sizeof fields);
    fields[0].name = "ptr";
    fields[0].type = ir_scalar(IR_PTR);
    fields[1].name = "len";
    fields[1].type = ir_scalar(IR_I64);
    str = ir_struct_add(&m, IR_AGG_STRUCT, "str", fields, 2, false, 0);
    puts_fn = ir_extern_add(&m, "puts", IR_I32, false);
    ir_param_add(puts_fn, IR_PTR, IR_NO_AGG);
    text = ir_global_add(&m, "main", "str.0", hi, sizeof hi, 1);
    f = ir_function_add(&m, "main", "main", IR_I64, IR_NO_AGG);
    b0 = ir_block_add(f);
    slot = ir_slot(f, b0, ir_aggregate(str));
    bytes = ir_addr(f, b0, ir_global_op(text));
    ir_store(f, b0, IR_PTR, ir_temp_op(f, bytes), ir_temp_op(f, slot));
    len_field = ir_ptradd(f, b0, ir_temp_op(f, slot),
                          ir_sym_operand(&m, ir_sym_offset_of(&m, str, 1)));
    ir_store(f, b0, IR_I64, ir_int_op(IR_I64, 2), ir_temp_op(f, len_field));
    ptr = ir_load(f, b0, IR_PTR, ir_temp_op(f, slot));
    arg = ir_temp_op(f, ptr);
    status = ir_call(f, b0, IR_I32, ir_func_op(puts_fn), &arg, 1);
    wide = ir_unary(f, b0, IR_SEXT, IR_I64, ir_temp_op(f, status));
    ir_ret(f, b0, IR_I64, ir_temp_op(f, wide));
    printed(&m, "type str = struct { ptr: ptr, len: i64 }\n"
                "extern fn puts(ptr) -> i32\n"
                "global main.str.0 size 3 align 1 bytes 68 69 00\n"
                "fn main.main() -> i64 {\n"
                "b0:\n"
                "    %0 = slot str\n"
                "    %1 = addr @main.str.0\n"
                "    store ptr %1, %0\n"
                "    %2 = ptradd %0, offset_of str.len\n"
                "    store i64 2, %2\n"
                "    %3 = load ptr %0\n"
                "    %4 = call i32 @puts(%3)\n"
                "    %5 = sext i64 %4\n"
                "    ret i64 %5\n"
                "}\n");
    verified(&m, "");
    ir_module_free(&m);
    arena_free(&arena);
}

/* Symbolic values name sizes and offsets and combine with operations.
   Equal values share one entry, and an entry for a number is an integer
   operand. */
static void symbolic(void)
{
    struct arena arena = {0};
    struct ir_module m;
    struct ir_function *f;
    struct ir_block *b0;
    struct ir_field fields[2];
    uint32_t foo, size, length, bytes, x, scaled, sum, copy;

    ir_module_init(&m, &arena, "main");
    memset(fields, 0, sizeof fields);
    fields[0].name = "a";
    fields[0].type = ir_scalar(IR_I8);
    fields[1].name = "b";
    fields[1].type = ir_scalar(IR_I32);
    foo = ir_struct_add(&m, IR_AGG_STRUCT, "main.Foo", fields, 2, false, 0);
    CHECK(ir_struct_add(&m, IR_AGG_STRUCT, "main.Foo", fields, 2, false, 0) ==
          foo);
    size = ir_sym_size_of(&m, ir_aggregate(foo));
    CHECK(ir_sym_size_of(&m, ir_aggregate(foo)) == size);
    length = ir_sym_op(&m, IR_SUB, IR_I64, size, ir_sym_int(&m, IR_I64, 1));
    bytes = ir_array_add(&m, "[size_of(main.Foo) - 1]byte", ir_scalar(IR_I8),
                         length, "size_of(Foo) - 1");
    CHECK(ir_sym_operand(&m, ir_sym_int(&m, IR_I64, 7)).kind == IR_INT);
    f = ir_function_add(&m, "main", "f", IR_I64, IR_NO_AGG);
    x = ir_param_add(f, IR_I64, IR_NO_AGG);
    b0 = ir_block_add(f);
    copy = ir_slot(f, b0, ir_aggregate(bytes));
    scaled = ir_binary(f, b0, IR_MUL, IR_I64, ir_temp_op(f, x),
                       ir_sym_operand(&m, size));
    sum = ir_binary(f, b0, IR_ADD, IR_I64, ir_temp_op(f, scaled),
                    ir_sym_operand(&m, length));
    ir_memcopy(f, b0, ir_temp_op(f, copy), ir_temp_op(f, copy),
               ir_aggregate(bytes));
    ir_ret(f, b0, IR_I64, ir_temp_op(f, sum));
    printed(&m, "type main.Foo = struct { a: i8, b: i32 }\n"
                "type [size_of(main.Foo) - 1]byte = array "
                "sub i64(size_of main.Foo, 1) of i8\n"
                "fn main.f(%0: i64) -> i64 {\n"
                "b0:\n"
                "    %1 = slot [size_of(main.Foo) - 1]byte\n"
                "    %2 = mul i64 %0, size_of main.Foo\n"
                "    %3 = add i64 %2, sub i64(size_of main.Foo, 1)\n"
                "    memcopy %1, %1, [size_of(main.Foo) - 1]byte\n"
                "    ret i64 %3\n"
                "}\n");
    verified(&m, "");
    ir_module_free(&m);
    arena_free(&arena);
}

static void verifier(void)
{
    struct arena arena = {0};
    struct ir_module m;
    struct ir_function *f;
    struct ir_block *b0, *b1;
    uint32_t x, y;

    ir_module_init(&m, &arena, "main");
    f = ir_function_add(&m, "main", "f", IR_I64, IR_NO_AGG);
    x = ir_param_add(f, IR_I64, IR_NO_AGG);
    y = ir_param_add(f, IR_I32, IR_NO_AGG);
    b0 = ir_block_add(f);
    b1 = ir_block_add(f);
    ir_binary(f, b0, IR_ADD, IR_I64, ir_temp_op(f, x), ir_temp_op(f, y));
    ir_ret(f, b1, IR_I32, ir_temp_op(f, y));
    verified(&m, "main.f b0: add i64 has an operand of type i32\n"
                 "main.f b0: the block does not end with a terminator\n"
                 "main.f b1: ret i32 in a function that returns i64\n");
    ir_module_free(&m);
    arena_free(&arena);
}

/* A call through a pointer passes the arguments of its signature. */
static void indirect_arguments(void)
{
    struct arena arena = {0};
    struct ir_module m;
    struct ir_function *sig;
    struct ir_function *f;
    struct ir_block *b0;
    uint32_t target;
    uint32_t result;

    ir_module_init(&m, &arena, "main");
    sig = ir_declare_add(&m, "main", "fn.0", IR_I64, IR_NO_AGG);
    ir_param_add(sig, IR_I64, IR_NO_AGG);
    f = ir_function_add(&m, "main", "f", IR_I64, IR_NO_AGG);
    target = ir_param_add(f, IR_PTR, IR_NO_AGG);
    b0 = ir_block_add(f);
    result = ir_call_indirect(f, b0, IR_I64, ir_temp_op(f, target), sig, NULL,
                              0);
    ir_ret(f, b0, IR_I64, ir_temp_op(f, result));
    verified(&m, "main.f b0: call passes 0 arguments to fn.0, which takes 1\n");
    ir_module_free(&m);
    arena_free(&arena);
}

/* An integer constant keeps only the bits of its type. */
static void constants(void)
{
    CHECK(ir_int_op(IR_I8, (uint64_t)-1).as.integer == 0xff);
    CHECK(ir_int_op(IR_I16, 0x12345).as.integer == 0x2345);
    CHECK(ir_int_op(IR_I32, (uint64_t)-2).as.integer == 0xfffffffe);
    CHECK(ir_int_op(IR_I64, (uint64_t)-2).as.integer == (uint64_t)-2);
}

/* A temporary is used before any definition on the path through b1. */
static void unassigned(void)
{
    struct arena arena = {0};
    struct ir_module m;
    struct ir_function *f;
    struct ir_block *b0, *b1, *b2;
    uint32_t c, x;

    ir_module_init(&m, &arena, "main");
    f = ir_function_add(&m, "main", "f", IR_I64, IR_NO_AGG);
    c = ir_param_add(f, IR_I8, IR_NO_AGG);
    b0 = ir_block_add(f);
    b1 = ir_block_add(f);
    b2 = ir_block_add(f);
    ir_branch(f, b0, ir_temp_op(f, c), b1, b2);
    x = ir_unary(f, b1, IR_COPY, IR_I64, ir_int_op(IR_I64, 1));
    ir_jump(f, b1, b2);
    ir_ret(f, b2, IR_I64, ir_temp_op(f, x));
    verified(&m, "main.f b2: ret uses %1 before a definition on some path\n");
    ir_module_free(&m);
    arena_free(&arena);
}

void test_ir(void)
{
    constants();
    indirect_arguments();
    unassigned();
    scale();
    loop();
    memory();
    symbolic();
    verifier();
}
