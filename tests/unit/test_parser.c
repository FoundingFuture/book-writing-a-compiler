#include "check.h"
#include "arena.h"
#include "ast.h"
#include "diagnostic.h"
#include "lexer.h"
#include "parser.h"

struct parsed {
    struct arena arena;
    struct diagnostics diags;
    struct token_list tokens;
    struct module *module;
    bool ok;
};

static void parse_source(struct parsed *p, const char *source)
{
    memset(p, 0, sizeof *p);
    CHECK(lex(source, strlen(source), &p->arena, &p->diags, &p->tokens));
    p->ok = parse(source, &p->tokens, &p->arena, &p->diags, &p->module);
}

static void release(struct parsed *p)
{
    token_list_free(&p->tokens);
    diagnostics_free(&p->diags);
    arena_free(&p->arena);
}

static void tree(const char *source, const char *expected)
{
    struct parsed p;
    struct text out = {0};
    size_t i;

    parse_source(&p, source);
    CHECK(p.ok);
    for (i = 0; i < p.diags.count; i++) {
        fprintf(stderr, "unexpected: %d:%d: %s\n", p.diags.items[i].line,
                p.diags.items[i].column, p.diags.items[i].message);
    }
    if (p.module != NULL) {
        ast_dump(&out, p.module);
        CHECK_STR(text_cstr(&out), expected);
    }
    text_free(&out);
    release(&p);
}

struct expected_error {
    int line;
    int column;
    const char *message;
};

static void errors(const char *source, const struct expected_error *expected,
                   size_t n)
{
    struct parsed p;
    size_t i;

    parse_source(&p, source);
    CHECK(!p.ok);
    CHECK(p.diags.count == n);
    for (i = 0; i < p.diags.count; i++) {
        if (i >= n || p.diags.items[i].line != expected[i].line ||
            p.diags.items[i].column != expected[i].column ||
            strcmp(p.diags.items[i].message, expected[i].message) != 0) {
            check_failures++;
            fprintf(stderr, "diagnostic %zu: %d:%d: %s\n", i,
                    p.diags.items[i].line, p.diags.items[i].column,
                    p.diags.items[i].message);
        }
    }
    release(&p);
}

