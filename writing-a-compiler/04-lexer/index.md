---
title: "The lexer"
description: "A hand-written lexer in C that validates UTF-8 and splits Anti source into tokens with positions, from keywords and strings to doc comments in two forms."
summary: "Reading UTF-8 source into tokens by hand. Keywords, identifiers, numbers with separators and character literals. Comments that do not nest, the four doc comment markers in line and block form, and positions for error messages. Ordinary, raw and byte string literals with hash delimiters."
date: 2026-09-13T22:32:00+02:00
lastmod: 2026-09-15T03:50:39+02:00
draft: false
weight: 40
tags: [compilers, programming-languages]
keywords: [lexer, tokens, utf-8 validation, doc comments, block comments, escape sequences, raw string literals, hash delimiters]
---

## Previously

[Chapter 3, Setup and the first executable]({{% relref "/programming/writing-a-compiler/03-setup-and-first-executable" %}}), lists the tools that Linux, macOS and Windows need, from llvm-mc, lld and llvm-ar to the platform linker. It sets up the repository layout and the CMake build. A direct code path in antic then compiles a program that returns an exit code, so the toolchain runs before the pipeline exists.

## Token structure

The first stage of antic reads the bytes of a source file and groups them. Each group is a token, with a kind, a position and, for a literal, a value. The position is the line and the column where the token starts, both counted from 1, with the column counted in bytes. It also holds the byte offset and the length of the token's source text.

```c
/* Bytes that belong to a token: the decoded bytes of a string literal,
   or the digits of a float literal without separators. */
struct token_text {
    const char *bytes;
    size_t length;
};

struct token {
    enum token_kind kind;
    int line;
    int column;
    size_t offset;              /* first byte in the source */
    size_t length;              /* bytes of source text */
    union {
        uint64_t integer;       /* TOKEN_INT, the literal's magnitude */
        uint32_t character;     /* TOKEN_CHAR, a Unicode scalar value */
        struct token_text text; /* TOKEN_FLOAT, strings, doc comments */
    } value;
};

struct token_list {
    struct token *items;
    size_t count;
    size_t capacity;
};
```

The file `src/lexer.h` defines one kind per keyword, one per operator, one per group of literals and one per doc comment marker. The keyword `int` is `TOKEN_INT_TYPE`, and an integer literal such as `42` is `TOKEN_INT`. The six words that chapter 2 still reserves for threads share the kind `TOKEN_RESERVED`, which no grammar rule accepts. A doc comment token keeps its text in `value.text`, like a string literal.

The function `lex` reads the whole file and returns every token, followed by `TOKEN_EOF`. It stores the decoded bytes of string literals in a memory pool that hands out blocks and releases all of them together when the compilation ends. The pool is `struct arena` in `src/arena.h`. Errors go to a list of diagnostics.

```c
/* Split source into tokens, ending with TOKEN_EOF. Report every error to
   diags and keep going, so that one run reports all of them. Returns true
   when no error occurred. */
bool lex(const char *source, size_t length, struct arena *arena,
         struct diagnostics *diags, struct token_list *out);
```

## Error reporting

The lexer reports an error and continues with the next character. It appends a `TOKEN_ERROR` in place of the bad text, so a later stage still sees a token at that position. A file with several mistakes therefore produces one message per mistake in one run. The driver prints each diagnostic in the form `file:line:column: error: message` and stops after the lexer when any diagnostic exists.

The file below holds two mistakes, an unknown escape in a string and a character literal with two characters.

```anti
fn main() -> int
{
    let s = "tab\q";
    let c = 'ab';
    return 1;
}
```

```text
tests/errors/lexical.anti:3:14: error: unknown escape `\q`
tests/errors/lexical.anti:4:10: error: a character literal holds one character
```

The file indents each statement with a tab, which the listing shows as four spaces. A tab is one byte and therefore one column, so the backslash of `\q` is at column 14.

## Source validation

