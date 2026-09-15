#include "parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "text.h"

/* DESIGN: recursive descent, one function per grammar rule of chapter 2,
   with precedence climbing for the binary operators. After an error the
   parser is in panic mode and reports nothing more. It skips to the start
   of a statement or an item, so one mistake produces one message. */

struct parser {
    const char *source;
    const struct token *tokens;     /* without doc comments */
    const struct token *all;        /* with doc comments */
    size_t *origin;                 /* index in all of each token */
    size_t pos;
    struct arena *arena;
    struct diagnostics *diags;
    bool panic;
    bool ok;
    bool no_struct_literal;         /* inside a condition */
};

/* A growable array of fixed-size elements, copied into the memory pool
   when the list is complete. */
struct list {
    void *data;
    size_t count;
    size_t capacity;
    size_t size;
};

static void list_push(struct list *l, const void *element)
{
    if (l->count == l->capacity) {
        size_t capacity = l->capacity == 0 ? 8 : l->capacity * 2;
        void *data = realloc(l->data, capacity * l->size);
        if (data == NULL) {
            fputs("antic: out of memory\n", stderr);
            exit(70);
        }
        l->data = data;
        l->capacity = capacity;
    }
    memcpy((char *)l->data + l->count * l->size, element, l->size);
    l->count++;
}

static void *list_finish(struct parser *p, struct list *l, size_t *count)
{
    void *copy = NULL;

    *count = l->count;
    if (l->count > 0) {
        copy = arena_alloc(p->arena, l->count * l->size);
        memcpy(copy, l->data, l->count * l->size);
    }
    free(l->data);
    l->data = NULL;
    return copy;
}

static const struct token *peek(const struct parser *p)
{
    return &p->tokens[p->pos];
}

static const struct token *peek_at(const struct parser *p, size_t ahead)
{
    size_t i = p->pos;

    while (ahead > 0 && p->tokens[i].kind != TOKEN_EOF) {
        i++;
        ahead--;
    }
    return &p->tokens[i];
}

static bool check(const struct parser *p, enum token_kind kind)
{
    return peek(p)->kind == kind;
}

static const struct token *next(struct parser *p)
{
    const struct token *t = peek(p);

    if (t->kind != TOKEN_EOF) {
        p->pos++;
    }
    return t;
}

static bool accept(struct parser *p, enum token_kind kind)
{
    if (check(p, kind)) {
        next(p);
        return true;
    }
    return false;
}

static struct pos pos_of(const struct token *t)
{
    struct pos pos = {t->line, t->column};
    return pos;
}

static void error_here(struct parser *p, const char *message)
{
    p->ok = false;
    if (p->panic) {
        return;
    }
    p->panic = true;
    diagnostics_add(p->diags, peek(p)->line, peek(p)->column, "%s", message);
}

static bool expect(struct parser *p, enum token_kind kind)
{
    char message[64];

    if (accept(p, kind)) {
        return true;
    }
    snprintf(message, sizeof message, "expected %s", token_kind_name(kind));
    error_here(p, message);
    return false;
}

static bool expect_name(struct parser *p, struct name *name)
{
    const struct token *t = peek(p);

    if (!expect(p, TOKEN_IDENT)) {
        return false;
    }
    name->text = p->source + t->offset;
    name->length = t->length;
    return true;
}

static void *node(struct parser *p, size_t size)
{
    return arena_alloc(p->arena, size);
}

static bool is_doc(enum token_kind kind)
{
    return kind == TOKEN_DOC || kind == TOKEN_MODULE_DOC ||
           kind == TOKEN_NOTE || kind == TOKEN_MODULE_NOTE;
}

/* The text of the doc comments of one kind between the previous token and
   the current one. DESIGN: two comments of one kind join with a blank
   line, the paragraph break of the doc markup, so no text is lost. */
static struct doc_text doc_before(struct parser *p, enum token_kind kind)
{
    struct doc_text doc = {NULL, 0};
    struct text joined = {0};
    size_t i = p->pos == 0 ? 0 : p->origin[p->pos - 1] + 1;
    char *copy;

    for (; i < p->origin[p->pos]; i++) {
        const struct token *t = &p->all[i];
        if (t->kind != kind) {
            continue;
        }
        if (joined.length > 0) {
            text_append(&joined, "\n\n");
        }
        text_append_bytes(&joined, t->value.text.bytes, t->value.text.length);
    }
    if (joined.length > 0) {
        copy = node(p, joined.length + 1);
        memcpy(copy, text_cstr(&joined), joined.length + 1);
        doc.text = copy;
        doc.length = joined.length;
    }
    text_free(&joined);
    return doc;
}

