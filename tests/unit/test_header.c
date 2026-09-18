#include "check.h"
#include "arena.h"
#include "ast.h"
#include "diagnostic.h"
#include "header.h"
#include "lexer.h"
#include "parser.h"
#include "sema.h"
#include "types.h"

/* Check source as module path, make its interface and compare the header
   that antic writes for it. */
static void header_of(const char *path, const char *source, bool bundled,
                      const char *expected)
{
    struct arena arena = {0};
    struct diagnostics diags = {0};
    struct token_list tokens = {0};
    struct module *module = NULL;
    struct types types;
    struct interface iface;
    const struct interface *ifaces[1];
    struct text out = {0};

    types_init(&types, &arena);
    if (!lex(source, strlen(source), &arena, &diags, &tokens) ||
        !parse(source, &tokens, &arena, &diags, &module) ||
        !sema_check(module, path, NULL, NULL, 0, &types, &arena, &diags, true)) {
        check_failures++;
        fprintf(stderr, "header source does not check: %s\n",
                diags.count > 0 ? diags.items[0].message : "");
    } else {
        sema_interface(module, path, &arena, &iface);
        ifaces[0] = &iface;
        header_write(&out, "geo", ifaces, 1, bundled);
        CHECK_STR(text_cstr(&out), expected);
    }
    text_free(&out);
    token_list_free(&tokens);
    diagnostics_free(&diags);
    arena_free(&arena);
}

void test_header(void)
{
    header_of("com.example.geo",
              "/// A point on the plane.\n"
              "export struct Vec2 {\n"
              "    /// Across.\n"
              "    x: c_int,\n"
              "    y: c_int,\n"
              "}\n"
              "export packed struct Tight { tag: u8, value: i32 }\n"
              "export struct Wide align(16) { a: i8, flags: u32 : 3 }\n"
              "export union Num { i: i64, d: f64 }\n"
              "export struct Holder { tags: [4]u8, cb: fn(c_long, *byte) -> bool,"
              " next: *Holder, n: Num }\n"
              "/// The largest count.\n"
              "export const LIMIT: int = 10;\n"
              "export const HALF: f64 = 0.5;\n"
              "export const ON: bool = true;\n"
              "export const NAME: str = \"g\\\"eo\";\n"
              "/// Dot product. The caller frees nothing.\n"
              "export fn dot(a: Vec2, b: *Vec2) -> c_int { return a.x * b.x; }\n"
              "export fn fill(p: *byte, n: c_size_t, w: c_wchar, u: uint) {}\n"
              "export struct Keys { register: c_int, NULL: c_int }\n"
              "export struct Breaks { a: u8, _: u32 : 0, b: u8 : 3 }\n"
              "export fn pick(default: c_int, new: c_int, k: Keys) -> c_int {\n"
              "    return default;\n"
              "}\n"
              "pub fn hidden() -> int { return 1; }\n",
              false,
              "/* geo.h, the C interface of com.example.geo, written by antic.\n"
              "   Do not edit. A failure that Anti cannot report calls abort(). */\n"
              "#ifndef GEO_H\n"
              "#define GEO_H\n"
              "\n"
              "#include <stdbool.h>\n"
              "#include <stddef.h>\n"
              "#include <stdint.h>\n"
              "\n"
              "#ifdef __cplusplus\n"
              "#define ANTI_ALIGNAS(n) alignas(n)\n"
              "extern \"C\" {\n"
              "#else\n"
              "#define ANTI_ALIGNAS(n) _Alignas(n)\n"
              "#endif\n"
              "\n"
              "/** A point on the plane. */\n"
              "typedef struct Vec2 {\n"
              "    /** Across. */\n"
              "    int32_t x;\n"
              "    int32_t y;\n"
              "} Vec2;\n"
              "\n"
              "#pragma pack(push, 1)\n"
              "typedef struct Tight {\n"
              "    uint8_t tag;\n"
              "    int32_t value;\n"
              "} Tight;\n"
              "#pragma pack(pop)\n"
              "\n"
              "typedef struct Wide {\n"
              "    ANTI_ALIGNAS(16) int8_t a;\n"
              "    uint32_t flags : 3;\n"
              "} Wide;\n"
              "\n"
              "typedef union Num {\n"
              "    int64_t i;\n"
              "    double d;\n"
              "} Num;\n"
              "\n"
              "typedef struct Holder {\n"
              "    uint8_t tags[4];\n"
              "    bool (*cb)(long, uint8_t *);\n"
              "    struct Holder *next;\n"
              "    Num n;\n"
              "} Holder;\n"
              "\n"
              "typedef struct Keys {\n"
              "    int32_t register_;\n"
              "    int32_t NULL_;\n"
              "} Keys;\n"
              "\n"
              "typedef struct Breaks {\n"
              "    uint8_t a;\n"
              "    uint32_t : 0;\n"
              "    uint8_t b : 3;\n"
              "} Breaks;\n"
              "\n"
              "/** The largest count. */\n"
              "#define LIMIT ((int64_t)10)\n"
              "#define HALF 0.5\n"
              "#define ON true\n"
              "static const char NAME[] = \"g\\\"eo\";\n"
              "\n"
              "/** Dot product. The caller frees nothing. */\n"
              "int32_t dot(Vec2 a, Vec2 *b);\n"
              "void fill(uint8_t *p, uint64_t n, wchar_t w, uint64_t u);\n"
              "int32_t pick(int32_t default_, int32_t new_, Keys k);\n"
              "\n"
              "#ifdef __cplusplus\n"
              "}\n"
              "#endif\n"
              "\n"
              "#endif\n");
    /* A bundled archive carries the runtime, and two in one program
       define it twice. */
    header_of("com.example.geo", "export fn one() -> int { return 1; }\n", true,
              "/* geo.h, the C interface of com.example.geo, written by antic.\n"
              "   Do not edit. A failure that Anti cannot report calls abort().\n"
              "   The archive holds the Anti runtime. Link only one archive\n"
              "   with a bundled runtime into a program. */\n"
              "#ifndef GEO_H\n"
              "#define GEO_H\n"
              "\n"
              "#include <stdbool.h>\n"
              "#include <stddef.h>\n"
              "#include <stdint.h>\n"
              "\n"
              "#ifdef __cplusplus\n"
              "#define ANTI_ALIGNAS(n) alignas(n)\n"
              "extern \"C\" {\n"
              "#else\n"
              "#define ANTI_ALIGNAS(n) _Alignas(n)\n"
              "#endif\n"
              "\n"
              "int64_t one(void);\n"
              "\n"
              "#ifdef __cplusplus\n"
              "}\n"
              "#endif\n"
              "\n"
              "#endif\n");
}
