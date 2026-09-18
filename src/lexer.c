#include "lexer.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "text.h"

/* The spelling of every keyword and symbol, indexed by kind. Keyword
   lookup, symbol matching and the names in messages all read this one
   table. */
enum category { CAT_OTHER, CAT_KEYWORD, CAT_SYMBOL };

struct kind_info {
    const char *spelling;
    enum category category;
};

static const struct kind_info kinds[TOKEN_KIND_COUNT] = {
    [TOKEN_AS] = {"as", CAT_KEYWORD},
    [TOKEN_BREAK] = {"break", CAT_KEYWORD},
    [TOKEN_CONST] = {"const", CAT_KEYWORD},
    [TOKEN_CONTINUE] = {"continue", CAT_KEYWORD},
    [TOKEN_DO] = {"do", CAT_KEYWORD},
    [TOKEN_ELSE] = {"else", CAT_KEYWORD},
    [TOKEN_EXPORT] = {"export", CAT_KEYWORD},
    [TOKEN_EXTERN] = {"extern", CAT_KEYWORD},
    [TOKEN_FN] = {"fn", CAT_KEYWORD},
    [TOKEN_IF] = {"if", CAT_KEYWORD},
    [TOKEN_IMPORT] = {"import", CAT_KEYWORD},
    [TOKEN_LET] = {"let", CAT_KEYWORD},
    [TOKEN_PUB] = {"pub", CAT_KEYWORD},
    [TOKEN_RETURN] = {"return", CAT_KEYWORD},
    [TOKEN_STRUCT] = {"struct", CAT_KEYWORD},
    [TOKEN_WHILE] = {"while", CAT_KEYWORD},
    [TOKEN_UNION] = {"union", CAT_KEYWORD},
    [TOKEN_TRUE] = {"true", CAT_KEYWORD},
    [TOKEN_FALSE] = {"false", CAT_KEYWORD},
    [TOKEN_NULL] = {"null", CAT_KEYWORD},
    [TOKEN_ALLOC] = {"alloc", CAT_KEYWORD},
    [TOKEN_FREE] = {"free", CAT_KEYWORD},
    [TOKEN_SIZE_OF] = {"size_of", CAT_KEYWORD},
    [TOKEN_BOOL_TYPE] = {"bool", CAT_KEYWORD},
    [TOKEN_BYTE_TYPE] = {"byte", CAT_KEYWORD},
    [TOKEN_CHAR_TYPE] = {"char", CAT_KEYWORD},
    [TOKEN_F32] = {"f32", CAT_KEYWORD},
    [TOKEN_F64] = {"f64", CAT_KEYWORD},
    [TOKEN_FLOAT_TYPE] = {"float", CAT_KEYWORD},
    [TOKEN_I8] = {"i8", CAT_KEYWORD},
    [TOKEN_I16] = {"i16", CAT_KEYWORD},
    [TOKEN_I32] = {"i32", CAT_KEYWORD},
    [TOKEN_I64] = {"i64", CAT_KEYWORD},
    [TOKEN_INT_TYPE] = {"int", CAT_KEYWORD},
    [TOKEN_STR_TYPE] = {"str", CAT_KEYWORD},
    [TOKEN_U8] = {"u8", CAT_KEYWORD},
    [TOKEN_U16] = {"u16", CAT_KEYWORD},
    [TOKEN_U32] = {"u32", CAT_KEYWORD},
    [TOKEN_U64] = {"u64", CAT_KEYWORD},
    [TOKEN_UINT_TYPE] = {"uint", CAT_KEYWORD},
    [TOKEN_C_CHAR] = {"c_char", CAT_KEYWORD},
    [TOKEN_C_DOUBLE] = {"c_double", CAT_KEYWORD},
    [TOKEN_C_FLOAT] = {"c_float", CAT_KEYWORD},
    [TOKEN_C_INT] = {"c_int", CAT_KEYWORD},
    [TOKEN_C_LONGLONG] = {"c_longlong", CAT_KEYWORD},
    [TOKEN_C_SHORT] = {"c_short", CAT_KEYWORD},
    [TOKEN_C_SIZE_T] = {"c_size_t", CAT_KEYWORD},
    [TOKEN_C_UCHAR] = {"c_uchar", CAT_KEYWORD},
    [TOKEN_C_UINT] = {"c_uint", CAT_KEYWORD},
    [TOKEN_C_ULONGLONG] = {"c_ulonglong", CAT_KEYWORD},
    [TOKEN_C_USHORT] = {"c_ushort", CAT_KEYWORD},
    [TOKEN_C_LONG] = {"c_long", CAT_KEYWORD},
    [TOKEN_C_ULONG] = {"c_ulong", CAT_KEYWORD},
    [TOKEN_C_WCHAR] = {"c_wchar", CAT_KEYWORD},
    [TOKEN_WORKER] = {"worker", CAT_KEYWORD},
    [TOKEN_PARALLEL] = {"parallel", CAT_KEYWORD},
    [TOKEN_SELF] = {"self", CAT_KEYWORD},
    [TOKEN_ABSTRACT] = {"abstract", CAT_KEYWORD},
    [TOKEN_CONCRETE] = {"concrete", CAT_KEYWORD},
    [TOKEN_ENUM] = {"enum", CAT_KEYWORD},
    [TOKEN_USE] = {"use", CAT_KEYWORD},
    [TOKEN_INHERITS] = {"inherits", CAT_KEYWORD},
    [TOKEN_CLASS] = {"class", CAT_KEYWORD},
    [TOKEN_SUPER] = {"super", CAT_KEYWORD},
    [TOKEN_IS] = {"is", CAT_KEYWORD},
    [TOKEN_DUP] = {"dup", CAT_KEYWORD},
    [TOKEN_DELETE] = {"delete", CAT_KEYWORD},
    [TOKEN_DESTROY] = {"destroy", CAT_KEYWORD},
    [TOKEN_STATIC] = {"static", CAT_KEYWORD},
    [TOKEN_ASSERT] = {"assert", CAT_KEYWORD},
    [TOKEN_SWITCH] = {"switch", CAT_KEYWORD},
    [TOKEN_FOR] = {"for", CAT_KEYWORD},
    [TOKEN_DEFER] = {"defer", CAT_KEYWORD},
    [TOKEN_IMPLEMENTS] = {"implements", CAT_KEYWORD},
    [TOKEN_SINGLETON] = {"singleton", CAT_KEYWORD},
    [TOKEN_INTERNAL] = {"internal", CAT_KEYWORD},
    [TOKEN_PROTECTED] = {"protected", CAT_KEYWORD},
    [TOKEN_CATCH] = {"catch", CAT_KEYWORD},
    [TOKEN_TRY] = {"try", CAT_KEYWORD},
    [TOKEN_YIELD] = {"yield", CAT_KEYWORD},
    [TOKEN_LPAREN] = {"(", CAT_SYMBOL},
    [TOKEN_RPAREN] = {")", CAT_SYMBOL},
    [TOKEN_LBRACE] = {"{", CAT_SYMBOL},
    [TOKEN_RBRACE] = {"}", CAT_SYMBOL},
    [TOKEN_LBRACKET] = {"[", CAT_SYMBOL},
    [TOKEN_RBRACKET] = {"]", CAT_SYMBOL},
    [TOKEN_COMMA] = {",", CAT_SYMBOL},
    [TOKEN_SEMICOLON] = {";", CAT_SYMBOL},
    [TOKEN_COLON] = {":", CAT_SYMBOL},
    [TOKEN_COLON_COLON] = {"::", CAT_SYMBOL},
    [TOKEN_DOT] = {".", CAT_SYMBOL},
    [TOKEN_DOT_DOT] = {"..", CAT_SYMBOL},
    [TOKEN_ELLIPSIS] = {"...", CAT_SYMBOL},
    [TOKEN_ARROW] = {"->", CAT_SYMBOL},
    [TOKEN_FAT_ARROW] = {"=>", CAT_SYMBOL},
    [TOKEN_PLUS] = {"+", CAT_SYMBOL},
    [TOKEN_MINUS] = {"-", CAT_SYMBOL},
    [TOKEN_STAR] = {"*", CAT_SYMBOL},
    [TOKEN_SLASH] = {"/", CAT_SYMBOL},
    [TOKEN_PERCENT] = {"%", CAT_SYMBOL},
    [TOKEN_AMP] = {"&", CAT_SYMBOL},
    [TOKEN_PIPE] = {"|", CAT_SYMBOL},
    [TOKEN_CARET] = {"^", CAT_SYMBOL},
    [TOKEN_TILDE] = {"~", CAT_SYMBOL},
    [TOKEN_BANG] = {"!", CAT_SYMBOL},
    [TOKEN_QUESTION] = {"?", CAT_SYMBOL},
    [TOKEN_ATOMIC] = {"atomic", CAT_KEYWORD},
    [TOKEN_DISPATCH] = {"dispatch", CAT_KEYWORD},
    [TOKEN_JOIN] = {"join", CAT_KEYWORD},
    [TOKEN_JOIN_ALL] = {"join_all", CAT_KEYWORD},
    [TOKEN_SHL] = {"<<", CAT_SYMBOL},
    [TOKEN_SHR] = {">>", CAT_SYMBOL},
    [TOKEN_AND_AND] = {"&&", CAT_SYMBOL},
    [TOKEN_OR_OR] = {"||", CAT_SYMBOL},
    [TOKEN_EQ] = {"==", CAT_SYMBOL},
    [TOKEN_NE] = {"!=", CAT_SYMBOL},
    [TOKEN_LT] = {"<", CAT_SYMBOL},
    [TOKEN_LE] = {"<=", CAT_SYMBOL},
    [TOKEN_GT] = {">", CAT_SYMBOL},
    [TOKEN_GE] = {">=", CAT_SYMBOL},
    [TOKEN_ASSIGN] = {"=", CAT_SYMBOL},
    [TOKEN_PLUS_ASSIGN] = {"+=", CAT_SYMBOL},
    [TOKEN_MINUS_ASSIGN] = {"-=", CAT_SYMBOL},
    [TOKEN_STAR_ASSIGN] = {"*=", CAT_SYMBOL},
    [TOKEN_SLASH_ASSIGN] = {"/=", CAT_SYMBOL},
    [TOKEN_PERCENT_ASSIGN] = {"%=", CAT_SYMBOL},
    [TOKEN_AMP_ASSIGN] = {"&=", CAT_SYMBOL},
    [TOKEN_PIPE_ASSIGN] = {"|=", CAT_SYMBOL},
    [TOKEN_CARET_ASSIGN] = {"^=", CAT_SYMBOL},
    [TOKEN_SHL_ASSIGN] = {"<<=", CAT_SYMBOL},
    [TOKEN_SHR_ASSIGN] = {">>=", CAT_SYMBOL},
};

