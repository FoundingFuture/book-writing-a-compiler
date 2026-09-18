#include "direct.h"

#include <stdio.h>
#include <string.h>

#include "arena.h"
#include "lexer.h"
#include "parser.h"

/* DESIGN: the direct path compiles exactly one program shape. Since
   chapter 5 it reads the syntax tree, so the parser reports every syntax
   error. The back end chapters replace the whole path with the pipeline. */

static bool fail(struct pos pos, struct diagnostic *diag, const char *message)
{
    diag->line = pos.line;
    diag->column = pos.column;
    snprintf(diag->message, sizeof diag->message, "%s", message);
    return false;
}

static bool is_name(const struct name *name, const char *text)
{
    return name->length == strlen(text) &&
           memcmp(name->text, text, name->length) == 0;
}

/* The returned value: an integer literal, or '-' before one, that fits
   int. */
static bool return_value(const struct expr *e, int64_t *value,
                         struct diagnostic *diag)
{
    const uint64_t max_positive = (uint64_t)INT64_MAX;
    bool negative = false;
    uint64_t magnitude;

    if (e->kind == EXPR_UNARY && e->as.unary.op == TOKEN_MINUS) {
        negative = true;
        e = e->as.unary.operand;
    }
    if (e->kind != EXPR_INT) {
        return fail(e->pos, diag, "expected an integer literal");
    }
    magnitude = e->as.integer;
    if (magnitude > max_positive + (negative ? 1u : 0u)) {
        return fail(e->pos, diag, "integer literal does not fit int");
    }
    if (negative) {
        *value = magnitude == max_positive + 1 ? INT64_MIN
                                               : -(int64_t)magnitude;
    } else {
        *value = (int64_t)magnitude;
    }
    return true;
}

bool direct_check(const struct module *module, int64_t *value,
                  struct diagnostic *diag)
{
    const struct item *it;
    const struct stmt *s;
    struct pos start = {1, 1};

    if (module->item_count != 1 || module->import_count != 0) {
        return fail(start, diag,
                    "the direct path compiles one function, main");
    }
    it = module->items[0];
    if (it->kind != ITEM_FN || !is_name(&it->name, "main")) {
        return fail(it->name_pos, diag, "expected `main`");
    }
    if (it->param_count != 0 || it->result == NULL ||
        it->result->kind != TYPEX_BUILTIN ||
        it->result->builtin != TOKEN_INT_TYPE) {
        return fail(it->name_pos, diag,
                    "the direct path compiles fn main() -> int");
    }
    if (it->body->count != 1 || it->body->stmts[0]->kind != STMT_RETURN ||
        it->body->stmts[0]->as.return_value == NULL) {
        return fail(it->body->pos, diag,
                    "the direct path compiles one return statement");
    }
    s = it->body->stmts[0];
    return return_value(s->as.return_value, value, diag);
}

bool direct_parse(const char *source, size_t length, int64_t *value,
                  struct diagnostic *diag)
{
    struct arena arena = {0};
    struct diagnostics diags = {0};
    struct token_list tokens = {0};
    struct module *module = NULL;
    bool ok = false;

    if (lex(source, length, &arena, &diags, &tokens) &&
        parse(source, &tokens, &arena, &diags, &module)) {
        ok = direct_check(module, value, diag);
    } else {
        *diag = diags.items[0];
    }
    token_list_free(&tokens);
    diagnostics_free(&diags);
    arena_free(&arena);
    return ok;
}