/* Skip to the start of the next statement. That is past a semicolon, or
   up to a closing brace or a keyword that starts a statement. */
static void sync_statement(struct parser *p)
{
    for (;;) {
        switch (peek(p)->kind) {
        case TOKEN_SEMICOLON:
            next(p);
            p->panic = false;
            return;
        case TOKEN_RBRACE:
        case TOKEN_EOF:
        case TOKEN_LET:
        case TOKEN_CONST:
        case TOKEN_IF:
        case TOKEN_WHILE:
        case TOKEN_DO:
        case TOKEN_RETURN:
        case TOKEN_BREAK:
        case TOKEN_CONTINUE:
            p->panic = false;
            return;
        default:
            next(p);
        }
    }
}

/* Skip to the next item at the outermost brace level. */
static void sync_item(struct parser *p)
{
    int depth = 0;

    for (;;) {
        switch (peek(p)->kind) {
        case TOKEN_EOF:
            p->panic = false;
            return;
        case TOKEN_LBRACE:
            depth++;
            break;
        case TOKEN_RBRACE:
            if (depth > 0) {
                depth--;
            }
            break;
        case TOKEN_FN:
        case TOKEN_STRUCT:
        case TOKEN_UNION:
        case TOKEN_EXTERN:
        case TOKEN_CONST:
        case TOKEN_PUB:
        case TOKEN_EXPORT:
        case TOKEN_IMPORT:
            if (depth == 0) {
                p->panic = false;
                return;
            }
            break;
        default:
            break;
        }
        next(p);
    }
}

static struct expr *expression(struct parser *p);
static struct type_expr *type(struct parser *p);
static struct block *block(struct parser *p);

/* Types */

static bool is_builtin_type(enum token_kind kind)
{
    return kind >= TOKEN_BOOL_TYPE && kind <= TOKEN_C_WCHAR;
}

static struct type_expr *type(struct parser *p)
{
    const struct token *t = peek(p);
    struct type_expr *ty = node(p, sizeof *ty);

    ty->pos = pos_of(t);
    if (is_builtin_type(t->kind)) {
        next(p);
        ty->kind = TYPEX_BUILTIN;
        ty->builtin = t->kind;
    } else if (t->kind == TOKEN_IDENT) {
        ty->kind = TYPEX_NAMED;
        expect_name(p, &ty->name);
        if (accept(p, TOKEN_DOT)) {
            ty->module = ty->name;
            if (!expect_name(p, &ty->name)) {
                return NULL;
            }
        }
    } else if (accept(p, TOKEN_STAR)) {
        ty->kind = TYPEX_POINTER;
        if ((ty->element = type(p)) == NULL) {
            return NULL;
        }
    } else if (accept(p, TOKEN_LBRACKET)) {
        if (accept(p, TOKEN_RBRACKET)) {
            ty->kind = TYPEX_SLICE;
        } else {
            ty->kind = TYPEX_ARRAY;
            if ((ty->length = expression(p)) == NULL ||
                !expect(p, TOKEN_RBRACKET)) {
                return NULL;
            }
        }
        if ((ty->element = type(p)) == NULL) {
            return NULL;
        }
    } else if (accept(p, TOKEN_FN)) {
        struct list params = {NULL, 0, 0, sizeof(struct type_expr *)};

        ty->kind = TYPEX_FN;
        if (!expect(p, TOKEN_LPAREN)) {
            return NULL;
        }
        while (!check(p, TOKEN_RPAREN)) {
            struct type_expr *param = type(p);
            if (param == NULL) {
                free(params.data);
                return NULL;
            }
            list_push(&params, &param);
            if (!accept(p, TOKEN_COMMA)) {
                break;
            }
        }
        ty->params = list_finish(p, &params, &ty->param_count);
        if (!expect(p, TOKEN_RPAREN)) {
            return NULL;
        }
        if (accept(p, TOKEN_ARROW) && (ty->result = type(p)) == NULL) {
            return NULL;
        }
    } else {
        error_here(p, "expected a type");
        return NULL;
    }
    return ty;
}

/* Expressions */

