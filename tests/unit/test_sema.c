#include "check.h"
#include "arena.h"
#include "ast.h"
#include "diagnostic.h"
#include "lexer.h"
#include "parser.h"
#include "sema.h"
#include "types.h"

struct checked {
    struct arena arena;
    struct diagnostics diags;
    struct token_list tokens;
    struct module *module;
    struct types types;
    bool ok;
};

static void run(struct checked *c, const char *source)
{
    memset(c, 0, sizeof *c);
    types_init(&c->types, &c->arena);
    c->ok = lex(source, strlen(source), &c->arena, &c->diags, &c->tokens) &&
            parse(source, &c->tokens, &c->arena, &c->diags, &c->module);
    if (!c->ok) {
        fprintf(stderr, "syntax error in test source: %s\n",
                c->diags.items[0].message);
        check_failures++;
        return;
    }
    c->ok = sema_check(c->module, "main", NULL, 0, &c->types, &c->arena,
                       &c->diags);
}

static void release(struct checked *c)
{
    token_list_free(&c->tokens);
    diagnostics_free(&c->diags);
    arena_free(&c->arena);
}

static void accepts(const char *source)
{
    struct checked c;
    size_t i;

    run(&c, source);
    if (!c.ok) {
        check_failures++;
        fprintf(stderr, "rejected:\n%s\n", source);
        for (i = 0; i < c.diags.count; i++) {
            fprintf(stderr, "  %d:%d: %s\n", c.diags.items[i].line,
                    c.diags.items[i].column, c.diags.items[i].message);
        }
    }
    release(&c);
}

static void rejects(const char *source, int line, int column,
                    const char *message)
{
    struct checked c;

    run(&c, source);
    if (c.ok || c.diags.count == 0) {
        check_failures++;
        fprintf(stderr, "accepted, expected %d:%d: %s\n%s\n", line, column,
                message, source);
    } else if (c.diags.items[0].line != line ||
               c.diags.items[0].column != column ||
               strcmp(c.diags.items[0].message, message) != 0) {
        check_failures++;
        fprintf(stderr, "expected %d:%d: %s\ngot      %d:%d: %s\n%s\n", line,
                column, message, c.diags.items[0].line,
                c.diags.items[0].column, c.diags.items[0].message, source);
    }
    release(&c);
}

static void typed(const char *source, const char *expected)
{
    struct checked c;
    struct text out = {0};

    run(&c, source);
    CHECK(c.ok);
    if (c.module != NULL) {
        ast_dump_typed(&out, c.module);
        CHECK_STR(text_cstr(&out), expected);
    }
    text_free(&out);
    release(&c);
}

/* Check source and expect success without any message, then run the doc
   warnings and expect one warning. */
static void warns(const char *source, int line, int column,
                  const char *message)
{
    struct checked c;

    run(&c, source);
    CHECK(c.ok);
    CHECK(c.diags.count == 0);
    sema_doc_warnings(c.module, &c.diags);
    CHECK(c.diags.count == 1);
    if (c.diags.count == 1) {
        CHECK(c.diags.items[0].warning);
        CHECK(c.diags.items[0].line == line &&
              c.diags.items[0].column == column);
        CHECK_STR(c.diags.items[0].message, message);
    }
    release(&c);
}