Chapter 2 specifies UTF-8 source. Before it produces a token, the lexer checks every byte of the file. UTF-8 encodes a character in one to four bytes, and RFC 3629 excludes overlong encodings, the values `0xD800` to `0xDFFF` and values above `0x10FFFF`[^1]. An overlong encoding uses more bytes than the value needs. The function `utf8_length` returns the length of one valid sequence, or 0.

```c
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
```

After this check, every later function in the lexer can assume valid UTF-8. A file that fails it produces the single message `invalid UTF-8` at the position of the first bad byte. The lexer skips a byte order mark, the bytes `EF BB BF`, at the start of the file, and positions start after it.

## Whitespace and comments

Spaces, tabs, carriage returns and line feeds separate tokens. A `//` comment runs to the end of the line. A `/*` comment ends at the first `*/` after it. Block comments do not nest, so a `/*` inside a block comment is plain comment text. A block comment without its end reports `unterminated block comment` at its start.

The source `/* a /* b */ c */` shows the rule. Its comment ends after `b`, and the rest, `c */`, lexes as the identifier `c` and the symbols `*` and `/`. Four markers turn a comment into a doc comment, which the next section defines. The function `skip_trivia` skips whitespace and ordinary comments and stops before a doc comment.

```c
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
```

The function `skip_block` passes the given number of opener bytes and then reads up to the first `*/`. It returns false when the input ends first.

```c
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
```

## Doc comments

A doc comment is a comment whose text documents the item or the module that follows it. Anti has four doc markers, and each marker has a line form and a block form. The lexer produces one token kind per marker, with the same text for both forms.

| Line form | Block form | Token kind | Documents |
|---|---|---|---|
| `///` | `/** */` | `TOKEN_DOC` | the next item, for whoever can see it |
| `//!` | `/*! */` | `TOKEN_MODULE_DOC` | the module, for its user |
| `//#` | `/*# */` | `TOKEN_NOTE` | the next item, for the developers of the library |
| `//#!` | `/*#! */` | `TOKEN_MODULE_NOTE` | the module, for its developers |

An item is a function, an external function, a struct, a union or a constant, and a struct field takes a doc comment too. The lexer produces the tokens without knowing what follows them. [Chapter 5]({{% relref "/programming/writing-a-compiler/05-parser-and-syntax-tree" %}}) attaches their text to items, fields and the module.

### Marker detection

The function `doc_marker` reads the opener of a comment and returns its token kind, or `TOKEN_EOF` for an ordinary comment. Three openers stay ordinary: `////`, the empty block comment `/**/` and `/***`. A rule line of slashes and a banner of stars are therefore ordinary comments.

```c
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
```

The parameter `ahead` lets the line form test the start of the next line before it consumes that line. The result `length` is the length of the marker, 4 for `//#!` and `/*#!` and 3 for the others.

### Line form

A line doc comment runs from its marker to the end of the line. Consecutive lines with the same marker form one token, and their texts join with a line feed. A line without that marker ends the token. A blank line or a line with another marker therefore starts another comment. Spaces and tabs may stand before the marker on every line.

```c
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
```

The helper `take_line` copies the text of one line without the carriage return of a CRLF line end. The token's position is the position of the first marker, and its source text spans all its lines.

### Block form

A block doc comment of one line holds its text between the opener and `*/`, as in `/** Dot product. */`. Over several lines it holds its text on the lines between the opener and the line of `*/`. The line of the opener then holds only whitespace after the marker, and the last line holds only whitespace before `*/`. Text in either place is a lexical error.

| Source | Message | Position |
|---|---|---|
| `/** text` with `*/` on a later line | `` doc comment text starts on the line after `/**` `` | the opener |
| `text */` on the last line | `` doc comment text ends on the line before `*/` `` | column 1 of that line |

Each line of the longer form then holds text or a delimiter, and the block form gives the text of the line form. A block doc comment without `*/` reports only `unterminated block comment` at its opener, even with text on the first line. Block doc comments do not nest either, so their text cannot contain `*/`. Text with `*/` needs the line form.