static struct expr *new_expr(struct parser *p, enum expr_kind kind,
                             const struct token *at)
{
    struct expr *e = node(p, sizeof *e);

    e->kind = kind;
    e->pos = pos_of(at);
    e->spelling.bytes = p->source + at->offset;
    e->spelling.length = at->length;
    return e;
}

/* The comma-separated name: value pairs and the closing brace of a struct
   or slice literal. */
static struct field_init *field_inits(struct parser *p, size_t *count)
{
    struct list fields = {NULL, 0, 0, sizeof(struct field_init)};
    bool saved = p->no_struct_literal;

    p->no_struct_literal = false;
    while (!check(p, TOKEN_RBRACE)) {
        struct field_init f;
        f.pos = pos_of(peek(p));
        if (!expect_name(p, &f.name) || !expect(p, TOKEN_COLON) ||
            (f.value = expression(p)) == NULL) {
            free(fields.data);
            p->no_struct_literal = saved;
            return NULL;
        }
        list_push(&fields, &f);
        if (!accept(p, TOKEN_COMMA)) {
            break;
        }
    }
    p->no_struct_literal = saved;
    if (!expect(p, TOKEN_RBRACE)) {
        free(fields.data);
        return NULL;
    }
    return list_finish(p, &fields, count);
}

/* A comma-separated list of expressions up to the closing token. */
static struct expr **expressions(struct parser *p, enum token_kind close,
                                 size_t *count)
{
    struct list items = {NULL, 0, 0, sizeof(struct expr *)};

    while (!check(p, close)) {
        struct expr *e = expression(p);
        if (e == NULL) {
            free(items.data);
            return NULL;
        }
        list_push(&items, &e);
        if (!accept(p, TOKEN_COMMA)) {
            break;
        }
    }
    if (!expect(p, close)) {
        free(items.data);
        return NULL;
    }
    return list_finish(p, &items, count);
}

static struct expr *primary(struct parser *p)
{
    const struct token *t = peek(p);
    struct expr *e;

