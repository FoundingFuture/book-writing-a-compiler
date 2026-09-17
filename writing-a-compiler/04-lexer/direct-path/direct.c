#include "direct.h"

#include <stdio.h>
#include <string.h>

/* DESIGN: the direct path recognises exactly one program shape. Since
   chapter 4 it reads tokens from the lexer. The back end chapters replace
   the whole path with the pipeline. */

struct cursor {
    const struct token *token;
    const char *source;
};

static bool fail(const struct token *t, struct diagnostic *diag,
                 const char *message)
{
    diag->line = t->line;
    diag->column = t->column;
    snprintf(diag->message, sizeof diag->message, "%s", message);
    return false;
}

static bool expect(struct cursor *c, enum token_kind kind,
                   struct diagnostic *diag)
{
    char message[64];

    if (c->token->kind != kind) {
        snprintf(message, sizeof message, "expected %s",
                 token_kind_name(kind));
        return fail(c->token, diag, message);
    }
    c->token++;
    return true;
}

/* The chapter 3 shape names its function main. */
static bool expect_main(struct cursor *c, struct diagnostic *diag)
{
    const struct token *t = c->token;

    if (t->kind != TOKEN_IDENT || t->length != 4 ||
        memcmp(c->source + t->offset, "main", 4) != 0) {
        return fail(t, diag, "expected `main`");
    }
    c->token++;
    return true;
}

/* An optional '-' and an integer literal whose value fits int. */
static bool integer(struct cursor *c, int64_t *value, struct diagnostic *diag)
{
    const uint64_t max_positive = (uint64_t)INT64_MAX;
    bool negative = false;
    uint64_t magnitude;

    if (c->token->kind == TOKEN_MINUS) {
        negative = true;
        c->token++;
    }
    if (c->token->kind != TOKEN_INT) {
        return fail(c->token, diag, "expected an integer literal");
    }
    magnitude = c->token->value.integer;
    if (magnitude > max_positive + (negative ? 1u : 0u)) {
        return fail(c->token, diag, "integer literal does not fit int");
    }
    if (negative) {
        *value = magnitude == max_positive + 1 ? INT64_MIN
                                               : -(int64_t)magnitude;
    } else {
        *value = (int64_t)magnitude;
    }
    c->token++;
    return true;
}

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

bool direct_parse(const char *source, size_t length, int64_t *value,
                  struct diagnostic *diag)
{
    struct arena arena = {0};
    struct diagnostics diags = {0};
    struct token_list tokens = {0};
    bool ok;

    if (lex(source, length, &arena, &diags, &tokens)) {
        ok = direct_parse_tokens(source, &tokens, value, diag);
    } else {
        *diag = diags.items[0];
        ok = false;
    }
    token_list_free(&tokens);
    diagnostics_free(&diags);
    arena_free(&arena);
    return ok;
}
