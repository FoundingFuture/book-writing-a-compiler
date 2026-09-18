#include "check.h"
#include "arena.h"
#include "diagnostic.h"
#include "lexer.h"
#include "text.h"

struct lexed {
    struct arena arena;
    struct diagnostics diags;
    struct token_list tokens;
    bool ok;
};

static void lex_n(struct lexed *l, const char *source, size_t length)
{
    memset(l, 0, sizeof *l);
    l->ok = lex(source, length, &l->arena, &l->diags, &l->tokens);
}

static void lex_s(struct lexed *l, const char *source)
{
    lex_n(l, source, strlen(source));
}

static void done(struct lexed *l)
{
    token_list_free(&l->tokens);
    diagnostics_free(&l->diags);
    arena_free(&l->arena);
}

static void kinds(const char *source, const enum token_kind *expected,
                  size_t n)
{
    struct lexed l;
    size_t i;

    lex_s(&l, source);
    CHECK(l.ok);
    CHECK(l.tokens.count == n + 1);
    for (i = 0; i < n && i < l.tokens.count; i++) {
        if (l.tokens.items[i].kind != expected[i]) {
            check_failures++;
            fprintf(stderr, "%s: token %zu is %s, expected %s\n", source, i,
                    token_kind_name(l.tokens.items[i].kind),
                    token_kind_name(expected[i]));
        }
    }
    CHECK(l.tokens.count > 0 &&
          l.tokens.items[l.tokens.count - 1].kind == TOKEN_EOF);
    done(&l);
}

static void integer(const char *source, uint64_t value)
{
    struct lexed l;

    lex_s(&l, source);
    CHECK(l.ok && l.tokens.count == 2);
    CHECK(l.tokens.items[0].kind == TOKEN_INT);
    CHECK(l.tokens.items[0].value.integer == value);
    done(&l);
}

static void floating(const char *source, const char *digits)
{
    struct lexed l;

    lex_s(&l, source);
    CHECK(l.ok && l.tokens.count == 2);
    CHECK(l.tokens.items[0].kind == TOKEN_FLOAT);
    CHECK(l.tokens.items[0].value.text.length == strlen(digits));
    CHECK(memcmp(l.tokens.items[0].value.text.bytes, digits,
                 strlen(digits)) == 0);
    done(&l);
}

static void character(const char *source, uint32_t value)
{
    struct lexed l;

    lex_s(&l, source);
    CHECK(l.ok && l.tokens.count == 2);
    CHECK(l.tokens.items[0].kind == TOKEN_CHAR);
    CHECK(l.tokens.items[0].value.character == value);
    done(&l);
}

static void string(const char *source, enum token_kind kind,
                   const char *bytes, size_t length)
{
    struct lexed l;

    lex_s(&l, source);
    CHECK(l.ok && l.tokens.count == 2);
    CHECK(l.tokens.items[0].kind == kind);
    CHECK(l.tokens.items[0].value.text.length == length);
    CHECK(memcmp(l.tokens.items[0].value.text.bytes, bytes, length) == 0);
    done(&l);
}

static void error_n(const char *source, size_t length, int line, int column,
                    const char *message)
{
    struct lexed l;

    lex_n(&l, source, length);
    CHECK(!l.ok);
    CHECK(l.diags.count >= 1);
    if (l.diags.count >= 1) {
        CHECK(l.diags.items[0].line == line);
        CHECK(l.diags.items[0].column == column);
        CHECK_STR(l.diags.items[0].message, message);
    }
    done(&l);
}

static void error(const char *source, int line, int column,
                  const char *message)
{
    error_n(source, strlen(source), line, column, message);
}

/* The kinds and texts of the doc tokens in source, one per line of out as
   kind:text with newlines written as |. */
static void docs(const char *source, const char *expected)
{
    struct lexed l;
    struct text out = {0};
    size_t i;
    size_t k;

    lex_s(&l, source);
    CHECK(l.ok);
    for (i = 0; i < l.tokens.count; i++) {
        const struct token *t = &l.tokens.items[i];
        if (t->kind != TOKEN_DOC && t->kind != TOKEN_MODULE_DOC &&
            t->kind != TOKEN_NOTE && t->kind != TOKEN_MODULE_NOTE) {
            continue;
        }
        text_append(&out, t->kind == TOKEN_DOC          ? "doc:"
                          : t->kind == TOKEN_MODULE_DOC ? "module_doc:"
                          : t->kind == TOKEN_NOTE       ? "note:"
                                                        : "module_note:");
        for (k = 0; k < t->value.text.length; k++) {
            char c = t->value.text.bytes[k] == '\n' ? '|'
                                                    : t->value.text.bytes[k];
            text_append_bytes(&out, &c, 1);
        }
        text_append(&out, "\n");
    }
    CHECK_STR(text_cstr(&out), expected);
    text_free(&out);
    done(&l);
}

