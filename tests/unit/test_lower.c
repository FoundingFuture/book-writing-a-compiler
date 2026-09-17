#include "check.h"
#include "arena.h"
#include "ast.h"
#include "diagnostic.h"
#include "ir.h"
#include "lexer.h"
#include "lower.h"
#include "parser.h"
#include "sema.h"
#include "types.h"

struct lowered {
    struct arena arena;
    struct diagnostics diags;
    struct token_list tokens;
    struct module *module;
    struct types types;
    struct ir_module ir;
    bool ok;
};

static void run(struct lowered *l, const char *source)
{
    memset(l, 0, sizeof *l);
    types_init(&l->types, &l->arena);
    ir_module_init(&l->ir, &l->arena, "main");
    if (!lex(source, strlen(source), &l->arena, &l->diags, &l->tokens) ||
        !parse(source, &l->tokens, &l->arena, &l->diags, &l->module) ||
        !sema_check(l->module, "main", NULL, 0, &l->types, &l->arena,
                    &l->diags)) {
        fprintf(stderr, "test source does not check: %d:%d: %s\n%s\n",
                l->diags.items[0].line, l->diags.items[0].column,
                l->diags.items[0].message, source);
        check_failures++;
        return;
    }
    l->ok = lower_module(l->module, "main", &l->ir, &l->diags);
}

static void release(struct lowered *l)
{
    ir_module_free(&l->ir);
    token_list_free(&l->tokens);
    diagnostics_free(&l->diags);
    arena_free(&l->arena);
}

static void lowers(const char *source, const char *expected)
{
    struct lowered l;
    struct text out = {0};
    struct text errors = {0};

    run(&l, source);
    CHECK(l.ok);
    ir_print(&out, &l.ir);
    CHECK_STR(text_cstr(&out), expected);
    if (!ir_verify(&l.ir, &errors)) {
        check_failures++;
        fprintf(stderr, "verifier:\n%s", text_cstr(&errors));
    }
    text_free(&out);
    text_free(&errors);
    release(&l);
}