/* The words chapter 2 reserves for threads and does not use yet. They lex
   as TOKEN_RESERVED, which no grammar rule accepts. Chapter 22 took
   `worker` and `parallel` out of the list and gave them a token. */
static const char *const reserved[] = {
    "chan", "recv", "select", "send", "sync", "thread",
};

struct lexer {
    const char *src;
    size_t length;
    size_t pos;
    int line;
    int column;
    struct arena *arena;
    struct diagnostics *diags;
    struct token_list *out;
    bool ok;
};

/* How escapes and NUL behave in a literal. */
enum literal_mode { MODE_CHAR, MODE_STR, MODE_BYTES };

static int at(const struct lexer *lx, size_t ahead)
{
    size_t i = lx->pos + ahead;
    return i < lx->length ? (unsigned char)lx->src[i] : -1;
}

static void advance(struct lexer *lx)
{
    if (lx->src[lx->pos] == '\n') {
        lx->line++;
        lx->column = 1;
    } else {
        lx->column++;
    }
    lx->pos++;
}

static bool is_digit(int c)
{
    return c >= '0' && c <= '9';
}

static bool is_hex(int c)
{
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int hex_value(int c)
{
    return is_digit(c) ? c - '0' : (c | 0x20) - 'a' + 10;
}

static bool is_ident_start(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_ident_char(int c)
{
    return is_ident_start(c) || is_digit(c);
}

static void error_at(struct lexer *lx, int line, int column,
                     const char *message)
{
    diagnostics_add(lx->diags, line, column, "%s", message);
    lx->ok = false;
}

static struct token *push(struct lexer *lx, enum token_kind kind,
                          size_t start, int line, int column)
{
    struct token_list *list = lx->out;
    struct token *t;

    if (list->count == list->capacity) {
        size_t capacity = list->capacity == 0 ? 256 : list->capacity * 2;
        struct token *items = realloc(list->items, capacity * sizeof *items);
        if (items == NULL) {
            fputs("antic: out of memory\n", stderr);
            exit(70);
        }
        list->items = items;
        list->capacity = capacity;
    }
    t = &list->items[list->count++];
    memset(t, 0, sizeof *t);
    t->kind = kind;
    t->line = line;
    t->column = column;
    t->offset = start;
    t->length = lx->pos - start;
    return t;
}

static struct token_text keep(struct lexer *lx, const struct text *bytes)
{
    struct token_text text;
    char *copy = arena_alloc(lx->arena, bytes->length + 1);

    memcpy(copy, text_cstr(bytes), bytes->length);
    text.bytes = copy;
    text.length = bytes->length;
    return text;
}

/* Append one byte to a buffer that may hold NUL bytes. */
static void append_byte(struct text *t, unsigned char byte)
{
    char one[2] = {(char)byte, '\0'};

    if (byte == 0) {
        /* text_append stops at NUL, so a zero byte goes in as a
           placeholder that is overwritten in place. */
        text_append(t, "?");
        t->data[t->length - 1] = '\0';
        return;
    }
    text_append(t, one);
}

static void append_utf8(struct text *t, uint32_t cp)
{
    if (cp < 0x80) {
        append_byte(t, (unsigned char)cp);
    } else if (cp < 0x800) {
        append_byte(t, (unsigned char)(0xC0 | (cp >> 6)));
        append_byte(t, (unsigned char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        append_byte(t, (unsigned char)(0xE0 | (cp >> 12)));
        append_byte(t, (unsigned char)(0x80 | ((cp >> 6) & 0x3F)));
        append_byte(t, (unsigned char)(0x80 | (cp & 0x3F)));
    } else {
        append_byte(t, (unsigned char)(0xF0 | (cp >> 18)));
        append_byte(t, (unsigned char)(0x80 | ((cp >> 12) & 0x3F)));
        append_byte(t, (unsigned char)(0x80 | ((cp >> 6) & 0x3F)));
        append_byte(t, (unsigned char)(0x80 | (cp & 0x3F)));
    }
}

/* Return the length of the valid UTF-8 sequence at s, or 0. RFC 3629
   excludes overlong forms, the surrogates D800 to DFFF and values above
   10FFFF. */
static size_t utf8_length(const unsigned char *s, size_t available,
                          uint32_t *cp)
{
    size_t n;
    uint32_t min;
    size_t i;

    if (s[0] < 0x80) {
        *cp = s[0];
        return 1;
    } else if ((s[0] & 0xE0) == 0xC0) {
        n = 2;
        min = 0x80;
        *cp = s[0] & 0x1F;
    } else if ((s[0] & 0xF0) == 0xE0) {
        n = 3;
        min = 0x800;
        *cp = s[0] & 0x0F;
    } else if ((s[0] & 0xF8) == 0xF0) {
        n = 4;
        min = 0x10000;
        *cp = s[0] & 0x07;
    } else {
        return 0;
    }
    if (n > available) {
        return 0;
    }
    for (i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            return 0;
        }
        *cp = (*cp << 6) | (s[i] & 0x3F);
    }
    if (*cp < min || *cp > 0x10FFFF || (*cp >= 0xD800 && *cp <= 0xDFFF)) {
        return 0;
    }
    return n;
}

/* Check the whole file before lexing, so that every later step can rely
   on valid UTF-8. */
static bool validate(struct lexer *lx)
{
    const unsigned char *s = (const unsigned char *)lx->src;
    int line = 1;
    int column = 1;
    size_t i = lx->pos;
    uint32_t cp;

    while (i < lx->length) {
        size_t n = utf8_length(s + i, lx->length - i, &cp);
        if (n == 0) {
            error_at(lx, line, column, "invalid UTF-8");
            return false;
        }
        if (cp == '\n') {
            line++;
            column = 1;
        } else {
            column += (int)n;
        }
        i += n;
    }
    return true;
}

/* The doc marker of the comment that starts ahead bytes from the current
   position, or TOKEN_EOF for an ordinary comment. Four slashes, an empty
   block comment and a third star stay ordinary. */
static enum token_kind doc_marker(const struct lexer *lx, size_t ahead,
                                  size_t *length)
{
    bool block = at(lx, ahead + 1) == '*';
    int c = at(lx, ahead + 2);
    int d = at(lx, ahead + 3);

    *length = 3;
    if (at(lx, ahead) != '/' || (at(lx, ahead + 1) != '/' && !block)) {
        return TOKEN_EOF;
    }
    if (c == '#' && d == '!') {
        *length = 4;
        return TOKEN_MODULE_NOTE;
    }
    if (c == '#') {
        return TOKEN_NOTE;
    }
    if (c == '!') {
        return TOKEN_MODULE_DOC;
    }
    if (!block && c == '/' && d != '/') {
        return TOKEN_DOC;
    }
    if (block && c == '*' && d != '*' && d != '/') {
        return TOKEN_DOC;
    }
    return TOKEN_EOF;
}

static bool is_blank(int c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

/* Push a doc token whose text is raw, lines separated by newlines, with
   the leading whitespace shared by all non-blank lines removed. */
static void push_doc(struct lexer *lx, enum token_kind kind, size_t start,
                     int line, int column, const struct text *raw)
{
    const char *s = text_cstr(raw);
    struct text out = {0};
    size_t common = SIZE_MAX;
    size_t i = 0;

    while (i <= raw->length) {
        size_t end = i;
        size_t lead = 0;
        while (end < raw->length && s[end] != '\n') {
            end++;
        }
        while (i + lead < end && is_blank(s[i + lead])) {
            lead++;
        }
        if (i + lead < end && lead < common) {
            common = lead;
        }
        i = end + 1;
    }
    for (i = 0; i <= raw->length;) {
        size_t end = i;
        size_t lead = 0;
        while (end < raw->length && s[end] != '\n') {
            end++;
        }
        while (i + lead < end && is_blank(s[i + lead])) {
            lead++;
        }
        if (i > 0) {
            text_append(&out, "\n");
        }
        if (i + lead < end) {
            text_append_bytes(&out, s + i + common, end - i - common);
        }
        i = end + 1;
    }
    push(lx, kind, start, line, column)->value.text = keep(lx, &out);
    text_free(&out);
}

/* Append the source bytes from the given offset to the current position,
   without the carriage return of a CRLF line end. */
static void take_line(struct lexer *lx, size_t from, struct text *raw)
{
    size_t n = lx->pos - from;

    if (n > 0 && lx->src[lx->pos - 1] == '\r') {
        n--;
    }
    text_append_bytes(raw, lx->src + from, n);
}

/* DESIGN: consecutive lines with one marker form one token, and a line
   without it ends the token, so a blank line separates two comments. */
static void line_doc(struct lexer *lx, enum token_kind kind, size_t marker)
{
    size_t start = lx->pos;
    int line = lx->line;
    int column = lx->column;
    struct text raw = {0};

    for (;;) {
        size_t next = 1;
        size_t length;
        size_t from;
        while (marker-- > 0) {
            advance(lx);
        }
        from = lx->pos;
        while (at(lx, 0) != -1 && at(lx, 0) != '\n') {
            advance(lx);
        }
        take_line(lx, from, &raw);
        if (at(lx, 0) != '\n') {
            break;
        }
        while (at(lx, next) == ' ' || at(lx, next) == '\t') {
            next++;
        }
        if (at(lx, next + 1) != '/' || doc_marker(lx, next, &length) != kind) {
            break;
        }
        while (next-- > 0) {
            advance(lx);
        }
        marker = length;
        text_append(&raw, "\n");
    }
    push_doc(lx, kind, start, line, column, &raw);
    text_free(&raw);
}

/* Skip to the end of a block comment that starts at the current position.
   Returns false at the end of the input. */
static bool skip_block(struct lexer *lx, size_t skip)
{
    while (skip-- > 0) {
        advance(lx);
    }
    while (!(at(lx, 0) == '*' && at(lx, 1) == '/')) {
        if (at(lx, 0) == -1) {
            return false;
        }
        advance(lx);
    }
    advance(lx);
    advance(lx);
    return true;
}

/* DESIGN: a block doc of one line holds its text between the opener and
   the closer, the form C programmers write for a short comment. Over
   several lines the text starts on the line after the opener and ends on
   the line before the closer. A line then holds text or a delimiter, and
   the block form gives the text of the line form. */
static void block_doc(struct lexer *lx, enum token_kind kind, size_t marker)
{
    size_t start = lx->pos;
    int line = lx->line;
    int column = lx->column;
    struct text raw = {0};
    char message[64];
    size_t i;

    for (i = 0; i < marker; i++) {
        advance(lx);
    }
    while (is_blank(at(lx, 0))) {
        advance(lx);
    }
    for (i = 0; at(lx, i) != -1 && at(lx, i) != '\n'; i++) {
        size_t from = lx->pos;
        if (at(lx, i) != '*' || at(lx, i + 1) != '/') {
            continue;
        }
        while (i > 0 && is_blank(at(lx, i - 1))) {
            i--;
        }
        while (lx->pos < from + i) {
            advance(lx);
        }
        text_append_bytes(&raw, lx->src + from, i);
        skip_block(lx, 0);
        push_doc(lx, kind, start, line, column, &raw);
        text_free(&raw);
        return;
    }
    if (at(lx, 0) != '\n') {
        snprintf(message, sizeof message,
                 "doc comment text starts on the line after `%.*s`",
                 (int)marker, lx->src + start);
        /* One message for one comment: a missing end outranks the text. */
        error_at(lx, line, column,
                 skip_block(lx, 0) ? message : "unterminated block comment");
        return;
    }
    advance(lx);
    for (i = 0;; i++) {
        size_t from = lx->pos;
        int text_line = lx->line;
        bool blank = true;
        while (at(lx, 0) != -1 && at(lx, 0) != '\n' &&
               !(at(lx, 0) == '*' && at(lx, 1) == '/')) {
            blank = blank && is_blank(at(lx, 0));
            advance(lx);
        }
        if (at(lx, 0) == -1) {
            error_at(lx, line, column, "unterminated block comment");
            text_free(&raw);
            return;
        }
        if (at(lx, 0) == '*') {
            advance(lx);
            advance(lx);
            if (!blank) {
                error_at(lx, text_line, 1,
                         "doc comment text ends on the line before `*/`");
                text_free(&raw);
                return;
            }
            break;
        }
        if (i > 0) {
            text_append(&raw, "\n");
        }
        take_line(lx, from, &raw);
        advance(lx);
    }
    push_doc(lx, kind, start, line, column, &raw);
    text_free(&raw);
}

/* Skip whitespace and ordinary comments. Block comments do not nest, so
   the first star-slash ends one. Stop before a doc comment. */
static void skip_trivia(struct lexer *lx)
{
    for (;;) {
        int c = at(lx, 0);
        size_t marker;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance(lx);
        } else if (c != '/' || doc_marker(lx, 0, &marker) != TOKEN_EOF) {
            return;
        } else if (at(lx, 1) == '/') {
            while (at(lx, 0) != -1 && at(lx, 0) != '\n') {
                advance(lx);
            }
        } else if (at(lx, 1) == '*') {
            int line = lx->line;
            int column = lx->column;
            if (!skip_block(lx, 2)) {
                error_at(lx, line, column, "unterminated block comment");
                return;
            }
        } else {
            return;
        }
    }
}

/* The kind of the word of n bytes at s: a keyword, TOKEN_RESERVED or
   TOKEN_IDENT. */
static enum token_kind word_kind(const char *s, size_t n)
{
    size_t i;
    int k;

    for (k = 0; k < TOKEN_KIND_COUNT; k++) {
        const char *spelling = kinds[k].spelling;
        if (kinds[k].category == CAT_KEYWORD && strlen(spelling) == n &&
            memcmp(spelling, s, n) == 0) {
            return (enum token_kind)k;
        }
    }
    for (i = 0; i < sizeof reserved / sizeof reserved[0]; i++) {
        if (strlen(reserved[i]) == n && memcmp(reserved[i], s, n) == 0) {
            return TOKEN_RESERVED;
        }
    }
    return TOKEN_IDENT;
}

bool lexer_is_keyword(const char *s, size_t n)
{
    return word_kind(s, n) != TOKEN_IDENT;
}

static void identifier(struct lexer *lx, size_t start, int line, int column)
{
    while (is_ident_char(at(lx, 0))) {
        advance(lx);
    }
    push(lx, word_kind(lx->src + start, lx->pos - start), start, line, column);
}

/* Read digits in the given base with '_' only between two digits. Digits
   go to digits when it is not NULL. Returns false on a misplaced '_'. */
static bool digit_run(struct lexer *lx, bool hex, struct text *digits)
{
    bool (*valid)(int) = hex ? is_hex : is_digit;
    char one[2] = {0, 0};

    while (valid(at(lx, 0)) || at(lx, 0) == '_') {
        if (at(lx, 0) == '_') {
            if (!valid(at(lx, 1))) {
                return false;
            }
        } else if (digits != NULL) {
            one[0] = (char)at(lx, 0);
            text_append(digits, one);
        }
        advance(lx);
    }
    return true;
}

static void number_error(struct lexer *lx, size_t start, int line,
                         int column, const char *message)
{
    error_at(lx, line, column, message);
    while (is_ident_char(at(lx, 0)) ||
           (at(lx, 0) == '.' && is_digit(at(lx, 1)))) {
        advance(lx);
    }
    push(lx, TOKEN_ERROR, start, line, column);
}

static void number(struct lexer *lx, size_t start, int line, int column)
{
    struct text digits = {0};
    uint64_t value = 0;
    bool hex = at(lx, 0) == '0' && at(lx, 1) == 'x';
    size_t i;

    if (hex) {
        advance(lx);
        advance(lx);
        if (at(lx, 0) == '_') {
            number_error(lx, start, line, column,
                         "`_` must stand between two digits");
            goto done;
        }
        if (!is_hex(at(lx, 0))) {
            number_error(lx, start, line, column,
                         "expected hexadecimal digits after 0x");
            goto done;
        }
    }
    if (!digit_run(lx, hex, &digits)) {
        number_error(lx, start, line, column,
                     "`_` must stand between two digits");
        goto done;
    }

    if (!hex && at(lx, 0) == '.' && is_digit(at(lx, 1))) {
        text_append(&digits, ".");
        advance(lx);
        if (!digit_run(lx, false, &digits)) {
            number_error(lx, start, line, column,
                         "`_` must stand between two digits");
            goto done;
        }
        if (at(lx, 0) == 'e' || at(lx, 0) == 'E') {
            char exponent[3] = {(char)at(lx, 0), 0, 0};
            advance(lx);
            if (at(lx, 0) == '+' || at(lx, 0) == '-') {
                exponent[1] = (char)at(lx, 0);
                advance(lx);
            }
            text_append(&digits, exponent);
            if (!is_digit(at(lx, 0))) {
                number_error(lx, start, line, column,
                             "expected digits in the exponent");
                goto done;
            }
            if (!digit_run(lx, false, &digits)) {
                number_error(lx, start, line, column,
                             "`_` must stand between two digits");
                goto done;
            }
        }
        if (is_ident_char(at(lx, 0))) {
            number_error(lx, start, line, column,
                         "invalid character in numeric literal");
            goto done;
        }
        push(lx, TOKEN_FLOAT, start, line, column)->value.text =
            keep(lx, &digits);
        goto done;
    }

    if (is_ident_char(at(lx, 0))) {
        number_error(lx, start, line, column,
                     "invalid character in numeric literal");
        goto done;
    }
    if (!hex && digits.length > 1 && digits.data[0] == '0') {
        number_error(lx, start, line, column,
                     "a decimal literal other than 0 does not start with 0");
        goto done;
    }
    for (i = 0; i < digits.length; i++) {
        uint64_t base = hex ? 16 : 10;
        uint64_t digit = (uint64_t)hex_value(digits.data[i]);
        if (value > (UINT64_MAX - digit) / base) {
            number_error(lx, start, line, column,
                         "integer literal is too large");
            goto done;
        }
        value = value * base + digit;
    }
    push(lx, TOKEN_INT, start, line, column)->value.integer = value;

done:
    text_free(&digits);
}

/* Decode one escape at the current '\'. Stores a scalar value, or a raw
   byte for \xHH in a byte string. Returns false after reporting an
   error. */
static bool escape(struct lexer *lx, enum literal_mode mode, uint32_t *value,
                   bool *raw_byte)
{
    int line = lx->line;
    int column = lx->column;
    int c = at(lx, 1);
    char message[80];

    *raw_byte = false;
    advance(lx);
    if (c == -1) {
        return false;
    }
    advance(lx);
    switch (c) {
    case 'n': *value = '\n'; return true;
    case 'r': *value = '\r'; return true;
    case 't': *value = '\t'; return true;
    case '\\': *value = '\\'; return true;
    case '"': *value = '"'; return true;
    case '\'': *value = '\''; return true;
    case '0':
        if (mode != MODE_BYTES) {
            error_at(lx, line, column, "NUL is not allowed here");
            return false;
        }
        *value = 0;
        *raw_byte = true;
        return true;
    case 'x':
        if (!is_hex(at(lx, 0)) || !is_hex(at(lx, 1))) {
            error_at(lx, line, column,
                     "`\\xHH` needs two hexadecimal digits");
            return false;
        }
        *value = (uint32_t)(hex_value(at(lx, 0)) * 16 + hex_value(at(lx, 1)));
        advance(lx);
        advance(lx);
        if (mode == MODE_BYTES) {
            *raw_byte = true;
            return true;
        }
        if (*value > 0x7F) {
            error_at(lx, line, column,
                     "`\\xHH` stops at `\\x7F` outside byte strings");
            return false;
        }
        if (*value == 0) {
            error_at(lx, line, column, "NUL is not allowed here");
            return false;
        }
        return true;
    case 'u': {
        int n = 0;
        uint32_t cp = 0;

        if (at(lx, 0) != '{') {
            error_at(lx, line, column,
                     "`\\u{}` needs 1 to 6 hexadecimal digits");
            return false;
        }
        advance(lx);
        while (is_hex(at(lx, 0)) && n < 7) {
            cp = cp * 16 + (uint32_t)hex_value(at(lx, 0));
            advance(lx);
            n++;
        }
        if (n == 0 || n > 6 || at(lx, 0) != '}') {
            error_at(lx, line, column,
                     "`\\u{}` needs 1 to 6 hexadecimal digits");
            return false;
        }
        advance(lx);
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            snprintf(message, sizeof message,
                     "`\\u{%X}` is not a Unicode scalar value", (unsigned)cp);
            error_at(lx, line, column, message);
            return false;
        }
        if (cp == 0 && mode != MODE_BYTES) {
            error_at(lx, line, column, "NUL is not allowed here");
            return false;
        }
        *value = cp;
        return true;
    }
    default:
        snprintf(message, sizeof message, "unknown escape `\\%c`",
                 c < 0x80 ? c : '?');
        error_at(lx, line, column, message);
        return false;
    }
}

static void character(struct lexer *lx, size_t start, int line, int column)
{
    uint32_t value = 0;
    bool raw_byte;
    bool valid = true;
    int c;

    advance(lx);
    c = at(lx, 0);
    if (c == '\'') {
        advance(lx);
        error_at(lx, line, column, "empty character literal");
        push(lx, TOKEN_ERROR, start, line, column);
        return;
    }
    if (c == -1 || c == '\n' || c == '\r') {
        error_at(lx, line, column, "unterminated character literal");
        push(lx, TOKEN_ERROR, start, line, column);
        return;
    }
    if (c == '\\') {
        valid = escape(lx, MODE_CHAR, &value, &raw_byte);
    } else {
        size_t n = utf8_length((const unsigned char *)lx->src + lx->pos,
                               lx->length - lx->pos, &value);
        while (n-- > 0) {
            advance(lx);
        }
    }
    if (at(lx, 0) != '\'') {
        size_t i = lx->pos;
        while (i < lx->length && lx->src[i] != '\'' && lx->src[i] != '\n') {
            i++;
        }
        if (i < lx->length && lx->src[i] == '\'') {
            error_at(lx, line, column,
                     "a character literal holds one character");
            while (lx->pos <= i) {
                advance(lx);
            }
        } else {
            error_at(lx, line, column, "unterminated character literal");
        }
        push(lx, TOKEN_ERROR, start, line, column);
        return;
    }
    advance(lx);
    if (!valid) {
        push(lx, TOKEN_ERROR, start, line, column);
        return;
    }
    push(lx, TOKEN_CHAR, start, line, column)->value.character = value;
}

/* A string literal: an optional r, b or br prefix, then n '#', a quote,
   the content and a quote followed by n '#'. */
static void string(struct lexer *lx, size_t start, int line, int column)
{
    struct text bytes = {0};
    bool raw = false;
    bool byte_string = false;
    enum literal_mode mode;
    size_t hashes = 0;
    bool valid = true;

    if (at(lx, 0) == 'b') {
        byte_string = true;
        advance(lx);
    }
    if (at(lx, 0) == 'r') {
        raw = true;
        advance(lx);
    }
    while (at(lx, 0) == '#') {
        hashes++;
        advance(lx);
    }
    advance(lx); /* the opening quote */
    mode = byte_string ? MODE_BYTES : MODE_STR;

    for (;;) {
        int c = at(lx, 0);

        if (c == -1) {
            error_at(lx, line, column, "unterminated string literal");
            push(lx, TOKEN_ERROR, start, line, column);
            text_free(&bytes);
            return;
        }
        if (c == '"') {
            size_t i = 0;
            while (i < hashes && at(lx, 1 + i) == '#') {
                i++;
            }
            if (i == hashes) {
                advance(lx);
                while (i-- > 0) {
                    advance(lx);
                }
                break;
            }
            append_byte(&bytes, '"');
            advance(lx);
        } else if (c == '\\' && !raw) {
            uint32_t value;
            bool raw_byte;
            if (escape(lx, mode, &value, &raw_byte)) {
                if (raw_byte) {
                    append_byte(&bytes, (unsigned char)value);
                } else {
                    append_utf8(&bytes, value);
                }
            } else {
                valid = false;
            }
        } else if (c == '\r' && at(lx, 1) == '\n') {
            append_byte(&bytes, '\n');
            advance(lx);
            advance(lx);
        } else if (c == 0 && mode != MODE_BYTES) {
            error_at(lx, lx->line, lx->column, "NUL is not allowed here");
            valid = false;
            advance(lx);
        } else {
            append_byte(&bytes, (unsigned char)c);
            advance(lx);
        }
    }

    if (valid) {
        push(lx, byte_string ? TOKEN_BYTES : TOKEN_STRING, start, line,
             column)->value.text = keep(lx, &bytes);
    } else {
        push(lx, TOKEN_ERROR, start, line, column);
    }
    text_free(&bytes);
}

/* A prefix of r, b or br starts a string literal when '#' characters and
   a quote follow it. A '#' without a prefix does the same. */
static bool starts_string(const struct lexer *lx)
{
    size_t i = 0;

    if (at(lx, i) == 'b') {
        i++;
    }
    if (at(lx, i) == 'r') {
        i++;
    }
    while (at(lx, i) == '#') {
        i++;
    }
    return at(lx, i) == '"';
}

static bool symbol(struct lexer *lx, size_t start, int line, int column)
{
    size_t best_length = 0;
    int best = -1;
    int k;

    for (k = 0; k < TOKEN_KIND_COUNT; k++) {
        const char *s = kinds[k].spelling;
        size_t n;
        if (kinds[k].category != CAT_SYMBOL) {
            continue;
        }
        n = strlen(s);
        if (n > best_length && lx->pos + n <= lx->length &&
            memcmp(s, lx->src + lx->pos, n) == 0) {
            best = k;
            best_length = n;
        }
    }
    if (best < 0) {
        return false;
    }
    while (best_length-- > 0) {
        advance(lx);
    }
    push(lx, (enum token_kind)best, start, line, column);
    return true;
}

bool lex(const char *source, size_t length, struct arena *arena,
         struct diagnostics *diags, struct token_list *out)
{
    struct lexer lx = {source, length, 0, 1, 1, arena, diags, out, true};

    /* DESIGN: a byte order mark is not part of the text, so positions
       start after it. */
    if (length >= 3 && memcmp(source, "\xEF\xBB\xBF", 3) == 0) {
        lx.pos = 3;
    }
    if (!validate(&lx)) {
        push(&lx, TOKEN_EOF, lx.pos, 1, 1);
        return false;
    }

    for (;;) {
        size_t start;
        int line;
        int column;
        int c;

        enum token_kind doc;
        size_t marker;

        skip_trivia(&lx);
        start = lx.pos;
        line = lx.line;
        column = lx.column;
        c = at(&lx, 0);
        if (c == -1) {
            push(&lx, TOKEN_EOF, start, line, column);
            return lx.ok;
        }

        doc = doc_marker(&lx, 0, &marker);
        if (doc != TOKEN_EOF && at(&lx, 1) == '/') {
            line_doc(&lx, doc, marker);
        } else if (doc != TOKEN_EOF) {
            block_doc(&lx, doc, marker);
        } else if ((c == 'r' || c == 'b' || c == '#' || c == '"') &&
            starts_string(&lx)) {
            string(&lx, start, line, column);
        } else if (is_ident_start(c)) {
            identifier(&lx, start, line, column);
        } else if (is_digit(c)) {
            number(&lx, start, line, column);
        } else if (c == '\'') {
            character(&lx, start, line, column);
        } else if (!symbol(&lx, start, line, column)) {
            uint32_t cp;
            size_t n = utf8_length((const unsigned char *)source + lx.pos,
                                   length - lx.pos, &cp);
            if (c >= 0x80) {
                error_at(&lx, line, column,
                         "unexpected character outside a literal");
            } else {
                char message[40];
                snprintf(message, sizeof message, "unexpected character `%c`",
                         c);
                error_at(&lx, line, column, message);
            }
            while (n-- > 0) {
                advance(&lx);
            }
            push(&lx, TOKEN_ERROR, start, line, column);
        }
    }
}

void token_list_free(struct token_list *list)
{
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

const char *token_kind_name(enum token_kind kind)
{
    static char names[TOKEN_KIND_COUNT][16];

    switch (kind) {
    case TOKEN_EOF: return "end of file";
    case TOKEN_ERROR: return "invalid token";
    case TOKEN_IDENT: return "identifier";
    case TOKEN_INT: return "integer literal";
    case TOKEN_FLOAT: return "float literal";
    case TOKEN_CHAR: return "character literal";
    case TOKEN_STRING: return "string literal";
    case TOKEN_BYTES: return "byte string literal";
    case TOKEN_RESERVED: return "reserved word";
    case TOKEN_DOC:
    case TOKEN_MODULE_DOC: return "doc comment";
    case TOKEN_NOTE:
    case TOKEN_MODULE_NOTE: return "developer note";
    default: break;
    }
    if (names[kind][0] == '\0') {
        snprintf(names[kind], sizeof names[kind], "`%s`",
                 kinds[kind].spelling);
    }
    return names[kind];
}

const char *token_category(enum token_kind kind)
{
    switch (kind) {
    case TOKEN_IDENT: return "ident";
    case TOKEN_INT: return "int_lit";
    case TOKEN_FLOAT: return "float_lit";
    case TOKEN_CHAR: return "char_lit";
    case TOKEN_STRING:
    case TOKEN_BYTES: return "string_lit";
    case TOKEN_RESERVED: return "keyword";
    case TOKEN_DOC:
    case TOKEN_MODULE_DOC:
    case TOKEN_NOTE:
    case TOKEN_MODULE_NOTE: return "doc";
    case TOKEN_EOF: return "eof";
    case TOKEN_ERROR: return "error";
    default: break;
    }
    return kinds[kind].category == CAT_KEYWORD ? "keyword" : "symbol";
}