    switch (t->kind) {
    case TOKEN_INT:
        next(p);
        e = new_expr(p, EXPR_INT, t);
        e->as.integer = t->value.integer;
        return e;
    case TOKEN_FLOAT:
    case TOKEN_STRING:
    case TOKEN_BYTES:
        next(p);
        e = new_expr(p, t->kind == TOKEN_FLOAT    ? EXPR_FLOAT
                        : t->kind == TOKEN_STRING ? EXPR_STRING
                                                  : EXPR_BYTES,
                     t);
        e->as.text = t->value.text;
        return e;
    case TOKEN_CHAR:
        next(p);
        e = new_expr(p, EXPR_CHAR, t);
        e->as.character = t->value.character;
        return e;
    case TOKEN_TRUE:
    case TOKEN_FALSE:
        next(p);
        e = new_expr(p, EXPR_BOOL, t);
        e->as.boolean = t->kind == TOKEN_TRUE;
        return e;
    case TOKEN_NULL:
        next(p);
        return new_expr(p, EXPR_NULL, t);
    case TOKEN_IDENT: {
        bool qualified = peek_at(p, 1)->kind == TOKEN_DOT &&
                         peek_at(p, 2)->kind == TOKEN_IDENT &&
                         peek_at(p, 3)->kind == TOKEN_LBRACE;
        if (!p->no_struct_literal &&
            (qualified || peek_at(p, 1)->kind == TOKEN_LBRACE)) {
            e = new_expr(p, EXPR_STRUCT_LIT, t);
            expect_name(p, &e->as.struct_lit.name);
            if (qualified) {
                e->as.struct_lit.module = e->as.struct_lit.name;
                next(p);
                expect_name(p, &e->as.struct_lit.name);
            }
            next(p);
            e->as.struct_lit.fields =
                field_inits(p, &e->as.struct_lit.field_count);
            return p->panic ? NULL : e;
        }
        e = new_expr(p, EXPR_NAME, t);
        expect_name(p, &e->as.name);
        return e;
    }
    case TOKEN_LPAREN: {
        bool saved = p->no_struct_literal;
        next(p);
        p->no_struct_literal = false;
        e = expression(p);
        p->no_struct_literal = saved;
        return e != NULL && expect(p, TOKEN_RPAREN) ? e : NULL;
    }
    case TOKEN_LBRACKET:
        if (peek_at(p, 1)->kind == TOKEN_RBRACKET) {
            if (p->no_struct_literal) {
                break;
            }
            e = new_expr(p, EXPR_SLICE_LIT, t);
            next(p);
            next(p);
            if ((e->as.slice_lit.element = type(p)) == NULL ||
                !expect(p, TOKEN_LBRACE)) {
                return NULL;
            }
            e->as.slice_lit.fields =
                field_inits(p, &e->as.slice_lit.field_count);
            return p->panic ? NULL : e;
        }
        next(p);
        {
            struct expr *first = expression(p);
            if (first == NULL) {
                return NULL;
            }
            if (accept(p, TOKEN_SEMICOLON)) {
                e = new_expr(p, EXPR_ARRAY_REPEAT, t);
                e->as.array_repeat.value = first;
                if ((e->as.array_repeat.count = expression(p)) == NULL ||
                    !expect(p, TOKEN_RBRACKET)) {
                    return NULL;
                }
                return e;
            }
            e = new_expr(p, EXPR_ARRAY_LIT, t);
            {
                struct list items = {NULL, 0, 0, sizeof(struct expr *)};
                list_push(&items, &first);
                while (accept(p, TOKEN_COMMA) && !check(p, TOKEN_RBRACKET)) {
                    struct expr *item = expression(p);
                    if (item == NULL) {
                        free(items.data);
                        return NULL;
                    }
                    list_push(&items, &item);
                }
                e->as.array_lit.elements =
                    list_finish(p, &items, &e->as.array_lit.count);
            }
            return expect(p, TOKEN_RBRACKET) ? e : NULL;
        }
    case TOKEN_ALLOC:
        next(p);
        e = new_expr(p, EXPR_ALLOC, t);
        if (!expect(p, TOKEN_LPAREN) ||
            (e->as.alloc.type = type(p)) == NULL ||
            !expect(p, TOKEN_COMMA) ||
            (e->as.alloc.count = expression(p)) == NULL) {
            return NULL;
        }
        accept(p, TOKEN_COMMA);
        return expect(p, TOKEN_RPAREN) ? e : NULL;
    case TOKEN_FREE:
        next(p);
        e = new_expr(p, EXPR_FREE, t);
        if (!expect(p, TOKEN_LPAREN) ||
            (e->as.free_pointer = expression(p)) == NULL) {
            return NULL;
        }
        accept(p, TOKEN_COMMA);
        return expect(p, TOKEN_RPAREN) ? e : NULL;
    case TOKEN_SIZE_OF:
        next(p);
        e = new_expr(p, EXPR_SIZE_OF, t);
        if (!expect(p, TOKEN_LPAREN) || (e->as.size_of = type(p)) == NULL) {
            return NULL;
        }
        accept(p, TOKEN_COMMA);
        return expect(p, TOKEN_RPAREN) ? e : NULL;
    default:
        break;
    }
    error_here(p, "expected an expression");
    return NULL;
}

static struct expr *postfix(struct parser *p)
{
    struct expr *e = primary(p);

    while (e != NULL) {
        const struct token *t = peek(p);
        struct expr *outer;

        if (accept(p, TOKEN_LPAREN)) {
            bool saved = p->no_struct_literal;
            outer = new_expr(p, EXPR_CALL, t);
            outer->pos = e->pos;
            outer->as.call.callee = e;
            p->no_struct_literal = false;
            outer->as.call.args =
                expressions(p, TOKEN_RPAREN, &outer->as.call.arg_count);
            p->no_struct_literal = saved;
            if (p->panic) {
                return NULL;
            }
        } else if (accept(p, TOKEN_LBRACKET)) {
            bool saved = p->no_struct_literal;
            struct expr *first;
            p->no_struct_literal = false;
            first = expression(p);
            if (first != NULL && accept(p, TOKEN_DOT_DOT)) {
                outer = new_expr(p, EXPR_SLICE, t);
                outer->as.slice.base = e;
                outer->as.slice.low = first;
                outer->as.slice.high = expression(p);
            } else {
                outer = new_expr(p, EXPR_INDEX, t);
                outer->as.index.base = e;
                outer->as.index.index = first;
            }
            p->no_struct_literal = saved;
            outer->pos = e->pos;
            if (p->panic || !expect(p, TOKEN_RBRACKET)) {
                return NULL;
            }
        } else if (accept(p, TOKEN_DOT)) {
            outer = new_expr(p, EXPR_FIELD, t);
            outer->pos = e->pos;
            outer->as.field.base = e;
            if (!expect_name(p, &outer->as.field.name)) {
                return NULL;
            }
        } else {
            return e;
        }
        e = outer;
    }
    return NULL;
}

