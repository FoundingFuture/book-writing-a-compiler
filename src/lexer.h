#ifndef ANTIC_LEXER_H
#define ANTIC_LEXER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arena.h"
#include "diagnostic.h"

enum token_kind {
    TOKEN_EOF,
    TOKEN_ERROR,

    /* Names and literals */
    TOKEN_IDENT,
    TOKEN_INT,
    TOKEN_FLOAT,
    TOKEN_CHAR,
    TOKEN_STRING,
    TOKEN_BYTES,

    /* Doc comments, the text in value.text */
    TOKEN_DOC,
    TOKEN_MODULE_DOC,
    TOKEN_NOTE,
    TOKEN_MODULE_NOTE,

    /* Keywords */
    TOKEN_AS, TOKEN_BREAK, TOKEN_CONST, TOKEN_CONTINUE, TOKEN_DO,
    TOKEN_ELSE, TOKEN_EXPORT, TOKEN_EXTERN, TOKEN_FN, TOKEN_IF, TOKEN_IMPORT, TOKEN_LET,
    TOKEN_PUB, TOKEN_RETURN, TOKEN_STRUCT, TOKEN_WHILE, TOKEN_UNION,
    TOKEN_TRUE, TOKEN_FALSE, TOKEN_NULL,
    TOKEN_ALLOC, TOKEN_FREE, TOKEN_SIZE_OF,
    TOKEN_BOOL_TYPE, TOKEN_BYTE_TYPE, TOKEN_CHAR_TYPE, TOKEN_F32, TOKEN_F64,
    TOKEN_FLOAT_TYPE, TOKEN_I8, TOKEN_I16, TOKEN_I32, TOKEN_I64,
    TOKEN_INT_TYPE, TOKEN_STR_TYPE, TOKEN_U8, TOKEN_U16, TOKEN_U32,
    TOKEN_U64, TOKEN_UINT_TYPE,
    TOKEN_C_CHAR, TOKEN_C_DOUBLE, TOKEN_C_FLOAT, TOKEN_C_INT,
    TOKEN_C_LONGLONG, TOKEN_C_SHORT, TOKEN_C_SIZE_T, TOKEN_C_UCHAR,
    TOKEN_C_UINT, TOKEN_C_ULONGLONG, TOKEN_C_USHORT, TOKEN_C_LONG,
    TOKEN_C_ULONG, TOKEN_C_WCHAR,
    TOKEN_WORKER, TOKEN_PARALLEL,
    TOKEN_SELF, TOKEN_ABSTRACT, TOKEN_CONCRETE, TOKEN_ENUM, TOKEN_USE,
    TOKEN_INHERITS, TOKEN_CLASS, TOKEN_SUPER, TOKEN_IS, TOKEN_DUP,
    TOKEN_DELETE, TOKEN_DESTROY, TOKEN_STATIC,
    TOKEN_ASSERT, TOKEN_SWITCH, TOKEN_FOR, TOKEN_DEFER,
    TOKEN_IMPLEMENTS, TOKEN_SINGLETON, TOKEN_INTERNAL, TOKEN_PROTECTED,
    TOKEN_CATCH, TOKEN_TRY, TOKEN_YIELD,
    TOKEN_RESERVED,

    /* Operators and punctuation */
    TOKEN_LPAREN, TOKEN_RPAREN, TOKEN_LBRACE, TOKEN_RBRACE, TOKEN_LBRACKET,
    TOKEN_RBRACKET, TOKEN_COMMA, TOKEN_SEMICOLON, TOKEN_COLON,
    TOKEN_COLON_COLON, TOKEN_DOT,
    TOKEN_DOT_DOT, TOKEN_ELLIPSIS, TOKEN_ARROW, TOKEN_FAT_ARROW,
    TOKEN_PLUS, TOKEN_MINUS, TOKEN_STAR, TOKEN_SLASH, TOKEN_PERCENT,
    TOKEN_AMP, TOKEN_PIPE, TOKEN_CARET, TOKEN_TILDE, TOKEN_BANG,
    TOKEN_SHL, TOKEN_SHR, TOKEN_AND_AND, TOKEN_OR_OR,
    TOKEN_EQ, TOKEN_NE, TOKEN_LT, TOKEN_LE, TOKEN_GT, TOKEN_GE,
    TOKEN_ASSIGN, TOKEN_PLUS_ASSIGN, TOKEN_MINUS_ASSIGN, TOKEN_STAR_ASSIGN,
    TOKEN_SLASH_ASSIGN, TOKEN_PERCENT_ASSIGN, TOKEN_AMP_ASSIGN,
    TOKEN_PIPE_ASSIGN, TOKEN_CARET_ASSIGN, TOKEN_SHL_ASSIGN,
    TOKEN_SHR_ASSIGN, TOKEN_QUESTION, TOKEN_ATOMIC, TOKEN_DISPATCH,
    TOKEN_JOIN, TOKEN_JOIN_ALL,

    TOKEN_KIND_COUNT
};

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

/* Split source into tokens, ending with TOKEN_EOF. Report every error to
   diags and keep going, so that one run reports all of them. Returns true
   when no error occurred. */
bool lex(const char *source, size_t length, struct arena *arena,
         struct diagnostics *diags, struct token_list *out);

void token_list_free(struct token_list *list);

/* Whether the n bytes at s spell a keyword or a reserved word. */
bool lexer_is_keyword(const char *s, size_t n);

/* The kind as the grammar spells it, such as `fn` or identifier. */
const char *token_kind_name(enum token_kind kind);

/* The group of a kind in a token dump: keyword, ident, int_lit,
   float_lit, char_lit, string_lit, doc or symbol. */
const char *token_category(enum token_kind kind);

#endif