void test_parser(void)
{
    tree("fn scale(x: int) -> int {\n"
         "    let k = 2 + 4;\n"
         "    return x * k;\n"
         "}\n",
         "function scale\n"
         "  param x\n"
         "    type int\n"
         "  result\n"
         "    type int\n"
         "  block\n"
         "    let_stmt k\n"
         "      additive +\n"
         "        int_lit 2\n"
         "        int_lit 4\n"
         "    return_stmt\n"
         "      multiplicative *\n"
         "        ident x\n"
         "        ident k\n");

    tree("worker fn sum(chunk: []int, k: int) -> int {\n"
         "    return 0;\n"
         "}\n"
         "fn f(a: []int) {\n"
         "    let r = parallel a -> sum(2);\n"
         "    let s = parallel a by 4 -> sum(2);\n"
         "}\n",
         "worker function sum\n"
         "  param chunk\n"
         "    type []\n"
         "      type int\n"
         "  param k\n"
         "    type int\n"
         "  result\n"
         "    type int\n"
         "  block\n"
         "    return_stmt\n"
         "      int_lit 0\n"
         "function f\n"
         "  param a\n"
         "    type []\n"
         "      type int\n"
         "  block\n"
         "    let_stmt r\n"
         "      parallel\n"
         "        ident a\n"
         "        call\n"
         "          ident sum\n"
         "          int_lit 2\n"
         "    let_stmt s\n"
         "      parallel by\n"
         "        ident a\n"
         "        int_lit 4\n"
         "        call\n"
         "          ident sum\n"
         "          int_lit 2\n");

    tree("fn f() { x = a + b * c - d; }",
         "function f\n"
         "  block\n"
         "    simple_stmt =\n"
         "      ident x\n"
         "      additive -\n"
         "        additive +\n"
         "          ident a\n"
         "          multiplicative *\n"
         "            ident b\n"
         "            ident c\n"
         "        ident d\n");

    tree("fn f() { y = -x as u8 * 2; }",
         "function f\n"
         "  block\n"
         "    simple_stmt =\n"
         "      ident y\n"
         "      multiplicative *\n"
         "        cast\n"
         "          unary -\n"
         "            ident x\n"
         "          type u8\n"
         "        int_lit 2\n");

    tree("fn f() { ok = x & 1 == 0 || p && !q; }",
         "function f\n"
         "  block\n"
         "    simple_stmt =\n"
         "      ident ok\n"
         "      or_expr ||\n"
         "        bit_and &\n"
         "          ident x\n"
         "          equality ==\n"
         "            int_lit 1\n"
         "            int_lit 0\n"
         "        and_expr &&\n"
         "          ident p\n"
         "          unary !\n"
         "            ident q\n");

    tree("fn f() { v.scale(2.0)[i].x += s[1..n]; }",
         "function f\n"
         "  block\n"
         "    simple_stmt +=\n"
         "      field x\n"
         "        index\n"
         "          call\n"
         "            field scale\n"
         "              ident v\n"
         "            float_lit 2.0\n"
         "          ident i\n"
         "      slice\n"
         "        ident s\n"
         "        int_lit 1\n"
         "        ident n\n");

    tree("fn f() {\n"
         "    let p: Vec2 = Vec2 { x: 1.0, y: 2.0, };\n"
         "    let q = g.Vec2 { x: 0.0, y: 0.0 };\n"
         "    let s = []byte { ptr: b, len: 3 };\n"
         "    let a = [1, 2, 3];\n"
         "    let z = [0; 16];\n"
         "    let m = alloc(Vec2, 4);\n"
         "    free(m);\n"
         "    let n = size_of(*Vec2);\n"
         "    let c = 'a';\n"
         "    let t = \"hi\";\n"
         "    let nothing: *byte = null;\n"
         "    let yes = true;\n"
         "}\n",
         "function f\n"
         "  block\n"
         "    let_stmt p\n"
         "      type Vec2\n"
         "      struct_lit Vec2\n"
         "        field_init x\n"
         "          float_lit 1.0\n"
         "        field_init y\n"
         "          float_lit 2.0\n"
         "    let_stmt q\n"
         "      struct_lit g.Vec2\n"
         "        field_init x\n"
         "          float_lit 0.0\n"
         "        field_init y\n"
         "          float_lit 0.0\n"
         "    let_stmt s\n"
         "      slice_lit\n"
         "        type []\n"
         "          type byte\n"
         "        field_init ptr\n"
         "          ident b\n"
         "        field_init len\n"
         "          int_lit 3\n"
         "    let_stmt a\n"
         "      array_lit\n"
         "        int_lit 1\n"
         "        int_lit 2\n"
         "        int_lit 3\n"
         "    let_stmt z\n"
         "      array_lit ;\n"
         "        int_lit 0\n"
         "        int_lit 16\n"
         "    let_stmt m\n"
         "      alloc\n"
         "        type Vec2\n"
         "        int_lit 4\n"
         "    simple_stmt\n"
         "      free\n"
         "        ident m\n"
         "    let_stmt n\n"
         "      size_of\n"
         "        type *\n"
         "          type Vec2\n"
         "    let_stmt c\n"
         "      char_lit 'a'\n"
         "    let_stmt t\n"
         "      string_lit \"hi\"\n"
         "    let_stmt nothing\n"
         "      type *\n"
         "        type byte\n"
         "      null\n"
         "    let_stmt yes\n"
         "      true\n");

    tree("fn f(n: int) -> int {\n"
         "    if n < 0 { return -1; } else if (Vec2 { x: n }).x == 0 {\n"
         "        const Z: int = 0;\n"
         "    } else { { } }\n"
         "    while i < n do { i += 1; if i > 5 { break; } continue; }\n"
         "    do { i -= 1; } while i > 0\n"
         "    return;\n"
         "}\n",
         "function f\n"
         "  param n\n"
         "    type int\n"
         "  result\n"
         "    type int\n"
         "  block\n"
         "    if_stmt\n"
         "      branch\n"
         "        relation <\n"
         "          ident n\n"
         "          int_lit 0\n"
         "        block\n"
         "          return_stmt\n"
         "            unary -\n"
         "              int_lit 1\n"
         "      branch\n"
         "        equality ==\n"
         "          field x\n"
         "            struct_lit Vec2\n"
         "              field_init x\n"
         "                ident n\n"
         "          int_lit 0\n"
         "        block\n"
         "          const_decl Z\n"
         "            type int\n"
         "            int_lit 0\n"
         "      else\n"
         "        block\n"
         "          block\n"
         "    while_stmt\n"
         "      relation <\n"
         "        ident i\n"
         "        ident n\n"
         "      block\n"
         "        simple_stmt +=\n"
         "          ident i\n"
         "          int_lit 1\n"
         "        if_stmt\n"
         "          branch\n"
         "            relation >\n"
         "              ident i\n"
         "              int_lit 5\n"
         "            block\n"
         "              break\n"
         "        continue\n"
         "    do_stmt\n"
         "      block\n"
         "        simple_stmt -=\n"
         "          ident i\n"
         "          int_lit 1\n"
         "      relation >\n"
         "        ident i\n"
         "        int_lit 0\n"
         "    return_stmt\n");

    /* Doc comments attach to the item or field that follows them, and
       module docs to the module. Text of two comments of one marker joins
       with a blank line. A comment before anything else is dropped. */
    tree("//! Guide.\n"
         "//#! Notes.\n"
         "import geometry;\n"
         "/// Adds.\n"
         "//# Fast \"path\".\n"
         "pub fn f() {}\n"
         "/// A point.\n"
         "///\n"
         "///     Two fields.\n"
         "struct P {\n"
         "    /// Across.\n"
         "    x: int,\n"
         "}\n"
         "/// First.\n"
         "\n"
         "/// Second.\n"
         "const K: int = 1;\n",
         "module_doc \"Guide.\"\n"
         "module_note \"Notes.\"\n"
         "import geometry\n"
         "pub function f\n"
         "  doc \"Adds.\"\n"
         "  note \"Fast \\\"path\\\".\"\n"
         "  block\n"
         "struct_decl P\n"
         "  doc \"A point.\\n\\n    Two fields.\"\n"
         "  field x\n"
         "    doc \"Across.\"\n"
         "    type int\n"
         "const_decl K\n"
         "  doc \"First.\\n\\nSecond.\"\n"
         "  type int\n"
         "  int_lit 1\n");
    /* packed and align are words only in their positions, and ordinary
       names everywhere else. */
    tree("packed struct P {\n    a: u8,\n    align: i32,\n}\n"
         "union U align(16) {\n    packed: u8,\n}\n"
         "fn align(packed: int) -> int {\n    return packed;\n}\n",
         "struct_decl P\n"
         "  packed\n"
         "  field a\n"
         "    type u8\n"
         "  field align\n"
         "    type i32\n"
         "union_decl U\n"
         "  align\n"
         "    int_lit 16\n"
         "  field packed\n"
         "    type u8\n"
         "function align\n"
         "  param packed\n"
         "    type int\n"
         "  result\n"
         "    type int\n"
         "  block\n"
         "    return_stmt\n"
         "      ident packed\n");
    tree("struct Flags {\n    visible: u32 : 1,\n    layer: u32 : 2 + 2,\n}\n",
         "struct_decl Flags\n"
         "  field visible\n"
         "    type u32\n"
         "    bits\n"
         "      int_lit 1\n"
         "  field layer\n"
         "    type u32\n"
         "    bits\n"
         "      additive +\n"
         "        int_lit 2\n"
         "        int_lit 2\n");
    tree("export fn dot(a: int) -> int { return a; }\n"
         "export union U { a: i32 }\n"
         "export const K: int = 3;\n",
         "export function dot\n"
         "  param a\n"
         "    type int\n"
         "  result\n"
         "    type int\n"
         "  block\n"
         "    return_stmt\n"
         "      ident a\n"
         "export union_decl U\n"
         "  field a\n"
         "    type i32\n"
         "export const_decl K\n"
         "  type int\n"
         "  int_lit 3\n");
    tree("union Value {\n    i: i64,\n    f: f64,\n}\n",
         "union_decl Value\n"
         "  field i\n"
         "    type i64\n"
         "  field f\n"
         "    type f64\n");
    tree("fn f() {\n    /// Stray.\n    return;\n}\n/// Trailing.\n",
         "function f\n"
         "  block\n"
         "    return_stmt\n");

    tree("import com.niese.geo as g;\nimport anti.text;\n",
         "import com.niese.geo as g\n"
         "import anti.text\n");
    tree("import geometry;\n"
         "import geometry as g;\n"
         "pub struct Shape {\n"
         "    area: fn(*Shape) -> f64,\n"
         "    corners: [4]g.Vec2,\n"
         "    names: []str,\n"
         "}\n"
         "extern fn printf(fmt: *byte, ...) -> i32;\n"
         "pub const LIMIT: u32 = 1 << 4;\n"
         "fn apply(cb: fn(i32, i32,), x: i32) { cb(x, x,); }\n",
         "import geometry\n"
         "import geometry as g\n"
         "pub struct_decl Shape\n"
         "  field area\n"
         "    type fn\n"
         "      type *\n"
         "        type Shape\n"
         "      result\n"
         "        type f64\n"
         "  field corners\n"
         "    type [N]\n"
         "      int_lit 4\n"
         "      type g.Vec2\n"
         "  field names\n"
         "    type []\n"
         "      type str\n"
         "extern_fn printf\n"
         "  param fmt\n"
         "    type *\n"
         "      type byte\n"
         "  variadic\n"
         "  result\n"
         "    type i32\n"
         "pub const_decl LIMIT\n"
         "  type u32\n"
         "  shift <<\n"
         "    int_lit 1\n"
         "    int_lit 4\n"
         "function apply\n"
         "  param cb\n"
         "    type fn\n"
         "      type i32\n"
         "      type i32\n"
         "  param x\n"
         "    type i32\n"
         "  block\n"
         "    simple_stmt\n"
         "      call\n"
         "        ident cb\n"
         "        ident x\n"
         "        ident x\n");

    {
        static const struct expected_error e[] = {{3, 1, "expected `;`"}};
        errors("fn main() -> int {\n    return 42\n}\n", e, 1);
    }
    {
        static const struct expected_error e[] = {
            {2, 13, "expected an expression"}, {4, 5, "expected `;`"}};
        errors("fn f() {\n    let a = ;\n    let b = 1\n    return;\n}\n", e,
               2);
    }
    {
        static const struct expected_error e[] = {{1, 13, "expected `)`"}};
        errors("fn f(x: int -> int {}\nfn g() {}\n", e, 1);
    }
    {
        static const struct expected_error e[] = {{2, 21, "expected `;`"}};
        errors("fn f() {\n    if v == Vec2 { x: 1 } { }\n}\n", e, 1);
    }
    {
        static const struct expected_error e[] = {
            {1, 14, "expected identifier"}};
        errors("fn f() { let worker = 1; }", e, 1);
    }
    {
        static const struct expected_error e[] = {{1, 1, "expected an item"}};
        errors("let x = 1;\nfn g() {}\n", e, 1);
    }
    {
        static const struct expected_error e[] = {{1, 1, "expected an item"}};
        errors("packed fn g() {}\n", e, 1);
    }
    {
        static const struct expected_error e[] = {
            {1, 13, "expected identifier"}};
        errors("import anti.fn.x;\nfn main() -> int { return 0; }\n", e, 1);
    }
    {
        static const struct expected_error e[] = {{2, 1, "expected `;`"}};
        errors("import geometry\nfn main() -> int { return 0; }\n", e, 1);
    }
    {
        static const struct expected_error e[] = {
            {1, 8, "an `extern fn` cannot be exported"}};
        errors("export extern fn f() -> int;\nfn g() {}\n", e, 1);
    }
    {
        static const struct expected_error e[] = {
            {1, 28, "expected a type"}};
        errors("fn f() { let x: int = 1 as 5; }", e, 1);
    }

    /* A class body opens with its base and the interfaces it implements,
       then the fields with their visibility and defaults, then the
       constants and the functions. */
    tree("class Circle\n"
         "{\n"
         "    inherits Shape,\n"
         "    implements ser: Serializable,\n"
         "    use hit: Box,\n"
         "    r: f32 = 1.0,\n"
         "    pub label: str,\n"
         "    protected n: int,\n"
         "\n"
         "    const MAX: f32 = 9.0;\n"
         "\n"
         "    abstract fn area(self) -> f32;\n"
         "\n"
         "    pub fn grow(self, by: f32)\n"
         "    {\n"
         "        return;\n"
         "    }\n"
         "\n"
         "    protected fn hide(self) {}\n"
         "\n"
         "    operator fn add(self, o: Circle) -> Circle {}\n"
         "\n"
         "    concrete fn Serializable::serialize(self) {}\n"
         "}\n",
         "class_decl Circle\n"
         "  inherits Shape\n"
         "  field ser\n"
         "    type Serializable\n"
         "    implements\n"
         "  field hit\n"
         "    type Box\n"
         "    use\n"
         "  field r\n"
         "    type f32\n"
         "    default\n"
         "      float_lit 1.0\n"
         "  field label\n"
         "    type str\n"
         "    pub\n"
         "  field n\n"
         "    type int\n"
         "    protected\n"
         "  const_decl MAX\n"
         "    type f32\n"
         "    float_lit 9.0\n"
         "  pub function area\n"
         "    abstract\n"
         "    self\n"
         "    result\n"
         "      type f32\n"
         "  pub function grow\n"
         "    self\n"
         "    param by\n"
         "      type f32\n"
         "    block\n"
         "      return_stmt\n"
         "  protected function hide\n"
         "    self\n"
         "    block\n"
         "  pub function add\n"
         "    operator\n"
         "    self\n"
         "    param o\n"
         "      type Circle\n"
         "    result\n"
         "      type Circle\n"
         "    block\n"
         "  pub function serialize\n"
         "    concrete Serializable\n"
         "    self\n"
         "    block\n");
    /* A singleton has one instance, and `mutable` marks a field the
       program may write. */
    tree("singleton class Config\n"
         "{\n"
         "    path: str = \"c.toml\",\n"
         "    mutable title: str,\n"
         "}\n",
         "class_decl Config\n"
         "  singleton\n"
         "  field path\n"
         "    type str\n"
         "    default\n"
         "      string_lit \"c.toml\"\n"
         "  field title\n"
         "    type str\n"
         "    mutable\n");
    /* An enum names its underlying type and may give explicit values. */
    tree("enum Mode: u8 { A = 1, B }\n",
         "enum_decl Mode\n"
         "  base\n"
         "    type u8\n"
         "  value A\n"
         "    default\n"
         "      int_lit 1\n"
         "  value B\n");

    /* A struct body holds fields alone. Each of the four forms that
       belong to a class names the kind that takes it. */
    {
        static const struct expected_error e[] = {
            {3, 5, "a struct holds fields alone, and a function belongs to "
                   "a class"},
            {3, 14, "expected identifier"}};
        errors("struct P\n{\n    pub fn f(self) {}\n}\n", e, 2);
    }
    {
        static const struct expected_error e[] = {
            {1, 19, "a field of a struct has no default, which belongs to "
                    "a class"}};
        errors("struct P { x: int = 0 }\n", e, 1);
    }
    {
        static const struct expected_error e[] = {
            {1, 12, "a struct holds fields alone, and a base belongs to a "
                    "class"}};
        errors("struct P { inherits b: Q, x: int }\n", e, 1);
    }
    {
        static const struct expected_error e[] = {
            {1, 12, "a struct holds fields alone, and a constant belongs to "
                    "a class"},
            {1, 30, "expected an item"}};
        errors("struct P { const N: int = 1; }\n", e, 2);
    }
}