static struct expr *unary(struct parser *p)
{
    const struct token *t = peek(p);

    switch (t->kind) {
    case TOKEN_MINUS:
    case TOKEN_BANG:
    case TOKEN_TILDE:
    case TOKEN_STAR:
    case TOKEN_AMP: {
        struct expr *e;
        next(p);
        e = new_expr(p, EXPR_UNARY, t);
        e->as.unary.op = t->kind;
        e->as.unary.operand = unary(p);
        return e->as.unary.operand != NULL ? e : NULL;
    }
    default:
        return postfix(p);
    }
}

/* unary { "as" type }: as binds tighter than the binary operators. */
static struct expr *cast(struct parser *p)
{
    struct expr *e = unary(p);

    while (e != NULL && check(p, TOKEN_AS)) {
        struct expr *c = new_expr(p, EXPR_CAST, next(p));
        c->pos = e->pos;
        c->as.cast.operand = e;
        if ((c->as.cast.type = type(p)) == NULL) {
            return NULL;
        }
        e = c;
    }
    return e;
}

/* The precedence of a binary operator, from 1 for || to 10 for * / %.
   0 means the token is no binary operator. */
static int precedence(enum token_kind kind)
{
    switch (kind) {
    case TOKEN_OR_OR: return 1;
    case TOKEN_AND_AND: return 2;
    case TOKEN_PIPE: return 3;
    case TOKEN_CARET: return 4;
    case TOKEN_AMP: return 5;
    case TOKEN_EQ:
    case TOKEN_NE: return 6;
    case TOKEN_LT:
    case TOKEN_LE:
    case TOKEN_GT:
    case TOKEN_GE: return 7;
    case TOKEN_SHL:
    case TOKEN_SHR: return 8;
    case TOKEN_PLUS:
    case TOKEN_MINUS: return 9;
    case TOKEN_STAR:
    case TOKEN_SLASH:
    case TOKEN_PERCENT: return 10;
    default: return 0;
    }
}

/* Precedence climbing: parse operands and every operator that binds at
   least as tightly as min. The right operand only takes operators that
   bind tighter, which makes each level group from left to right. */
static struct expr *binary(struct parser *p, int min)
{
    struct expr *left = cast(p);

    while (left != NULL && precedence(peek(p)->kind) >= min) {
        const struct token *op = next(p);
        struct expr *e = new_expr(p, EXPR_BINARY, op);
        e->pos = left->pos;
        e->as.binary.op = op->kind;
        e->as.binary.left = left;
        e->as.binary.right = binary(p, precedence(op->kind) + 1);
        if (e->as.binary.right == NULL) {
            return NULL;
        }
        left = e;
    }
    return left;
}

static struct expr *expression(struct parser *p)
{
    return binary(p, 1);
}

/* A condition may not hold a struct or slice literal outside
   parentheses, because its '{' would be read as the body. */
static struct expr *condition(struct parser *p)
{
    bool saved = p->no_struct_literal;
    struct expr *e;

    p->no_struct_literal = true;
    e = expression(p);
    p->no_struct_literal = saved;
    return e;
}

/* Statements */

static bool is_assign_op(enum token_kind kind)
{
    return kind >= TOKEN_ASSIGN && kind <= TOKEN_SHR_ASSIGN;
}

static struct stmt *new_stmt(struct parser *p, enum stmt_kind kind,
                             const struct token *at)
{
    struct stmt *s = node(p, sizeof *s);

    s->kind = kind;
    s->pos = pos_of(at);
    return s;
}

static struct stmt *let_or_const(struct parser *p)
{
    const struct token *t = next(p);
    struct stmt *s = new_stmt(p, t->kind == TOKEN_LET ? STMT_LET : STMT_CONST,
                              t);

    s->as.let.name_pos = pos_of(peek(p));
    if (!expect_name(p, &s->as.let.name)) {
        return NULL;
    }
    if (t->kind == TOKEN_CONST ? !expect(p, TOKEN_COLON)
                               : !accept(p, TOKEN_COLON)) {
        if (p->panic) {
            return NULL;
        }
    } else if ((s->as.let.type = type(p)) == NULL) {
        return NULL;
    }
    if (!expect(p, TOKEN_ASSIGN) || (s->as.let.value = expression(p)) == NULL ||
        !expect(p, TOKEN_SEMICOLON)) {
        return NULL;
    }
    return s;
}