void test_sema(void)
{
    /* A developer note on a public item needs a user comment beside it. */
    warns("//# Invariant.\npub fn f() {}\n"
          "//# Private.\nfn g() {}\n"
          "/// Doc.\n//# Noted.\npub fn h() {}\n",
          2, 8, "the pub item `f` has a `//#` note and no `///` comment");
    typed("fn scale(x: int) -> int {\n"
          "    let k = 2 + 4;\n"
          "    return x * k;\n"
          "}\n",
          "function scale             fn(int) -> int\n"
          "  param x                  int\n"
          "    type int\n"
          "  result\n"
          "    type int\n"
          "  block\n"
          "    let_stmt k             int\n"
          "      additive +           int\n"
          "        int_lit 2          int\n"
          "        int_lit 4          int\n"
          "    return_stmt\n"
          "      multiplicative *     int\n"
          "        ident x            int\n"
          "        ident k            int\n");

    /* Literal types from context. */
    typed("fn f(a: u32) -> u32 { let x: i8 = -128; return a + 1; }",
          "function f                 fn(u32) -> u32\n"
          "  param a                  u32\n"
          "    type u32\n"
          "  result\n"
          "    type u32\n"
          "  block\n"
          "    let_stmt x             i8\n"
          "      type i8\n"
          "      unary -              i8\n"
          "        int_lit 128        i8\n"
          "    return_stmt\n"
          "      additive +           u32\n"
          "        ident a            u32\n"
          "        int_lit 1          u32\n");
    /* uint and the C types with a fixed size are other spellings of the
       sized types. */
    typed("extern fn f(a: c_int, b: c_uchar, c: c_size_t, d: c_double) -> uint;",
          "extern_fn f                fn(i32, byte, u64, float) -> u64\n"
          "  param a\n"
          "    type c_int\n"
          "  param b\n"
          "    type c_uchar\n"
          "  param c\n"
          "    type c_size_t\n"
          "  param d\n"
          "    type c_double\n"
          "  result\n"
          "    type uint\n");
    accepts("fn f(a: c_char, b: c_short, c: c_ushort, d: c_uint) {\n"
            "    let x: i8 = a; let y: i16 = b; let z: u16 = c;\n"
            "    let w: u32 = d; let v: c_float = 1.5; let u: f32 = v;\n"
            "    let s: i64 = 1 as c_longlong; let t: u64 = 1 as c_ulonglong;\n"
            "}");
    /* A length computed from size_of stays symbolic until the back end
       folds it. Equal lengths make one array type. */
    accepts("struct H { tag: u8, n: i32 }\n"
            "const N: int = size_of(H) * 2;\n"
            "struct B { bytes: [N]byte, more: [size_of(H) * 2]byte }\n"
            "fn f(b: *B) -> int {\n"
            "    let copy: [N]byte = b.more;\n"
            "    let fill = [0; N - 1];\n"
            "    return copy.len + fill.len;\n"
            "}\n");
    rejects("struct H { n: i32 }\nfn f(a: [size_of(H)]byte) {}", 2, 10,
            "a length computed from `size_of` is allowed only in a struct "
            "field or a local variable");
    rejects("struct H { n: i32 }\nconst Z: [size_of(H)]byte = [0; 4];", 2, 11,
            "a length computed from `size_of` is allowed only in a struct "
            "field or a local variable");
    rejects("struct H { n: i32 }\nconst F: f64 = size_of(H) as f64;", 2, 16,
            "a value computed from `size_of` converts only to an integer type "
            "in a constant expression");
    rejects("struct H { n: i32 }\nconst D: int = size_of(H) / 0;", 2, 16,
            "`size_of(H) / 0` divides by zero");
    rejects("struct H { n: i32 }\nconst A: [2]int = [1, 2];\n"
            "const X: int = A[size_of(H) - 4];", 3, 18,
            "an index computed from `size_of` is not a constant expression");
    rejects("struct S { a: [size_of(S)]byte }", 1, 8,
            "struct `S` contains itself");
    /* An export fn takes and returns only types with a C representation,
       and an exported struct holds only such fields. */
    accepts("export struct Vec2 { x: c_int, y: c_int, tags: [4]u8 }\n"
            "export union Num { i: i64, d: f64 }\n"
            "export const LIMIT: int = 10;\n"
            "export const NAME: str = \"geo\";\n"
            "export fn dot(a: Vec2, b: *Vec2, n: *Num, cb: fn(i32) -> bool,\n"
            "              p: *byte, f: float, u: uint, q: **Vec2) -> c_int {\n"
            "    return a.x;\n"
            "}\n");
    rejects("export fn f(s: str) {}", 1, 13,
            "the parameter `s` of export fn `f` has type `str`, which C cannot "
            "represent");
    rejects("export fn f(c: char) {}", 1, 13,
            "the parameter `c` of export fn `f` has type `char`, which C cannot "
            "represent");
    rejects("export fn f(a: [4]int) {}", 1, 13,
            "the parameter `a` of export fn `f` has type `[4]int`, which C "
            "cannot represent");
    rejects("export fn f(p: *[]int) {}", 1, 13,
            "the parameter `p` of export fn `f` has type `*[]int`, which C "
            "cannot represent");
    rejects("export fn f(cb: fn(str)) {}", 1, 13,
            "the parameter `cb` of export fn `f` has type `fn(str)`, which C "
            "cannot represent");
    rejects("struct V { x: int }\nexport fn f(v: *V) {}", 2, 13,
            "the parameter `v` of export fn `f` has type `*V`, and `V` is not "
            "exported");
    rejects("export fn f() -> str { return \"x\"; }", 1, 18,
            "the result of export fn `f` has type `str`, which C cannot "
            "represent");
    rejects("export struct S { s: str }", 1, 19,
            "the field `s` of export struct `S` has type `str`, which C "
            "cannot represent");
    rejects("struct B { a: i32 }\nexport struct S { d: [size_of(B)]u8 }", 2, 19,
            "the field `d` of export struct `S` has type `[size_of(B)]byte`, "
            "which C cannot represent");
    rejects("export const P: *byte = null;", 1, 17,
            "export const `P` has type `*byte`, and an export const is a "
            "number, a bool or a str");
    rejects("export fn main() -> int { return 0; }", 1, 11,
            "`main` is the entry of the program and cannot be exported");
    accepts("export struct S align(8) { a: u32, b: u32 : 3 }");
    rejects("export struct S align(8) { a: u32 : 3, b: u32 }", 1, 28,
            "the first field `a` of export struct `S` is a bitfield, and the "
            "C header aligns the struct on its first field");
    /* A bitfield has a sized integer type and 1 to that many bits. It has
       no address. */
    accepts("struct Flags { visible: u32 : 1, layer: u32 : 4, level: i8 : 3 }\n"
            "fn f(p: *Flags) -> int {\n"
            "    p.layer += 1;\n"
            "    let f = Flags { visible: 1, layer: 2, level: -1 };\n"
            "    f.level = p.level;\n"
            "    return f.visible as int + f.layer as int;\n"
            "}\n");
    rejects("struct S { a: f32 : 3 }", 1, 15,
            "a bitfield has a sized integer type, found `f32`");
    rejects("struct S { a: c_long : 3 }", 1, 15,
            "a bitfield has a sized integer type, found `c_long`");
    rejects("struct S { a: u8 : 9 }", 1, 20,
            "a bitfield of `byte` has 1 to 8 bits");
    rejects("struct S { a: u8 : 0 }", 1, 20,
            "a bitfield of `byte` has 1 to 8 bits");
    rejects("struct S { a: u8 : 3 }\nfn f(s: S) { let p = &s.a; }", 2, 23,
            "a bitfield has no address");
    /* A zero-width bitfield `_: T : 0` breaks the unit. It holds no value,
       so no literal, access or constant names it. */
    accepts("struct S { a: u8, _: u32 : 0, b: u8 : 3, _: u16 : 0, c: u8 }\n"
            "const K: S = S { a: 1, b: 2, c: 3 };\n"
            "fn f(s: S) -> int {\n"
            "    let t = S { a: 4, b: 5, c: 6 };\n"
            "    return s.a as int + t.b as int + K.c as int;\n"
            "}\n");
    rejects("struct S { _: u32, a: u8 }", 1, 12,
            "the field `_` is a zero-width bitfield, written `_: T : 0`");
    rejects("struct S { _: u32 : 2, a: u8 }", 1, 12,
            "the field `_` is a zero-width bitfield, written `_: T : 0`");
    rejects("union U { a: u8, _: u32 : 0 }", 1, 18,
            "a union holds no zero-width bitfield");
    rejects("struct S { a: u8, _: u32 : 0 }\nfn f(s: S) -> u32 { return s._; }",
            2, 28, "`S` has no field `_`");
    rejects("struct S { a: u8, _: u32 : 0 }\n"
            "fn f() -> S { return S { a: 1, _: 0 }; }", 2, 32,
            "`S` has no field `_`");
    rejects("export struct S align(8) { _: u32 : 0, b: u32 }", 1, 28,
            "the first field `_` of export struct `S` is a bitfield, and the "
            "C header aligns the struct on its first field");
    /* An alignment is a constant power of two. */
    accepts("const N: int = 8;\npacked struct P { a: u8, b: i32 }\n"
            "struct A align(N * 2) { a: u8, p: P }\n"
            "fn f(a: A) -> i32 { return a.p.b; }\n");
    rejects("struct A align(3) { a: u8 }", 1, 16,
            "an alignment is a power of two");
    rejects("struct B { a: i32 }\nstruct A align(size_of(B)) { a: u8 }", 2, 16,
            "an alignment is a constant, not a value computed from `size_of`");
    /* A union literal names one field, and a union has no methods. */
    accepts("union Value { i: int, f: f64 }\n"
            "struct Tagged { tag: u8, v: Value }\n"
            "fn f(x: f64) -> int {\n"
            "    let t = Tagged { tag: 1, v: Value { f: x } };\n"
            "    t.v.i = t.v.i + 1;\n"
            "    return t.v.i;\n"
            "}\n");
    rejects("union Value { i: int, f: f64 }\n"
            "fn f() { let v = Value { i: 1, f: 2.0 }; }", 2, 18,
            "a literal of union `Value` names exactly one field");
    rejects("union Value { i: int, f: f64 }\n"
            "fn get(v: Value) -> int { return v.i; }\n"
            "fn f(v: Value) -> int { return v.get(); }", 3, 32,
            "`Value` has no field `get`");
    rejects("union Value { i: int, f: f64 }\n"
            "const V: Value = Value { i: 1 };", 2, 18,
            "a union is not a constant expression");
    /* c_long, c_ulong and c_wchar are types of their own whose width the
       target decides. A literal fits the narrower width. */
    typed("extern fn labs(n: c_long) -> c_long;\n"
          "fn f(a: i32, w: c_wchar) -> c_ulong {\n"
          "    return (labs(a as c_long) + 1) as c_ulong + w as c_ulong;\n"
          "}\n",
          "extern_fn labs             fn(c_long) -> c_long\n"
          "  param n\n"
          "    type c_long\n"
          "  result\n"
          "    type c_long\n"
          "function f                 fn(i32, c_wchar) -> c_ulong\n"
          "  param a                  i32\n"
          "    type i32\n"
          "  param w                  c_wchar\n"
          "    type c_wchar\n"
          "  result\n"
          "    type c_ulong\n"
          "  block\n"
          "    return_stmt\n"
          "      additive +           c_ulong\n"
          "        cast               c_ulong\n"
          "          additive +       c_long\n"
          "            call           c_long\n"
          "              ident labs   fn(c_long) -> c_long\n"
          "              cast         c_long\n"
          "                ident a    i32\n"
          "                type c_long\n"
          "            int_lit 1      c_long\n"
          "          type c_ulong\n"
          "        cast               c_ulong\n"
          "          ident w          c_wchar\n"
          "          type c_ulong\n");
    accepts("fn f() { let a: c_long = -2147483648; let b: c_ulong = 4294967295;"
            " let c: c_wchar = 65535; }");
    rejects("fn f() { let a: c_long = 2147483648; }", 1, 26,
            "`2147483648` does not fit `c_long` on every target");
    rejects("fn f() { let c: c_wchar = 65536; }", 1, 27,
            "`65536` does not fit `c_wchar` on every target");
    rejects("fn f(a: c_long) -> i64 { return a; }", 1, 33,
            "expected `int`, found `c_long`");
    rejects("const BIG: c_ulong = 65536 as c_ulong * 65536 as c_ulong;", 1, 22,
            "the value does not fit `c_ulong` on every target");
    accepts("fn f() { let big = 5_000_000_000; let half: f32 = 0.5; }");
    accepts("fn f() { let x: u8 = 250 + 10; let p: *int = null; }");
    accepts("fn f(p: *int) -> bool { return p == null; }");
    rejects("fn f() { let n: u8 = 300; }", 1, 22, "`300` does not fit `byte`");
    rejects("fn f() { let m: u8 = -1; }", 1, 22, "`-1` does not fit `byte`");
    rejects("fn f() { let g: f32 = 1; }", 1, 23,
            "expected `f32`, found an integer literal");
    rejects("fn f() { let x: int = 1.5; }", 1, 23,
            "expected `int`, found a float literal");
    rejects("fn f() { let p = null; }", 1, 18,
            "`null` needs a pointer type from its context");
    rejects("fn f(a: u8) { let b = -a; }", 1, 23,
            "unary `-` needs a signed integer or a float, found `byte`");

    /* Operators and conversions. */
    accepts("fn f(x: int) -> bool { return (x & 1) == 0 && x < 10 || !true; }");
    accepts("fn f(x: int) -> u8 { return (x as u8) << 3; }");
    accepts("fn f() -> u32 { return 'A' as u32 + 1; }");
    accepts("fn f(p: *int) -> *byte { return p as *byte; }");
    accepts("fn f(b: bool, c: u32) -> char { let n = b as int; return c as char; }");
    rejects("fn f(a: int, b: u8) -> int { return a + b; }", 1, 37,
            "the operands of `+` have the types `int` and `byte`");
    rejects("fn f(x: int) -> bool { return x & 1 == 0; }", 1, 31,
            "the operands of `&` have the types `int` and `bool`");
    rejects("fn f(a: float) -> float { return a % 2.0; }", 1, 34,
            "`%` needs integer operands, found `float`");
    rejects("fn f(x: int) -> bool { return x as bool; }", 1, 31,
            "cannot convert `int` to `bool`");
    rejects("fn f(a: bool, b: bool) -> bool { return a < b; }", 1, 41,
            "`<` needs numeric or `char` operands, found `bool`");

    /* Names, scopes and shadowing. */
    accepts("fn f(x: int) -> int { let x = x + 1; return x; }");
    accepts("fn f(x: int) -> int { let t = 0; { let x = x + 1; t += x; } "
            "return t + x; }");
    accepts("fn f() -> int { return g(); }\nfn g() -> int { return 1; }");
    rejects("fn f() -> int { return y; }", 1, 24, "unknown name `y`");
    rejects("fn f() { let a = 1; let a = 2; }", 1, 25,
            "`a` is already declared in this block");
    rejects("fn f() {}\nfn f() {}", 2, 4, "`f` is already declared");
    rejects("fn f(a: int, a: int) {}", 1, 14,
            "`a` is already declared in this block");

    /* Statements. */
    accepts("fn f(n: int) -> int { let i = 0; while i < n do { i += 1; "
            "if i == 3 { continue; } if i > 5 { break; } } do { i -= 1; } "
            "while i > 0 return i; }");
    rejects("fn f(n: int) { if n { } }", 1, 19,
            "a condition has type `bool`, found `int`");
    rejects("fn f() { break; }", 1, 10, "`break` outside a loop");
    rejects("fn f() -> int { return; }", 1, 17,
            "`return` needs a value of type `int`");
    rejects("fn f() { return 1; }", 1, 17, "`f` returns no value");
    rejects("fn f() -> int { return true; }", 1, 24,
            "expected `int`, found `bool`");
    rejects("fn f(n: int) -> int { if n > 0 { return 1; } }", 1, 4,
            "`f` can reach its end without `return`");
    accepts("fn f(n: int) -> int { if n > 0 { return 1; } else if n < 0 "
            "{ return -1; } else { return 0; } }");
    rejects("fn f() -> int { while true do { return 1; } }", 1, 4,
            "`f` can reach its end without `return`");
    rejects("fn f() { 1 = 2; }", 1, 10, "cannot assign to this expression");
    rejects("fn f(s: str) { s[0] = 65; }", 1, 16,
            "cannot assign to this expression");
    rejects("fn f(s: []int) { s.len = 3; }", 1, 18,
            "cannot assign to this expression");

    /* Structs, fields, methods and literals. */
    accepts("struct Vec2 { x: f32, y: f32 }\n"
            "fn length(v: *Vec2) -> f32 { return v.x * v.x + v.y * v.y; }\n"
            "fn scaled(v: Vec2, k: f32) -> Vec2 { return Vec2 { y: v.y * k, x: v.x * k }; }\n"
            "fn f() -> f32 { let v = Vec2 { x: 3.0, y: 4.0 }; let p = &v; "
            "let w = v.scaled(2.0); return v.length() + p.length() + p.scaled(1.0).x; }");
    accepts("struct Shape { area: fn(*Shape) -> f64, size: f64 }\n"
            "fn square_area(s: *Shape) -> f64 { return s.size * s.size; }\n"
            "fn scale(s: *Shape, k: f64) { s.size *= k; }\n"
            "fn demo() -> f64 { let sq = Shape { area: square_area, size: 2.0 }; "
            "sq.scale(1.5); return sq.area(&sq); }");
    rejects("struct V { x: int }\nfn f() { let v = V { x: 1, y: 2 }; }", 2, 28,
            "`V` has no field `y`");
    rejects("struct V { x: int, y: int }\nfn f() { let v = V { x: 1 }; }", 2,
            18, "the literal of `V` misses the field `y`");
    rejects("struct V { x: int }\nfn f(v: V) -> int { return v.z; }", 2, 28,
            "`V` has no field `z`");
    rejects("struct V { x: int }\nfn f(v: V) { v.go(); }", 2, 14,
            "`V` has no field or method `go`");
    rejects("struct Node { next: *Node, value: Node }", 1, 8,
            "struct `Node` contains itself");
    rejects("fn f(v: Vec2) {}", 1, 9, "unknown type `Vec2`");

    /* Pointers, arrays, slices and strings. */
    accepts("fn f(a: [4]int, s: []int, p: *int, t: str) -> int {\n"
            "    let x = a[1] + s[2] + p[3] + *p + a.len + s.len;\n"
            "    let b: byte = t[0];\n"
            "    let part: []byte = t[1..3];\n"
            "    let whole: []int = a[0..4];\n"
            "    let raw: []int = []int { ptr: p, len: 4 };\n"
            "    let q: *byte = t.ptr;\n"
            "    return x;\n"
            "}");
    accepts("fn f() -> int { let a = [1, 2, 3]; let z = [0; 16]; "
            "let m: [2]f32 = [0.5, 1.5]; return a[0] + z.len; }");
    accepts("fn f() { let p = alloc(int, 4); p[0] = 1; free(p); "
            "let n = size_of([3]i16); }");
    rejects("fn f(x: int) -> int { return x[0]; }", 1, 30,
            "cannot index `int`");
    rejects("fn f(a: [4]int) -> int { return a[true]; }", 1, 35,
            "expected `int`, found `bool`");
    rejects("fn f(x: int) -> int { return *x; }", 1, 30,
            "unary `*` needs a pointer, found `int`");
    rejects("fn f() { let a = [0; 0]; }", 1, 22,
            "an array length is at least 1");
    rejects("fn f() { free(3); }", 1, 15, "`free` needs a pointer, found `int`");

    /* Calls, externs and function pointers. */
    accepts("extern fn printf(fmt: *byte, ...) -> i32;\n"
            "fn f() { printf(\"%d %f\\n\".ptr, 1, 2.5); }");
    accepts("fn add(a: i32, b: i32) -> i32 { return a + b; }\n"
            "fn f() -> i32 { let op: fn(i32, i32) -> i32 = add; "
            "return op(1, 2); }");
    rejects("fn g(a: int) {}\nfn f() { g(1, 2); }", 2, 10,
            "`g` takes 1 argument, found 2");
    rejects("fn g(a: int) {}\nfn f() { g(true); }", 2, 12,
            "expected `int`, found `bool`");
    rejects("extern fn printf(fmt: *byte, ...) -> i32;\n"
            "fn f(x: f32) { printf(\"\".ptr, x); }", 2, 31,
            "a variadic argument has type i32, u32, int, u64, float or a "
            "pointer, found `f32`");
    rejects("fn g() {}\nfn f() { let x = g(); }", 2, 18,
            "`g` returns no value");

    /* Constants. */
    accepts("const WIDTH: int = 320;\n"
            "const HEIGHT: int = WIDTH * 3 / 4;\n"
            "const SCREEN: [2]int = [WIDTH, HEIGHT];\n"
            "const PIXELS: int = SCREEN[0] * SCREEN[1];\n"
            "const WIDE: bool = WIDTH > HEIGHT && HEIGHT >= 200;\n"
            "struct P { tag: u8, value: i32 }\n"
            "const BYTES: int = size_of(P) * 2;\n"
            "const NAME: str = \"antic\";\n"
            "const LETTERS: int = \"antic\".len;\n"
            "fn f() -> [PIXELS]byte { const LOCAL: u32 = 1 << 4; "
            "return [0; PIXELS]; }");
    rejects("fn g() -> int { return 1; }\nconst X: int = g();", 2, 16,
            "a call is not a constant expression");
    rejects("const A: int = B;\nconst B: int = A;", 2, 16,
            "`A` depends on itself");
    rejects("const X: int = 1 / 0;", 1, 16, "`1 / 0` divides by zero");
    /* An operation on constant operands that C leaves undefined is a
       compile error in a function body too, and names its values. */
    rejects("fn f() -> int { return 7 / 0; }", 1, 24,
            "`7 / 0` divides by zero");
    rejects("fn f() -> int { return 7 % 0; }", 1, 24,
            "`7 % 0` divides by zero");
    rejects("const ZERO: int = 0;\nfn f() -> int { return 5 / ZERO; }", 2, 24,
            "`5 / 0` divides by zero");
    rejects("fn f() -> int { return (1 / 0) + 2; }", 1, 25,
            "`1 / 0` divides by zero");
    /* A literal operand keeps the spelling of the source, and any other
       operand shows its value as a literal would write it. */
    rejects("fn f() -> int { return 0x10 / 0; }", 1, 24,
            "`0x10 / 0` divides by zero");
    rejects("const BIG: f64 = 1.5e10;\nfn f() -> i32 { return BIG as i32; }",
            2, 24, "`15000000000.0 as i32` does not fit `i32`");
    rejects("const HUGE: f64 = 2.5e300;\nfn f() -> int { return HUGE as int; }",
            2, 24, "`2.5e300 as int` does not fit `int`");
    rejects("fn f() -> i8 { let m: i8 = -128; const N: i8 = -128 / -1; "
            "return m; }", 1, 48, "`-128 / -1` does not fit `i8`");
    rejects("fn f() -> i16 { return -32768 % -1; }", 1, 24,
            "`-32768 % -1` does not fit `i16`");
    rejects("fn f() -> int { return 1 << 64; }", 1, 24,
            "`1 << 64` shifts out of range");
    rejects("fn f() -> i32 { return 1 >> -1; }", 1, 24,
            "`1 >> -1` shifts out of range");
    rejects("fn f() -> i32 { return 30000000000.0 as i32; }", 1, 24,
            "`30000000000.0 as i32` does not fit `i32`");
    rejects("fn f() -> u8 { return -1.5 as u8; }", 1, 23,
            "`-1.5 as byte` does not fit `byte`");
    rejects("fn f() -> int { return (0.0 / 0.0) as int; }", 1, 25,
            "`nan as int` does not fit `int`");
    rejects("struct H { n: i32 }\nfn f() -> int { return size_of(H) >> 64; }",
            2, 24, "`size_of(H) >> 64` shifts out of range");
    /* A variable operand leaves the operation to run time, and a float
       division by zero gives an infinity. */
    accepts("fn f(x: int, n: int) -> int { return x / 0 + (1 << n); }\n"
            "fn g(x: f64) -> int { return x as int; }\n"
            "const INF: f64 = 1.0 / 0.0;\n"
            "const LAST: i8 = -127 / -1;\n"
            "const ALMOST: u8 = 255.9 as u8;");
    rejects("const X: int = 1;\nfn f() -> *int { return &X; }", 2, 26,
            "a constant has no address");
    accepts("const GRID: [3]int = [1, 2, 3];\n"
            "const CELLS: int = GRID.len;");
    rejects("fn f() -> int { let a = [1, 2, 3]; const N: int = a.len; "
            "return N; }", 1, 51, "a variable is not a constant expression");

    /* main */
    accepts("fn main(args: []str, env: []str) -> int { return args.len; }");
    rejects("fn main(n: int) -> int { return n; }", 1, 4,
            "`main` must be fn main() -> int, fn main(args: []str) -> int "
            "or fn main(args: []str, env: []str) -> int");
    rejects("import geometry;\nfn f() {}", 1, 8,
            "cannot find module `geometry`");
}