void test_lexer(void)
{
    /* Consecutive lines of one marker form one token. Common leading
       whitespace is stripped, so the line and block forms give one text. */
    docs("/// Dot product.\n///\n///     indented\nfn f() {}",
         "doc:Dot product.||    indented\n");
    docs("/**\n    Dot product.\n\n        indented\n*/\nfn f() {}",
         "doc:Dot product.||    indented\n");
    /* One line holds the opener, the text and the closer. */
    docs("/" "** Dot product. */\nfn f() {}", "doc:Dot product.\n");
    docs("/" "*!  Guide.  */\n/" "*# Keep. */\nfn f() {}",
         "module_doc:Guide.\nnote:Keep.\n");
    docs("/" "** */\nfn f() {}", "doc:\n");
    docs("//! Guide.\n//#! Build notes.\n//# Invariant.\nstruct S { a: int }",
         "module_doc:Guide.\nmodule_note:Build notes.\nnote:Invariant.\n");
    /* Split the openers so that a comment scanner does not read them as
       comments. */
    docs("/" "*!\n  Guide.\n*/\n/" "*#!\n  Notes.\n*/\n/" "*#\n  Keep.\n*/\n"
         "fn f() {}",
         "module_doc:Guide.\nmodule_note:Notes.\nnote:Keep.\n");
    /* Four slashes, an empty block and three stars stay ordinary. */
    docs("//// rule\n/**/\n/*** banner */\nfn f() {}", "");
    {
        static const enum token_kind k[] = {
            TOKEN_FN, TOKEN_IDENT, TOKEN_LPAREN, TOKEN_RPAREN, TOKEN_ARROW,
            TOKEN_INT_TYPE, TOKEN_LBRACE, TOKEN_RETURN, TOKEN_INT,
            TOKEN_SEMICOLON, TOKEN_RBRACE};
        kinds("fn main() -> int { return 42; }", k, sizeof k / sizeof k[0]);
    }
    {
        static const enum token_kind k[] = {
            TOKEN_DOT_DOT, TOKEN_ELLIPSIS, TOKEN_DOT, TOKEN_ARROW, TOKEN_MINUS,
            TOKEN_MINUS_ASSIGN, TOKEN_EQ, TOKEN_ASSIGN, TOKEN_NE, TOKEN_BANG,
            TOKEN_LE, TOKEN_SHL, TOKEN_SHL_ASSIGN, TOKEN_LT, TOKEN_GE,
            TOKEN_SHR, TOKEN_SHR_ASSIGN, TOKEN_GT, TOKEN_AND_AND, TOKEN_AMP,
            TOKEN_AMP_ASSIGN, TOKEN_OR_OR, TOKEN_PIPE, TOKEN_PIPE_ASSIGN,
            TOKEN_CARET, TOKEN_CARET_ASSIGN, TOKEN_TILDE, TOKEN_PLUS,
            TOKEN_PLUS_ASSIGN, TOKEN_STAR, TOKEN_STAR_ASSIGN, TOKEN_SLASH,
            TOKEN_SLASH_ASSIGN, TOKEN_PERCENT, TOKEN_PERCENT_ASSIGN,
            TOKEN_COMMA, TOKEN_COLON_COLON, TOKEN_COLON, TOKEN_SEMICOLON,
            TOKEN_LBRACKET,
            TOKEN_RBRACKET, TOKEN_LPAREN, TOKEN_RPAREN, TOKEN_LBRACE,
            TOKEN_RBRACE};
        kinds(".. ... . -> - -= == = != ! <= << <<= < >= >> >>= > && & &= "
              "|| | |= ^ ^= ~ + += * *= / /= % %= , :: : ; [ ] ( ) { }",
              k, sizeof k / sizeof k[0]);
    }
    {
        static const enum token_kind k[] = {
            TOKEN_AS, TOKEN_BREAK, TOKEN_CONST, TOKEN_CONTINUE, TOKEN_DO,
            TOKEN_ELSE, TOKEN_EXPORT, TOKEN_EXTERN, TOKEN_FN, TOKEN_IF,
            TOKEN_IMPORT,
            TOKEN_LET, TOKEN_PUB, TOKEN_RETURN, TOKEN_STRUCT, TOKEN_WHILE,
            TOKEN_UNION,
            TOKEN_TRUE, TOKEN_FALSE, TOKEN_NULL, TOKEN_ALLOC, TOKEN_FREE,
            TOKEN_SIZE_OF, TOKEN_BOOL_TYPE, TOKEN_BYTE_TYPE, TOKEN_CHAR_TYPE,
            TOKEN_F32, TOKEN_F64, TOKEN_FLOAT_TYPE, TOKEN_I8, TOKEN_I16,
            TOKEN_I32, TOKEN_I64, TOKEN_INT_TYPE, TOKEN_STR_TYPE, TOKEN_U8,
            TOKEN_U16, TOKEN_U32, TOKEN_U64, TOKEN_UINT_TYPE, TOKEN_C_CHAR,
            TOKEN_C_DOUBLE, TOKEN_C_FLOAT, TOKEN_C_INT, TOKEN_C_LONGLONG,
            TOKEN_C_SHORT, TOKEN_C_SIZE_T, TOKEN_C_UCHAR, TOKEN_C_UINT,
            TOKEN_C_ULONGLONG, TOKEN_C_USHORT, TOKEN_C_LONG, TOKEN_C_ULONG,
            TOKEN_C_WCHAR, TOKEN_WORKER, TOKEN_PARALLEL, TOKEN_SELF,
            TOKEN_ABSTRACT, TOKEN_CONCRETE, TOKEN_ENUM, TOKEN_USE,
            TOKEN_INHERITS, TOKEN_CLASS, TOKEN_SUPER, TOKEN_IS, TOKEN_DUP,
            TOKEN_DELETE, TOKEN_DESTROY, TOKEN_STATIC, TOKEN_ASSERT,
            TOKEN_SWITCH, TOKEN_FOR, TOKEN_DEFER, TOKEN_IMPLEMENTS,
            TOKEN_SINGLETON, TOKEN_INTERNAL, TOKEN_PROTECTED, TOKEN_CATCH,
            TOKEN_TRY, TOKEN_YIELD, TOKEN_RESERVED,
            TOKEN_IDENT, TOKEN_IDENT, TOKEN_IDENT, TOKEN_IDENT, TOKEN_IDENT,
            TOKEN_IDENT};
        kinds("as break const continue do else export extern fn if import let pub "
              "return struct while union true false null alloc free size_of bool "
              "byte char f32 f64 float i8 i16 i32 i64 int str u8 u16 u32 u64 "
              "uint c_char c_double c_float c_int c_longlong c_short c_size_t "
              "c_uchar c_uint c_ulonglong c_ushort c_long c_ulong c_wchar "
              "worker parallel self abstract concrete enum use inherits "
              "class super is dup delete destroy static assert switch for "
              "defer implements singleton internal protected catch try yield "
              "chan r b br _ Fn c_",
              k, sizeof k / sizeof k[0]);
    }
    {
        static const enum token_kind k[] = {TOKEN_IDENT, TOKEN_IDENT};
        kinds("a // comment\nb", k, 2);
        kinds("a /* x /* y */ b", k, 2);
        kinds("\xEF\xBB\xBF" "a b", k, 2);
    }
    {
        static const enum token_kind k[] = {TOKEN_INT, TOKEN_DOT_DOT,
                                            TOKEN_IDENT};
        kinds("0..n", k, 3);
    }

    {
        struct lexed l;
        lex_s(&l, "fn\n  main");
        CHECK(l.ok && l.tokens.count == 3);
        CHECK(l.tokens.items[1].line == 2 && l.tokens.items[1].column == 3);
        CHECK(l.tokens.items[1].offset == 5 && l.tokens.items[1].length == 4);
        done(&l);
        lex_s(&l, "\xEF\xBB\xBF" "fn");
        CHECK(l.ok && l.tokens.items[0].line == 1 &&
              l.tokens.items[0].column == 1);
        done(&l);
    }

    integer("0", 0);
    integer("42", 42);
    integer("1_000_000", 1000000);
    integer("0xFF", 255);
    integer("0xdead_beef", 3735928559u);
    integer("18446744073709551615", UINT64_MAX);
    integer("0xFFFF_FFFF_FFFF_FFFF", UINT64_MAX);

    floating("3.25", "3.25");
    floating("1.5e3", "1.5e3");
    floating("6.022_140_76e23", "6.02214076e23");
    floating("2.5E-3", "2.5E-3");
    floating("007.5", "007.5");

    character("'a'", 'a');
    character("'\xC3\xA9'", 0xE9);
    character("'\\n'", '\n');
    character("'\\''", '\'');
    character("'\\x41'", 0x41);
    character("'\\u{1F600}'", 0x1F600);

    string("\"hi\"", TOKEN_STRING, "hi", 2);
    string("\"a\\nb\\t\\\\\\\"\"", TOKEN_STRING, "a\nb\t\\\"", 6);
    string("\"\\u{E9}\"", TOKEN_STRING, "\xC3\xA9", 2);
    string("\"\\x41\"", TOKEN_STRING, "A", 1);
    /* A literal may span lines, and a CRLF in one becomes a single LF, so
       the bytes do not depend on the line ending of the file. */
    string("\"line\r\nnext\"", TOKEN_STRING, "line\nnext", 9);
    string("\"line\nnext\"", TOKEN_STRING, "line\nnext", 9);
    string("r\"line\r\nnext\"", TOKEN_STRING, "line\nnext", 9);
    string("r\"a\\nb\"", TOKEN_STRING, "a\\nb", 4);
    string("#\"say \"hi\"\"#", TOKEN_STRING, "say \"hi\"", 8);
    string("#\"tab\\t\"#", TOKEN_STRING, "tab\t", 4);
    string("r##\"a\"#b\"##", TOKEN_STRING, "a\"#b", 4);
    string("b\"\\xff\\0\"", TOKEN_BYTES, "\xff\0", 2);
    string("b\"\xC3\xA9\"", TOKEN_BYTES, "\xC3\xA9", 2);
    string("br\"\\x\"", TOKEN_BYTES, "\\x", 2);
    string("b#\"\"\"#", TOKEN_BYTES, "\"", 1);

    error("a \xC3\xA9", 1, 3, "unexpected character outside a literal");
    error("a \xff", 1, 3, "invalid UTF-8");
    error("\"\xED\xA0\x80\"", 1, 2, "invalid UTF-8");
    error("# x", 1, 1, "unexpected character `#`");
    error("a $ b", 1, 3, "unexpected character `$`");
    error("/* open", 1, 1, "unterminated block comment");
    error("/** text\n*/", 1, 1,
          "doc comment text starts on the line after `/**`");
    error("/**\ntext */", 2, 1,
          "doc comment text ends on the line before `*/`");
    {
        /* An unterminated doc block reports one message. */
        struct lexed l;
        lex_s(&l, "/" "** text\nfn f() {}\n");
        CHECK(l.diags.count == 1);
        if (l.diags.count == 1) {
            CHECK_STR(l.diags.items[0].message, "unterminated block comment");
        }
        done(&l);
    }

    error("18446744073709551616", 1, 1, "integer literal is too large");
    error("007", 1, 1, "a decimal literal other than 0 does not start with 0");
    error("1_", 1, 1, "`_` must stand between two digits");
    error("1__0", 1, 1, "`_` must stand between two digits");
    error("0x_F", 1, 1, "`_` must stand between two digits");
    error("0x", 1, 1, "expected hexadecimal digits after 0x");
    error("0XFF", 1, 1, "invalid character in numeric literal");
    error("42abc", 1, 1, "invalid character in numeric literal");
    error("1e9", 1, 1, "invalid character in numeric literal");
    error("1.0e", 1, 1, "expected digits in the exponent");

    error("''", 1, 1, "empty character literal");
    error("'ab'", 1, 1, "a character literal holds one character");
    error("'a", 1, 1, "unterminated character literal");
    error("'\\x80'", 1, 2, "`\\xHH` stops at `\\x7F` outside byte strings");
    error("'\\0'", 1, 2, "NUL is not allowed here");
    error("'\\u{0}'", 1, 2, "NUL is not allowed here");
    error("'\\u{D800}'", 1, 2, "`\\u{D800}` is not a Unicode scalar value");
    error("'\\u{110000}'", 1, 2, "`\\u{110000}` is not a Unicode scalar value");
    error("'\\u{}'", 1, 2, "`\\u{}` needs 1 to 6 hexadecimal digits");
    error("\"\\q\"", 1, 2, "unknown escape `\\q`");
    error("\"\\x80\"", 1, 2, "`\\xHH` stops at `\\x7F` outside byte strings");
    error("\"a\\0\"", 1, 3, "NUL is not allowed here");
    error_n("\"a\0b\"", 5, 1, 3, "NUL is not allowed here");
    error("\"open", 1, 1, "unterminated string literal");
    error("r#\"open\"", 1, 1, "unterminated string literal");
}