static struct stmt *if_statement(struct parser *p)
{
    const struct token *t = next(p);
    struct stmt *s = new_stmt(p, STMT_IF, t);
    struct list branches = {NULL, 0, 0, sizeof(struct if_branch)};

    for (;;) {
        struct if_branch b;
        if ((b.cond = condition(p)) == NULL || (b.body = block(p)) == NULL) {
            free(branches.data);
            return NULL;
        }
        list_push(&branches, &b);
        if (!accept(p, TOKEN_ELSE)) {
            break;
        }
        if (!accept(p, TOKEN_IF)) {
            if ((s->as.if_chain.else_body = block(p)) == NULL) {
                free(branches.data);
                return NULL;
            }
            break;
        }
    }
    s->as.if_chain.branches = list_finish(p, &branches, &s->as.if_chain.count);
    return s;
}

static struct stmt *statement(struct parser *p)
{
    const struct token *t = peek(p);
    struct stmt *s;

    switch (t->kind) {
    case TOKEN_LET:
    case TOKEN_CONST:
        return let_or_const(p);
    case TOKEN_IF:
        return if_statement(p);
    case TOKEN_WHILE:
        next(p);
        s = new_stmt(p, STMT_WHILE, t);
        if ((s->as.loop.cond = condition(p)) == NULL || !expect(p, TOKEN_DO) ||
            (s->as.loop.body = block(p)) == NULL) {
            return NULL;
        }
        return s;
    case TOKEN_DO:
        next(p);
        s = new_stmt(p, STMT_DO_WHILE, t);
        if ((s->as.loop.body = block(p)) == NULL ||
            !expect(p, TOKEN_WHILE) ||
            (s->as.loop.cond = condition(p)) == NULL) {
            return NULL;
        }
        return s;
    case TOKEN_BREAK:
    case TOKEN_CONTINUE:
        next(p);
        s = new_stmt(p, t->kind == TOKEN_BREAK ? STMT_BREAK : STMT_CONTINUE, t);
        return expect(p, TOKEN_SEMICOLON) ? s : NULL;
    case TOKEN_RETURN:
        next(p);
        s = new_stmt(p, STMT_RETURN, t);
        if (!check(p, TOKEN_SEMICOLON) &&
            (s->as.return_value = expression(p)) == NULL) {
            return NULL;
        }
        return expect(p, TOKEN_SEMICOLON) ? s : NULL;
    case TOKEN_LBRACE:
        s = new_stmt(p, STMT_BLOCK, t);
        return (s->as.block = block(p)) != NULL ? s : NULL;
    default:
        break;
    }

    {
        struct expr *e = expression(p);
        if (e == NULL) {
            return NULL;
        }
        if (is_assign_op(peek(p)->kind)) {
            s = new_stmt(p, STMT_ASSIGN, t);
            s->as.assign.op = next(p)->kind;
            s->as.assign.target = e;
            if ((s->as.assign.value = expression(p)) == NULL) {
                return NULL;
            }
        } else {
            if (!check(p, TOKEN_SEMICOLON)) {
                expect(p, TOKEN_SEMICOLON);
                return NULL;
            }
            if (e->kind != EXPR_CALL && e->kind != EXPR_FREE) {
                error_here(p, "expected a call or an assignment");
                return NULL;
            }
            s = new_stmt(p, STMT_EXPR, t);
            s->as.expr = e;
        }
        return expect(p, TOKEN_SEMICOLON) ? s : NULL;
    }
}

static struct block *block(struct parser *p)
{
    struct block *b = node(p, sizeof *b);
    struct list stmts = {NULL, 0, 0, sizeof(struct stmt *)};

    b->pos = pos_of(peek(p));
    if (!expect(p, TOKEN_LBRACE)) {
        return NULL;
    }
    while (!check(p, TOKEN_RBRACE) && !check(p, TOKEN_EOF)) {
        struct stmt *s = statement(p);
        if (s != NULL) {
            list_push(&stmts, &s);
        } else {
            sync_statement(p);
        }
    }
    b->stmts = list_finish(p, &stmts, &b->count);
    return expect(p, TOKEN_RBRACE) ? b : NULL;
}

/* Items */

/* A parenthesised list of name: type pairs. When allow_variadic is set,
   an ellipsis token may end the list. */