```c
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
```

### Common indentation

Both forms collect the raw lines and pass them to `push_doc`. It removes the leading whitespace that all non-blank lines share, and it strips nothing else. A line of only whitespace becomes an empty line. The raw text of the line form keeps the space after `///`, and the common strip removes it.

```c
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
```

Both forms of one comment give the same token. The line form is three `///` lines.

```anti
/// Dot product.
///
///     indented
fn f()
{
}
```

The block form puts the same lines between `/**` and `*/`, indented by four spaces.

```anti
/**
    Dot product.

        indented
*/
fn f()
{
}
```

Both produce one `TOKEN_DOC` with the same text of three lines. The third line keeps the four spaces that it has beyond the common indentation.

```text
Dot product.

    indented
```

A `*` at the start of each line of a block comment stays in the text, because the strip removes only whitespace.

### Doc token dump

The option `--dump-tokens` prints a doc token with its position, the group `doc` and the first line of its source text. The file `tests/dump/docs.anti` documents a function with three `///` lines.

```anti
/// Scale by a factor.
///
/// Both operands are int.
fn scale(x: int) -> int
{
    return x * 2;
}
```

```sh
build/antic --dump-tokens tests/dump/docs.anti
```

```text
1:1   doc      /// Scale by a factor.
4:1   keyword  fn
4:4   ident    scale
4:9   symbol   (
4:10  ident    x
4:11  symbol   :
4:13  keyword  int
4:16  symbol   )
4:18  symbol   ->
4:21  keyword  int
5:1   symbol   {
6:2   keyword  return
6:9   ident    x
6:11  symbol   *
6:13  int_lit  2
6:14  symbol   ;
7:1   symbol   }
```

The three lines form one token at 1:1, and the next token is `fn` at 4:1.

## Keywords and operators

A table indexed by token kind holds the spelling of every keyword and symbol. Keyword lookup, symbol matching and the names in error messages all read that one table, so a spelling exists in one place. The table starts with the 17 keywords of the core language, from `as` to `union`.

```c
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
```

The entries continue with `true`, `false`, `null`, `alloc`, `free` and `size_of`, then the type names and the symbols. The type names end with `uint` and the names of the C types for bindings.

```c
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
```

The table holds 87 keywords. The object model adds `class`, `self`, `super`, `abstract`, `concrete`, `enum`, `use`, `inherits`, `implements`, `is`, `dup`, `delete`, `destroy`, `static`, `singleton`, `internal`, `protected` and `atomic`. The error forms add `catch`, `try` and `yield`, and the core language adds `assert`, `switch`, `for` and `defer`.

The words `packed`, `align`, `final`, `own`, `operator`, `mutable` and `by` have no entry and lex as identifiers. The parser of chapter 5 gives each of these contextual words its meaning in one position.

- `packed` stands directly before `struct`, `union` or `class`.
- `align` stands between the name of a struct, union or class and its brace.
- `final` stands before `class` or `fn`.
- `own` stands before the name of a field.
- `operator` stands before `fn`.
- `mutable` stands before the name of a field of a singleton.
- `by` stands after the range of a `for` and after the array of a `parallel`.

The words `in` and `fatal` read the same way, the first after the binding of a `for` and the second after `catch`.

An identifier starts with a letter or `_` and continues with letters, digits and `_`. The function `word_kind` compares a word with the keywords of the table and with the reserved words. A word that matches neither is `TOKEN_IDENT`.

```c
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
```

The function `lexer_is_keyword` answers the same question for the rest of antic. [Chapter 9]({{% relref "/programming/writing-a-compiler/09-modules-and-library-files" %}}) uses it to refuse a keyword as a segment of a module path.

Operators follow the longest match. At `<<=` the lexer produces one `TOKEN_SHL_ASSIGN`, not `<<` and `=`, and at `...` one `TOKEN_ELLIPSIS`. The function `symbol` tries every symbol of the table and keeps the longest one that matches.