void test_lower(void)
{
    /* A bitfield is read and written through the address of its struct and
       the field, and the back end lowers both to shifts and masks. */
    lowers("struct Flags { visible: u32 : 1, level: i8 : 3 }\n"
           "fn f(p: *Flags) -> i8 {\n"
           "    p.visible = 1;\n"
           "    p.level -= 2;\n"
           "    return p.level;\n"
           "}\n",
           "type main.Flags = struct { visible: i32 : 1 zeroext, "
           "level: i8 : 3 signext }\n"
           "fn main.f(%0: ptr) -> i8 {\n"
           "b0:\n"
           "    bitstore i32 1, %0, main.Flags.visible\n"
           "    %1 = bitload i8 %0, main.Flags.level\n"
           "    %2 = sub i8 %1, 2\n"
           "    bitstore i8 %2, %0, main.Flags.level\n"
           "    %3 = bitload i8 %0, main.Flags.level\n"
           "    ret i8 %3\n"
           "}\n");
    /* packed and align(N) reach the type table, and the back end lays the
       types out. */
    lowers("packed struct P { a: u8, b: i32 }\n"
           "struct A align(16) { a: u8 }\n"
           "fn f(p: *P, a: A) -> i32 {\n"
           "    return p.b + a.a as i32;\n"
           "}\n",
           "type main.A = struct align(16) { a: i8 }\n"
           "type main.P = packed struct { a: i8, b: i32 }\n"
           "fn main.f(%0: ptr, %1: agg main.A) -> i32 {\n"
           "b0:\n"
           "    %2 = ptradd %0, offset_of main.P.b\n"
           "    %3 = load i32 %2\n"
           "    %4 = load i8 %1\n"
           "    %5 = zext i32 %4\n"
           "    %6 = add i32 %3, %5\n"
           "    ret i32 %6\n"
           "}\n");
    /* Every field of a union lies at offset 0. */
    lowers("union Value { i: int, f: f64 }\n"
           "fn bits(x: f64) -> int {\n"
           "    let v = Value { f: x };\n"
           "    return v.i;\n"
           "}\n",
           "type main.Value = union { i: i64, f: f64 }\n"
           "fn main.bits(%0: f64) -> i64 {\n"
           "b0:\n"
           "    %1 = slot main.Value\n"
           "    store f64 %0, %1\n"
           "    %2 = load i64 %1\n"
           "    ret i64 %2\n"
           "}\n");
    /* A target-sized C type has an IR type of its own. A conversion extends
       from a type that is never wider and truncates to one that is never
       wider. */
    lowers("extern fn labs(n: c_long) -> c_long;\n"
           "fn f(a: i32, w: c_wchar) -> i64 {\n"
           "    let x = labs(a as c_long) + 1;\n"
           "    return x as i64 + w as i64 + (x as i32) as i64;\n"
           "}\n",
           "extern fn labs(clong) -> clong\n"
           "fn main.f(%0: i32, %1: cwchar) -> i64 {\n"
           "b0:\n"
           "    %2 = sext clong %0\n"
           "    %3 = call clong @labs(%2)\n"
           "    %4 = add clong %3, 1\n"
           "    %5 = copy clong %4\n"
           "    %6 = sext i64 %5\n"
           "    %7 = zext i64 %1\n"
           "    %8 = add i64 %6, %7\n"
           "    %9 = trunc i32 %5\n"
           "    %10 = sext i64 %9\n"
           "    %11 = add i64 %8, %10\n"
           "    ret i64 %11\n"
           "}\n");
    /* The length, the stride and .len of an array whose length comes from
       size_of are symbolic values. */
    lowers("struct H { tag: u8, n: i32 }\n"
           "fn f() -> int {\n"
           "    let a: [size_of(H) - 4]byte = [7; size_of(H) - 4];\n"
           "    return a.len + a[1] as int;\n"
           "}\n",
           "type main.H = struct { tag: i8, n: i32 }\n"
           "type [size_of(main.H) - 4]byte = array "
           "sub i64(size_of main.H, 4) of i8\n"
           "fn main.f() -> i64 {\n"
           "b0:\n"
           "    %0 = slot [size_of(main.H) - 4]byte\n"
           "    %1 = copy i64 0\n"
           "    jump b1\n"
           "b1:\n"
           "    %2 = slt i8 %1, sub i64(size_of main.H, 4)\n"
           "    branch %2, b2, b3\n"
           "b2:\n"
           "    %3 = mul i64 %1, size_of i8\n"
           "    %4 = ptradd %0, %3\n"
           "    store i8 7, %4\n"
           "    %5 = add i64 %1, 1\n"
           "    %1 = copy i64 %5\n"
           "    jump b1\n"
           "b3:\n"
           "    %6 = mul i64 1, size_of i8\n"
           "    %7 = ptradd %0, %6\n"
           "    %8 = load i8 %7\n"
           "    %9 = zext i64 %8\n"
           "    %10 = add i64 sub i64(size_of main.H, 4), %9\n"
           "    ret i64 %10\n"
           "}\n");
    lowers("fn scale(x: int) -> int {\n"
           "    let k = 2 + 4;\n"
           "    return x * k;\n"
           "}\n"
           "fn main() -> int {\n"
           "    return scale(7);\n"
           "}\n",
           "fn main.scale(%0: i64) -> i64 {\n"
           "b0:\n"
           "    %1 = add i64 2, 4\n"
           "    %2 = copy i64 %1\n"
           "    %3 = mul i64 %0, %2\n"
           "    ret i64 %3\n"
           "}\n"
           "fn main.main() -> i64 {\n"
           "b0:\n"
           "    %0 = call i64 @main.scale(7)\n"
           "    ret i64 %0\n"
           "}\n");

    /* A local whose address is taken lives in a stack slot. */
    lowers("fn f() -> int {\n"
           "    let x = 5;\n"
           "    let p = &x;\n"
           "    *p = *p + 1;\n"
           "    return x;\n"
           "}\n",
           "fn main.f() -> i64 {\n"
           "b0:\n"
           "    %0 = slot i64\n"
           "    store i64 5, %0\n"
           "    %1 = copy ptr %0\n"
           "    %2 = load i64 %1\n"
           "    %3 = add i64 %2, 1\n"
           "    store i64 %3, %1\n"
           "    %4 = load i64 %0\n"
           "    ret i64 %4\n"
           "}\n");

    /* A while loop and an if chain whose branches all return. */
    lowers("fn g(n: int) -> int {\n"
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
           "fn main.g(%0: i64) -> i64 {\n"
           "b0:\n"
           "    %1 = copy i64 0\n"
           "    jump b1\n"
           "b1:\n"
           "    %2 = slt i8 %1, %0\n"
           "    branch %2, b2, b3\n"
           "b2:\n"
           "    %3 = add i64 %1, 1\n"
           "    %1 = copy i64 %3\n"
           "    jump b1\n"
           "b3:\n"
           "    %4 = sgt i8 %1, 3\n"
           "    branch %4, b4, b5\n"
           "b4:\n"
           "    ret i64 1\n"
           "b5:\n"
           "    ret i64 0\n"
           "}\n");

    /* && evaluates its right operand only when the left one is true. */
    lowers("fn h(a: u32, b: u32) -> bool {\n"
           "    return a / b > 1 && a != b;\n"
           "}\n",
           "fn main.h(%0: i32, %1: i32) -> i8 {\n"
           "b0:\n"
           "    %2 = udiv i32 %0, %1\n"
           "    %3 = ugt i8 %2, 1\n"
           "    %4 = copy i8 %3\n"
           "    branch %3, b1, b2\n"
           "b1:\n"
           "    %5 = ne i8 %0, %1\n"
           "    %4 = copy i8 %5\n"
           "    jump b2\n"
           "b2:\n"
           "    ret i8 %4\n"
           "}\n");

    /* Conversions between types of one IR type need no instruction. */
    lowers("extern fn putchar(c: i32) -> i32;\n"
           "fn main() -> int {\n"
           "    let c: char = 'A';\n"
           "    putchar(c as u32 as i32);\n"
           "    return 0;\n"
           "}\n",
           "extern fn putchar(i32) -> i32\n"
           "fn main.main() -> i64 {\n"
           "b0:\n"
           "    %0 = copy i32 65\n"
           "    %1 = call i32 @putchar(%0)\n"
           "    ret i64 0\n"
           "}\n");

    /* alloc and free call the C library, and p[i] is an address. */
    lowers("fn k() -> i16 {\n"
           "    let p = alloc(i16, 4);\n"
           "    p[2] = 7;\n"
           "    let v = p[2];\n"
           "    free(p);\n"
           "    return v + size_of(i16) as i16;\n"
           "}\n",
           "extern fn malloc(i64) -> ptr\n"
           "extern fn free(ptr)\n"
           "fn main.k() -> i16 {\n"
           "b0:\n"
           "    %0 = mul i64 4, size_of i16\n"
           "    %1 = call ptr @malloc(%0)\n"
           "    %2 = copy ptr %1\n"
           "    %3 = mul i64 2, size_of i16\n"
           "    %4 = ptradd %2, %3\n"
           "    store i16 7, %4\n"
           "    %5 = mul i64 2, size_of i16\n"
           "    %6 = ptradd %2, %5\n"
           "    %7 = load i16 %6\n"
           "    %8 = copy i16 %7\n"
           "    call void @free(%2)\n"
           "    %9 = trunc i16 size_of i16\n"
           "    %10 = add i16 %8, %9\n"
           "    ret i16 %10\n"
           "}\n");

    lowers("fn m(x: f32) -> f64 {\n"
           "    return (x * 2.0) as f64 + 0.5;\n"
           "}\n",
           "fn main.m(%0: f32) -> f64 {\n"
           "b0:\n"
           "    %1 = fmul f32 %0, 2\n"
           "    %2 = fext f64 %1\n"
           "    %3 = fadd f64 %2, 0.5\n"
           "    ret f64 %3\n"
           "}\n");

    /* A function without a result returns at its end. */
    lowers("fn z(n: int) {\n"
           "    if n > 0 {\n"
           "        return;\n"
           "    }\n"
           "}\n",
           "fn main.z(%0: i64) {\n"
           "b0:\n"
           "    %1 = sgt i8 %0, 0\n"
           "    branch %1, b1, b2\n"
           "b1:\n"
           "    ret\n"
           "b2:\n"
           "    ret\n"
           "}\n");

    /* continue goes to the condition of a do while loop. */
    lowers("fn count(n: int) -> int {\n"
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
           "    %2 = add i64 %1, 1\n"
           "    %1 = copy i64 %2\n"
           "    %3 = eq i8 %1, 5\n"
           "    branch %3, b4, b5\n"
           "b2:\n"
           "    %5 = slt i8 %1, %0\n"
           "    branch %5, b1, b3\n"
           "b3:\n"
           "    ret i64 %1\n"
           "b4:\n"
           "    jump b2\n"
           "b5:\n"
           "    %4 = sgt i8 %1, 8\n"
           "    branch %4, b6, b7\n"
           "b6:\n"
           "    jump b3\n"
           "b7:\n"
           "    jump b2\n"
           "}\n");

    /* A condition branches after each operand of && and ||. */
    lowers("fn pick(a: bool, b: bool, c: int) -> int {\n"
           "    if a && (b || c > 0) {\n"
           "        return 1;\n"
           "    } else if !a {\n"
           "        return 2;\n"
           "    }\n"
           "    return 3;\n"
           "}\n",
           "fn main.pick(%0: i8 zeroext, %1: i8 zeroext, %2: i64) -> i64 {\n"
           "b0:\n"
           "    branch %0, b3, b2\n"
           "b1:\n"
           "    ret i64 1\n"
           "b2:\n"
           "    branch %0, b6, b5\n"
           "b3:\n"
           "    branch %1, b1, b4\n"
           "b4:\n"
           "    %3 = sgt i8 %2, 0\n"
           "    branch %3, b1, b2\n"
           "b5:\n"
           "    ret i64 2\n"
           "b6:\n"
           "    ret i64 3\n"
           "}\n");

    /* An address-taken parameter is copied into a slot on entry. */
    lowers("const BIAS: i8 = -128;\n"
           "fn bump(x: i8) -> i16 {\n"
           "    let p = &x;\n"
           "    *p -= BIAS;\n"
           "    return x as i16 + (x as u8 as i16);\n"
           "}\n",
           "fn main.bump(%0: i8 signext) -> i16 {\n"
           "b0:\n"
           "    %1 = slot i8\n"
           "    store i8 %0, %1\n"
           "    %2 = copy ptr %1\n"
           "    %3 = load i8 %2\n"
           "    %4 = sub i8 %3, -128\n"
           "    store i8 %4, %2\n"
           "    %5 = load i8 %1\n"
           "    %6 = sext i16 %5\n"
           "    %7 = load i8 %1\n"
           "    %8 = zext i16 %7\n"
           "    %9 = add i16 %6, %8\n"
           "    ret i16 %9\n"
           "}\n");

    lowers("fn conv(a: u16, f: f64) -> f32 {\n"
           "    return (a as f64 * f) as f32 + (f as i32) as f32;\n"
           "}\n",
           "fn main.conv(%0: i16 zeroext, %1: f64) -> f32 {\n"
           "b0:\n"
           "    %2 = uitof f64 %0\n"
           "    %3 = fmul f64 %2, %1\n"
           "    %4 = ftrunc f32 %3\n"
           "    %5 = ftosi i32 %1\n"
           "    %6 = sitof f32 %5\n"
           "    %7 = fadd f32 %4, %6\n"
           "    ret f32 %7\n"
           "}\n");

    /* A string literal is a global with a NUL after its bytes, and a str
       is the address of the bytes and their length. Literals with equal
       bytes share one global. */
    lowers("fn f() -> int {\n"
           "    let s = \"hi\";\n"
           "    let b = b\"h\\0\";\n"
           "    let t = \"hi\";\n"
           "    return s.len + b.len + t.len;\n"
           "}\n",
           "type str = struct { ptr: ptr, len: i64 }\n"
           "type []byte = struct { ptr: ptr, len: i64 }\n"
           "global main.0 size 3 align 1 bytes 68 69 00\n"
           "global main.1 size 3 align 1 bytes 68 00 00\n"
           "fn main.f() -> i64 {\n"
           "b0:\n"
           "    %0 = slot str\n"
           "    %1 = slot []byte\n"
           "    %2 = slot str\n"
           "    %3 = addr @main.0\n"
           "    store ptr %3, %0\n"
           "    %4 = ptradd %0, offset_of str.len\n"
           "    store i64 2, %4\n"
           "    %5 = addr @main.1\n"
           "    store ptr %5, %1\n"
           "    %6 = ptradd %1, offset_of []byte.len\n"
           "    store i64 2, %6\n"
           "    %7 = addr @main.0\n"
           "    store ptr %7, %2\n"
           "    %8 = ptradd %2, offset_of str.len\n"
           "    store i64 2, %8\n"
           "    %9 = ptradd %0, offset_of str.len\n"
           "    %10 = load i64 %9\n"
           "    %11 = ptradd %1, offset_of []byte.len\n"
           "    %12 = load i64 %11\n"
           "    %13 = add i64 %10, %12\n"
           "    %14 = ptradd %2, offset_of str.len\n"
           "    %15 = load i64 %14\n"
           "    %16 = add i64 %13, %15\n"
           "    ret i64 %16\n"
           "}\n");

    /* An element of a str or a slice lies after the loaded pointer. A
       slice of a str starts at ptr + lo and holds hi - lo bytes. */
    lowers("fn g(s: str, a: []i32, i: int) -> i32 {\n"
           "    let t = s[1..3];\n"
           "    return a[i] + s[i] as i32 + t.len as i32;\n"
           "}\n",
           "type str = struct { ptr: ptr, len: i64 }\n"
           "type []i32 = struct { ptr: ptr, len: i64 }\n"
           "type []byte = struct { ptr: ptr, len: i64 }\n"
           "fn main.g(%0: agg str, %1: agg []i32, %2: i64) -> i32 {\n"
           "b0:\n"
           "    %3 = slot []byte\n"
           "    %4 = load ptr %0\n"
           "    %5 = mul i64 1, size_of i8\n"
           "    %6 = ptradd %4, %5\n"
           "    store ptr %6, %3\n"
           "    %7 = sub i64 3, 1\n"
           "    %8 = ptradd %3, offset_of []byte.len\n"
           "    store i64 %7, %8\n"
           "    %9 = load ptr %1\n"
           "    %10 = mul i64 %2, size_of i32\n"
           "    %11 = ptradd %9, %10\n"
           "    %12 = load i32 %11\n"
           "    %13 = load ptr %0\n"
           "    %14 = mul i64 %2, size_of i8\n"
           "    %15 = ptradd %13, %14\n"
           "    %16 = load i8 %15\n"
           "    %17 = zext i32 %16\n"
           "    %18 = add i32 %12, %17\n"
           "    %19 = ptradd %3, offset_of []byte.len\n"
           "    %20 = load i64 %19\n"
           "    %21 = trunc i32 %20\n"
           "    %22 = add i32 %18, %21\n"
           "    ret i32 %22\n"
           "}\n");

    /* A slice literal stores its fields, and a slice of an array points
       into the array. */
    lowers("fn h(p: *u8, n: int) -> *u8 {\n"
           "    let arr = [5, 6, 7];\n"
           "    let s = []byte { ptr: p, len: n };\n"
           "    let whole = arr[0..3];\n"
           "    return s.ptr;\n"
           "}\n",
           "type [3]int = array 3 of i64\n"
           "type []byte = struct { ptr: ptr, len: i64 }\n"
           "type []int = struct { ptr: ptr, len: i64 }\n"
           "fn main.h(%0: ptr, %1: i64) -> ptr {\n"
           "b0:\n"
           "    %2 = slot [3]int\n"
           "    %3 = slot []byte\n"
           "    %4 = slot []int\n"
           "    store i64 5, %2\n"
           "    %5 = mul i64 1, size_of i64\n"
           "    %6 = ptradd %2, %5\n"
           "    store i64 6, %6\n"
           "    %7 = mul i64 2, size_of i64\n"
           "    %8 = ptradd %2, %7\n"
           "    store i64 7, %8\n"
           "    store ptr %0, %3\n"
           "    %9 = ptradd %3, offset_of []byte.len\n"
           "    store i64 %1, %9\n"
           "    %10 = mul i64 0, size_of i64\n"
           "    %11 = ptradd %2, %10\n"
           "    store ptr %11, %4\n"
           "    %12 = sub i64 3, 0\n"
           "    %13 = ptradd %4, offset_of []int.len\n"
           "    store i64 %12, %13\n"
           "    %14 = load ptr %3\n"
           "    ret ptr %14\n"
           "}\n");

    /* A str constant is data of its own that points at the literal. */
    lowers("const GREETING: str = \"hey\";\n"
           "fn k() -> int {\n"
           "    let g = GREETING;\n"
           "    return g.len;\n"
           "}\n",
           "type str = struct { ptr: ptr, len: i64 }\n"
           "global main.0 size 4 align 1 bytes 68 65 79 00\n"
           "global main.1 str { @main.0, i64 3 }\n"
           "fn main.k() -> i64 {\n"
           "b0:\n"
           "    %0 = slot str\n"
           "    %1 = addr @main.1\n"
           "    memcopy %0, %1, str\n"
           "    %2 = ptradd %0, offset_of str.len\n"
           "    %3 = load i64 %2\n"
           "    ret i64 %3\n"
           "}\n");
    /* An aggregate parameter is a pointer to the value, and a field is a
       ptradd of its offset. */
    lowers("struct P { x: int, y: i32 }\n"
           "fn t(p: P) -> int {\n"
           "    return p.x + p.y as int;\n"
           "}\n"
           "fn u(p: *P) {\n"
           "    p.y = 3;\n"
           "}\n",
           "type main.P = struct { x: i64, y: i32 }\n"
           "fn main.t(%0: agg main.P) -> i64 {\n"
           "b0:\n"
           "    %1 = load i64 %0\n"
           "    %2 = ptradd %0, offset_of main.P.y\n"
           "    %3 = load i32 %2\n"
           "    %4 = sext i64 %3\n"
           "    %5 = add i64 %1, %4\n"
           "    ret i64 %5\n"
           "}\n"
           "fn main.u(%0: ptr) {\n"
           "b0:\n"
           "    %1 = ptradd %0, offset_of main.P.y\n"
           "    store i32 3, %1\n"
           "    ret\n"
           "}\n");

    /* An aggregate local lives in a slot. A literal fills the slot, a copy
       is a memcopy, and a function returns the address of its value. */
    lowers("struct V { x: f64, y: f64 }\n"
           "fn make(a: f64) -> V {\n"
           "    let v = V { x: a, y: 2.0 };\n"
           "    let w = v;\n"
           "    w.y = v.x;\n"
           "    return w;\n"
           "}\n",
           "type main.V = struct { x: f64, y: f64 }\n"
           "fn main.make(%0: f64) -> agg main.V {\n"
           "b0:\n"
           "    %1 = slot main.V\n"
           "    %2 = slot main.V\n"
           "    store f64 %0, %1\n"
           "    %3 = ptradd %1, offset_of main.V.y\n"
           "    store f64 2, %3\n"
           "    memcopy %2, %1, main.V\n"
           "    %4 = ptradd %2, offset_of main.V.y\n"
           "    %5 = load f64 %1\n"
           "    store f64 %5, %4\n"
           "    ret ptr %2\n"
           "}\n");

    /* Array elements lie at multiples of the element size. */
    lowers("fn sum3(a: [3]i32) -> i32 {\n"
           "    let b = [a[2], a[1], 7];\n"
           "    return b[0] + b[2];\n"
           "}\n",
           "type [3]i32 = array 3 of i32\n"
           "fn main.sum3(%0: agg [3]i32) -> i32 {\n"
           "b0:\n"
           "    %1 = slot [3]i32\n"
           "    %2 = mul i64 2, size_of i32\n"
           "    %3 = ptradd %0, %2\n"
           "    %4 = load i32 %3\n"
           "    store i32 %4, %1\n"
           "    %5 = mul i64 1, size_of i32\n"
           "    %6 = ptradd %1, %5\n"
           "    %7 = mul i64 1, size_of i32\n"
           "    %8 = ptradd %0, %7\n"
           "    %9 = load i32 %8\n"
           "    store i32 %9, %6\n"
           "    %10 = mul i64 2, size_of i32\n"
           "    %11 = ptradd %1, %10\n"
           "    store i32 7, %11\n"
           "    %12 = mul i64 0, size_of i32\n"
           "    %13 = ptradd %1, %12\n"
           "    %14 = load i32 %13\n"
           "    %15 = mul i64 2, size_of i32\n"
           "    %16 = ptradd %1, %15\n"
           "    %17 = load i32 %16\n"
           "    %18 = add i32 %14, %17\n"
           "    ret i32 %18\n"
           "}\n");

    /* The length of an array is its element count. The base is still
       evaluated, for the call in make().len. */
    lowers("extern fn make() -> [2]f64;\n"
           "fn count(a: [5]int) -> int {\n"
           "    return a.len + make().len;\n"
           "}\n",
           "type [2]float = array 2 of f64\n"
           "type [5]int = array 5 of i64\n"
           "extern fn make() -> agg [2]float\n"
           "fn main.count(%0: agg [5]int) -> i64 {\n"
           "b0:\n"
           "    %1 = call agg @make()\n"
           "    %2 = add i64 5, 2\n"
           "    ret i64 %2\n"
           "}\n");

    /* DESIGN: an aggregate constant is read-only data of the module. The
       IR carries it as a typed tree, because the bytes of a struct depend
       on the target that lays it out. A use is the address of that data,
       so a constant costs no code and every use reads the same bytes. */
    lowers("struct Gap { a: u8, b: i32 }\n"
           "struct Pair { one: Gap, two: Gap }\n"
           "const GAP: Gap = Gap { a: 1, b: 2 };\n"
           "const PAIR: Pair = Pair { one: GAP, two: Gap { a: 3, b: 4 } };\n"
           "fn f(out: *Pair) {\n"
           "    *out = PAIR;\n"
           "}\n",
           "type main.Gap = struct { a: i8, b: i32 }\n"
           "type main.Pair = struct { one: main.Gap, two: main.Gap }\n"
           "global main.0 main.Pair { main.Gap { i8 1, i32 2 }, "
           "main.Gap { i8 3, i32 4 } }\n"
           "fn main.f(%0: ptr) {\n"
           "b0:\n"
           "    %1 = addr @main.0\n"
           "    memcopy %0, %1, main.Pair\n"
           "    ret\n"
           "}\n");

    /* A constant is read-only data and a literal argument gets a slot of
       its own. A call with an aggregate result gives the address of the
       result. */
    lowers("struct V { x: f64, y: f64 }\n"
           "const ONE: V = V { x: 1.0, y: 1.0 };\n"
           "extern fn add(a: V, b: V) -> V;\n"
           "fn apply() -> f64 {\n"
           "    let v = add(ONE, V { x: 3.0, y: 4.0 });\n"
           "    return v.y;\n"
           "}\n",
           "type main.V = struct { x: f64, y: f64 }\n"
           "extern fn add(agg main.V, agg main.V) -> agg main.V\n"
           "global main.0 main.V { f64 1, f64 1 }\n"
           "fn main.apply() -> f64 {\n"
           "b0:\n"
           "    %0 = slot main.V\n"
           "    %2 = slot main.V\n"
           "    %1 = addr @main.0\n"
           "    store f64 3, %2\n"
           "    %3 = ptradd %2, offset_of main.V.y\n"
           "    store f64 4, %3\n"
           "    %4 = call agg @add(%1, %2)\n"
           "    memcopy %0, %4, main.V\n"
           "    %5 = ptradd %0, offset_of main.V.y\n"
           "    %6 = load f64 %5\n"
           "    ret f64 %6\n"
           "}\n");

    /* A function used as a value is its address. A call through a
       function pointer names a signature, a declared function of the
       module that no code defines. */
    lowers("fn twice(x: int) -> int {\n"
           "    return x * 2;\n"
           "}\n"
           "fn apply(f: fn(int) -> int, x: int) -> int {\n"
           "    return f(x);\n"
           "}\n"
           "fn main() -> int {\n"
           "    let g = twice;\n"
           "    return apply(g, 3) + g(1);\n"
           "}\n",
           "extern fn main.fn.0(i64) -> i64\n"
           "fn main.twice(%0: i64) -> i64 {\n"
           "b0:\n"
           "    %1 = mul i64 %0, 2\n"
           "    ret i64 %1\n"
           "}\n"
           "fn main.apply(%0: ptr, %1: i64) -> i64 {\n"
           "b0:\n"
           "    %2 = call i64 %0 via @main.fn.0(%1)\n"
           "    ret i64 %2\n"
           "}\n"
           "fn main.main() -> i64 {\n"
           "b0:\n"
           "    %0 = addr @main.twice\n"
           "    %1 = copy ptr %0\n"
           "    %2 = call i64 @main.apply(%1, 3)\n"
           "    %3 = call i64 %1 via @main.fn.0(1)\n"
           "    %4 = add i64 %2, %3\n"
           "    ret i64 %4\n"
           "}\n");

    /* A field of function pointer type is called through its value. Equal
       function types share a signature, and a narrow parameter keeps its
       extension. */
    lowers("extern fn abs(x: i32) -> i32;\n"
           "struct Ops { unary: fn(i32) -> i32, narrow: fn(i8) -> i8 }\n"
           "fn call(o: *Ops, x: i32) -> i32 {\n"
           "    let same: fn(i32) -> i32 = abs;\n"
           "    return o.unary(x) + same(x) + o.narrow(1) as i32;\n"
           "}\n",
           "type main.Ops = struct { unary: ptr, narrow: ptr }\n"
           "extern fn abs(i32) -> i32\n"
           "extern fn main.fn.0(i32) -> i32\n"
           "extern fn main.fn.1(i8 signext) -> i8\n"
           "fn main.call(%0: ptr, %1: i32) -> i32 {\n"
           "b0:\n"
           "    %2 = addr @abs\n"
           "    %3 = copy ptr %2\n"
           "    %4 = load ptr %0\n"
           "    %5 = call i32 %4 via @main.fn.0(%1)\n"
           "    %6 = call i32 %3 via @main.fn.0(%1)\n"
           "    %7 = add i32 %5, %6\n"
           "    %8 = ptradd %0, offset_of main.Ops.narrow\n"
           "    %9 = load ptr %8\n"
           "    %10 = call i8 %9 via @main.fn.1(1)\n"
           "    %11 = sext i32 %10\n"
           "    %12 = add i32 %7, %11\n"
           "    ret i32 %12\n"
           "}\n");
}