static struct param *params(struct parser *p, bool allow_variadic,
                            bool *variadic, size_t *count)
{
    struct list list = {NULL, 0, 0, sizeof(struct param)};

    if (!expect(p, TOKEN_LPAREN)) {
        return NULL;
    }
    while (!check(p, TOKEN_RPAREN)) {
        struct param param;
        memset(&param, 0, sizeof param);
        if (allow_variadic && list.count > 0 && accept(p, TOKEN_ELLIPSIS)) {
            *variadic = true;
            accept(p, TOKEN_COMMA);
            break;
        }
        param.pos = pos_of(peek(p));
        if (!expect_name(p, &param.name) || !expect(p, TOKEN_COLON) ||
            (param.type = type(p)) == NULL) {
            free(list.data);
            return NULL;
        }
        list_push(&list, &param);
        if (!accept(p, TOKEN_COMMA)) {
            break;
        }
    }
    if (!expect(p, TOKEN_RPAREN)) {
        free(list.data);
        return NULL;
    }
    return list_finish(p, &list, count);
}

/* Whether token t is the identifier word. */
static bool is_word(const struct parser *p, const struct token *t,
                    const char *word)
{
    return t->kind == TOKEN_IDENT && t->length == strlen(word) &&
           memcmp(p->source + t->offset, word, t->length) == 0;
}

static struct item *item(struct parser *p)
{
    const struct token *start = peek(p);
    struct item *it = node(p, sizeof *it);

    it->pos = pos_of(start);
    it->doc = doc_before(p, TOKEN_DOC);
    it->note = doc_before(p, TOKEN_NOTE);
    it->exported = accept(p, TOKEN_EXPORT);
    it->pub = it->exported || accept(p, TOKEN_PUB);
    /* DESIGN: export marks items that Anti defines for C. An extern fn is
       defined in C already. */
    if (it->exported && peek(p)->kind == TOKEN_EXTERN) {
        error_here(p, "an `extern fn` cannot be exported");
        return NULL;
    }
    /* DESIGN: packed and align are contextual words. packed is one only
       directly before struct or union, and align only between the name of
       a struct or union and its opening brace. */
    if (is_word(p, peek(p), "packed") &&
        (peek_at(p, 1)->kind == TOKEN_STRUCT ||
         peek_at(p, 1)->kind == TOKEN_UNION)) {
        next(p);
        it->packed = true;
    }
    it->name_pos = pos_of(peek_at(p, 1));
    if (peek(p)->kind == TOKEN_EXTERN) {
        it->name_pos = pos_of(peek_at(p, 2));
    }
    switch (peek(p)->kind) {
    case TOKEN_FN:
        next(p);
        it->kind = ITEM_FN;
        if (!expect_name(p, &it->name)) {
            return NULL;
        }
        it->params = params(p, false, &it->variadic, &it->param_count);
        if (p->panic ||
            (accept(p, TOKEN_ARROW) && (it->result = type(p)) == NULL) ||
            (it->body = block(p)) == NULL) {
            return NULL;
        }
        return it;
    case TOKEN_EXTERN:
        next(p);
        it->kind = ITEM_EXTERN_FN;
        if (!expect(p, TOKEN_FN) || !expect_name(p, &it->name)) {
            return NULL;
        }
        it->params = params(p, true, &it->variadic, &it->param_count);
        if (p->panic ||
            (accept(p, TOKEN_ARROW) && (it->result = type(p)) == NULL) ||
            !expect(p, TOKEN_SEMICOLON)) {
            return NULL;
        }
        return it;
    case TOKEN_STRUCT:
    case TOKEN_UNION: {
        struct list fields = {NULL, 0, 0, sizeof(struct param)};
        it->kind = next(p)->kind == TOKEN_UNION ? ITEM_UNION : ITEM_STRUCT;
        if (!expect_name(p, &it->name)) {
            return NULL;
        }
        if (is_word(p, peek(p), "align")) {
            next(p);
            if (!expect(p, TOKEN_LPAREN) ||
                (it->align = expression(p)) == NULL ||
                !expect(p, TOKEN_RPAREN)) {
                return NULL;
            }
        }
        if (!expect(p, TOKEN_LBRACE)) {
            return NULL;
        }
        do {
            struct param field;
            memset(&field, 0, sizeof field);
            field.doc = doc_before(p, TOKEN_DOC);
            field.note = doc_before(p, TOKEN_NOTE);
            field.pos = pos_of(peek(p));
            if (!expect_name(p, &field.name) || !expect(p, TOKEN_COLON) ||
                (field.type = type(p)) == NULL ||
                (accept(p, TOKEN_COLON) &&
                 (field.bits = expression(p)) == NULL)) {
                free(fields.data);
                return NULL;
            }
            list_push(&fields, &field);
        } while (accept(p, TOKEN_COMMA) && !check(p, TOKEN_RBRACE));
        it->params = list_finish(p, &fields, &it->param_count);
        return expect(p, TOKEN_RBRACE) ? it : NULL;
    }
    case TOKEN_CONST:
        next(p);
        it->kind = ITEM_CONST;
        if (!expect_name(p, &it->name) || !expect(p, TOKEN_COLON) ||
            (it->type = type(p)) == NULL || !expect(p, TOKEN_ASSIGN) ||
            (it->value = expression(p)) == NULL ||
            !expect(p, TOKEN_SEMICOLON)) {
            return NULL;
        }
        return it;
    default:
        error_here(p, "expected an item");
        return NULL;
    }
}