The object model adds three spellings to the table.

- `::` is `TOKEN_COLON_COLON`. It qualifies a `concrete fn` with the table that the function fills.
- `=>` is `TOKEN_FAT_ARROW`. It separates the value of a `switch` arm from its body.
- `as?` is two tokens, `TOKEN_AS` and `TOKEN_QUESTION`. The character `?` appears nowhere else, so the parser reads the pair where a cast may stand.

```c
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
```

The longest match also decides `&&x`, which is the operator `&&` followed by `x`. The address of an address therefore needs a space, as in `& &x`.

## Numeric literals

A numeric literal starts with a digit. Chapter 2 gives four rules for integers. The base is decimal, or hexadecimal after a lowercase `0x`. A `_` stands only between two digits. Of the decimal literals, only `0` itself starts with the digit `0`. No letter follows the digits. A float literal has digits on both sides of the dot and an optional exponent.

The function `digit_run` reads digits in one base and checks the position of each `_`. It collects the digits without the separators.

```c
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
```

The function `number` reads a hexadecimal prefix, then the integer digits. When a dot and a digit follow, it continues as a float literal. It stores the float's digits as text, because the type of the literal, `f32` or `f64`, is not known until semantic analysis. For an integer it computes the value as an unsigned 64-bit number and reports a value above 18446744073709551615 as too large. Semantic analysis later checks that the value fits the literal's type.

| Source | Result |
|---|---|
| `1_000_000` | integer 1000000 |
| `0xdead_beef` | integer 3735928559 |
| `6.022_140_76e23` | float with digits `6.02214076e23` |
| `0..n` | integer `0`, symbol `..`, identifier `n` |
| `007` | `a decimal literal other than 0 does not start with 0` |
| `1__0` | `` `_` must stand between two digits `` |
| `1e9` | `invalid character in numeric literal` |
| `1.0e` | `expected digits in the exponent` |
| `18446744073709551616` | `integer literal is too large` |

A float literal needs a digit after the dot. `0..n` therefore reads as three tokens, and the range syntax of slices needs no special case.

## Escapes

Character literals and string literals share the escapes of chapter 2. The function `escape` decodes one of them and knows the kind of literal it is in. In a character literal and in a `str` literal, `\xHH` stops at `\x7F` and NUL is an error. In a byte string, `\xHH` stores any byte and `\0` is allowed. A `\u{}` escape takes 1 to 6 hexadecimal digits and must name a Unicode scalar value, a code point outside `0xD800` to `0xDFFF` and at most `0x10FFFF`[^2].

```c
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
```

The function reports an error at the position of the backslash. It returns a raw byte for `\xHH` in a byte string. Every other escape returns a scalar value, which the string reader encodes as UTF-8.

## Character literals

A character literal holds exactly one character or one escape between single quotes. The lexer stores its value as a scalar value in `value.character`. Three shapes are errors: `''` is empty, `'ab'` holds two characters, and a literal without its closing quote on the same line is unterminated.

## String literals

Four prefixes select the kind of string: none and `r` give a `str`, `b` and `br` give a byte string. Any of them may add `#` characters before the opening quote. A letter `r` or `b` starts a string literal only when a quote follows the prefix and its `#` characters. Anywhere else it starts an identifier.

```c
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
```

The function `string` counts the `#` characters and then reads the content until a quote followed by the same number of `#`. A quote that is not followed by them is part of the content, so `#"say "hi""#` holds `say "hi"`. A raw literal copies every byte, including `\`. In every literal, a carriage return followed by a line feed becomes a single line feed.

```c
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
```

The decoded bytes of a literal can contain NUL, as in `b"\0"`. They go into a buffer that records its length and then into the memory pool.

| Source | Kind | Decoded bytes |
|---|---|---|
| `"a\nb"` | string | `a`, line feed, `b` |
| `"\u{E9}"` | string | `C3 A9`, the UTF-8 encoding of `é` |
| `r"a\nb"` | string | `a\nb`, four characters |
| `r##"a"#b"##` | string | `a"#b` |
| `b"\xff\0"` | byte string | `FF 00` |
| `b"é"` | byte string | `C3 A9` |