/* After an error in an import, skip the rest of its line up to and with
   its `;`. An import holds no braces, and a keyword in its path would
   stop sync_item inside it. */
static void sync_import(struct parser *p, int line)
{
    while (!check(p, TOKEN_EOF) && peek(p)->line == line) {
        if (next(p)->kind == TOKEN_SEMICOLON) {
            break;
        }
    }
    p->panic = false;
}

/* A module path: identifiers joined by dots. The name holds the path with
   its dots and without any space between the tokens. */
static bool module_path(struct parser *p, struct name *out)
{
    struct text path = {0};
    struct name segment;
    char *copy;

    do {
        if (!expect_name(p, &segment)) {
            text_free(&path);
            return false;
        }
        text_appendf(&path, "%s%.*s", path.length > 0 ? "." : "",
                     (int)segment.length, segment.text);
    } while (accept(p, TOKEN_DOT));
    copy = node(p, path.length + 1);
    memcpy(copy, text_cstr(&path), path.length + 1);
    out->text = copy;
    out->length = path.length;
    text_free(&path);
    return true;
}

bool parse(const char *source, const struct token_list *tokens,
           struct arena *arena, struct diagnostics *diags,
           struct module **out)
{
    struct parser p = {source, NULL, tokens->items, NULL, 0, arena, diags,
                       false, true, false};
    struct module *m = arena_alloc(arena, sizeof *m);
    struct list imports = {NULL, 0, 0, sizeof(struct import)};
    struct list items = {NULL, 0, 0, sizeof(struct item *)};
    struct token *kept = malloc(tokens->count * sizeof *kept);
    size_t *origin = malloc(tokens->count * sizeof *origin);
    size_t count = 0;
    size_t i;

    if (kept == NULL || origin == NULL) {
        fputs("antic: out of memory\n", stderr);
        exit(70);
    }
    /* DESIGN: the grammar rules never see a doc comment. Items, fields and
       the module ask for the comments that precede them, so a comment in
       any other place is dropped. */
    for (i = 0; i < tokens->count; i++) {
        if (!is_doc(tokens->items[i].kind)) {
            origin[count] = i;
            kept[count++] = tokens->items[i];
        }
    }
    p.tokens = kept;
    p.origin = origin;
    m->doc = doc_before(&p, TOKEN_MODULE_DOC);
    m->note = doc_before(&p, TOKEN_MODULE_NOTE);

    while (check(&p, TOKEN_IMPORT)) {
        struct import imp;
        memset(&imp, 0, sizeof imp);
        imp.pos = pos_of(next(&p));
        imp.module_pos = pos_of(peek(&p));
        if (module_path(&p, &imp.module) &&
            (!accept(&p, TOKEN_AS) || expect_name(&p, &imp.alias)) &&
            expect(&p, TOKEN_SEMICOLON)) {
            list_push(&imports, &imp);
        } else {
            sync_import(&p, imp.pos.line);
        }
    }
    while (!check(&p, TOKEN_EOF)) {
        size_t before = p.pos;
        struct item *it = item(&p);
        if (it != NULL) {
            list_push(&items, &it);
        } else {
            if (p.pos == before) {
                next(&p);
            }
            sync_item(&p);
        }
    }
    m->imports = list_finish(&p, &imports, &m->import_count);
    m->items = list_finish(&p, &items, &m->item_count);
    free(kept);
    free(origin);
    *out = m;
    return p.ok;
}