## Main loop

The function `lex` validates the file, skips a byte order mark and then loops. Each pass skips whitespace and ordinary comments and looks at the next character. A doc marker hands over to `line_doc` or `block_doc`, and any other character to the reader for its kind of token. A character that starts no token is an error.

```c
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
```

## Token dump

The option `--dump-tokens` prints each token with its position, its group and its source text. The groups are `keyword`, `ident`, `int_lit`, `float_lit`, `char_lit`, `string_lit`, `doc` and `symbol`. For the function `scale` of chapter 1, antic prints the listing that chapter 1 shows. The statements in `tests/dump/scale.anti` start with a tab, so `let` is at column 2.

```sh
build/antic --dump-tokens tests/dump/scale.anti
```

```text
1:1   keyword  fn
1:4   ident    scale
1:9   symbol   (
1:10  ident    x
1:11  symbol   :
1:13  keyword  int
1:16  symbol   )
1:18  symbol   ->
1:21  keyword  int
2:1   symbol   {
3:2   keyword  let
3:6   ident    k
3:8   symbol   =
3:10  int_lit  2
3:12  symbol   +
3:14  int_lit  4
3:15  symbol   ;
4:2   keyword  return
4:9   ident    x
4:11  symbol   *
4:13  ident    k
4:14  symbol   ;
5:1   symbol   }
```

The listing in chapter 1 is the same output.

## Direct path on tokens

In chapter 3 the route from source to assembly scanned characters. From this chapter on it reads the token list, which removes its own handling of whitespace and digits. The file `direct.c` of this version lives in `direct-path/` of this chapter's folder, and builds with the driver of chapter 3 and the lexer of `src/`. It checks each expected token in turn and stops at the first mismatch with `expected` and the name of the missing token.

```c
bool direct_parse_tokens(const char *source, const struct token_list *tokens,
                         int64_t *value, struct diagnostic *diag)
{
    struct cursor c = {tokens->items, source};

    return expect(&c, TOKEN_FN, diag) && expect_main(&c, diag) &&
           expect(&c, TOKEN_LPAREN, diag) && expect(&c, TOKEN_RPAREN, diag) &&
           expect(&c, TOKEN_ARROW, diag) && expect(&c, TOKEN_INT_TYPE, diag) &&
           expect(&c, TOKEN_LBRACE, diag) && expect(&c, TOKEN_RETURN, diag) &&
           integer(&c, value, diag) && expect(&c, TOKEN_SEMICOLON, diag) &&
           expect(&c, TOKEN_RBRACE, diag) && expect(&c, TOKEN_EOF, diag);
}
```

The minus sign and the integer literal are two tokens, and the range check uses the literal's 64-bit value. The error messages keep their positions, so chapter 3's program behaves as before.

## Checks

The tests of this chapter run with its source.

```sh
ctest --test-dir build -L chapter-4 -E excerpt
```

```text
100% tests passed out of 7
```

## Next

[Chapter 5, The parser and the syntax tree]({{% relref "/programming/writing-a-compiler/05-parser-and-syntax-tree" %}}), builds the parser by recursive descent with precedence climbing. It defines the syntax tree in C and the parsing of each declaration and statement. Its grammar functions read dotted import paths, unions, bitfields, `export` and the contextual words `packed` and `align`. It attaches doc comments to items, and its error recovery produces one message for one mistake.

## References

[^1]: F. Yergeau, *UTF-8, a transformation format of ISO 10646*, RFC 3629, 2003, section 3, https://www.rfc-editor.org/rfc/rfc3629

[^2]: Unicode Consortium, *Glossary of Unicode Terms*, entry "Unicode Scalar Value", https://www.unicode.org/glossary/#unicode_scalar_value
