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
    bool *taken;                    /* the doc comments that were read */
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

/* The length of the doc marker at s, 4 for the two developer markers of
   the module and 3 for the six others. */
static size_t marker_length(const char *s)
{
    return s[2] == '#' && s[3] == '!' ? 4 : 3;
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
        p->taken[i] = true;
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
static struct expr *postfix(struct parser *p);
static bool is_word(const struct parser *p, const struct token *t,
                    const char *word);
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
    /* `self` is the receiver of a function of a struct body. It reads as
       a name, and the checker gives it the type *T. */
    case TOKEN_SELF:
        next(p);
        e = new_expr(p, EXPR_NAME, t);
        e->as.name.text = p->source + t->offset;
        e->as.name.length = t->length;
        return e;
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
        /* DESIGN: `alloc T { ... }` puts one object on the heap and
           writes the literal into it. `alloc(T, n)` stays the raw form
           for any type and any count. */
        if (!check(p, TOKEN_LPAREN)) {
            if ((e->as.alloc.value = expression(p)) == NULL) {
                return NULL;
            }
            return e;
        }
        if (!expect(p, TOKEN_LPAREN) ||
            (e->as.alloc.type = type(p)) == NULL ||
            !expect(p, TOKEN_COMMA) ||
            (e->as.alloc.count = expression(p)) == NULL) {
            return NULL;
        }
        accept(p, TOKEN_COMMA);
        return expect(p, TOKEN_RPAREN) ? e : NULL;
    case TOKEN_DUP:
    case TOKEN_DELETE:
    case TOKEN_DESTROY:
        e = new_expr(p, EXPR_OBJECT, t);
        e->as.object.op = t->kind;
        next(p);
        if (!expect(p, TOKEN_LPAREN) ||
            (e->as.object.operand = expression(p)) == NULL) {
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
    case TOKEN_DISPATCH:
        /* DESIGN: `dispatch obj -> f(args)` reads as one expression, as
           `parallel` does. The object stands before the arrow and the
           worker after it, alone or called with the arguments that the
           worker receives after the object. */
        next(p);
        e = new_expr(p, EXPR_DISPATCH, t);
        if ((e->as.dispatch.object = expression(p)) == NULL ||
            !expect(p, TOKEN_ARROW) ||
            (e->as.dispatch.call = postfix(p)) == NULL) {
            return NULL;
        }
        return e;
    case TOKEN_JOIN:
    case TOKEN_JOIN_ALL:
        e = new_expr(p, EXPR_JOIN, t);
        e->as.join.all = t->kind == TOKEN_JOIN_ALL;
        next(p);
        if (!expect(p, TOKEN_LPAREN) ||
            (e->as.join.job = expression(p)) == NULL) {
            return NULL;
        }
        accept(p, TOKEN_COMMA);
        return expect(p, TOKEN_RPAREN) ? e : NULL;
    case TOKEN_PARALLEL:
        /* DESIGN: `parallel a by n -> f(x)` reads as one expression. The
           array and the chunk count are expressions, and `by` is a
           contextual word rather than a keyword. After the arrow stands
           the worker, alone or called with the arguments that every
           chunk receives after its own. */
        next(p);
        e = new_expr(p, EXPR_PARALLEL, t);
        if ((e->as.parallel.array = expression(p)) == NULL) {
            return NULL;
        }
        if (is_word(p, peek(p), "by")) {
            next(p);
            if ((e->as.parallel.chunks = expression(p)) == NULL) {
                return NULL;
            }
        }
        if (!expect(p, TOKEN_ARROW) ||
            (e->as.parallel.call = postfix(p)) == NULL) {
            return NULL;
        }
        return e;
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
            /* `super` is the base part of a class and a keyword, so no
               declared field carries that name. It is read here as the
               one field name that a keyword spells. */
            if (check(p, TOKEN_SUPER)) {
                outer->as.field.name.text = p->source + peek(p)->offset;
                outer->as.field.name.length = peek(p)->length;
                accept(p, TOKEN_SUPER);
            } else if (!expect_name(p, &outer->as.field.name)) {
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
    /* DESIGN: `try f(args)` is `f(args) catch e { return e; }`, so it
       stands where the call stands and carries the same handler. */
    case TOKEN_TRY: {
        struct expr *e;
        next(p);
        e = postfix(p);
        if (e == NULL) {
            return NULL;
        }
        if (e->kind != EXPR_CALL) {
            error_here(p, "`try` stands before a call");
            return NULL;
        }
        e->as.call.handler.kind = HANDLE_TRY;
        e->as.call.handler.pos = pos_of(t);
        return e;
    }
    default:
        return postfix(p);
    }
}

/* unary { "as" type }: as binds tighter than the binary operators. */
static struct expr *cast(struct parser *p)
{
    struct expr *e = unary(p);

    while (e != NULL && (check(p, TOKEN_AS) || check(p, TOKEN_IS))) {
        bool test = check(p, TOKEN_IS);
        struct expr *c = new_expr(p, EXPR_CAST, next(p));
        c->pos = e->pos;
        c->as.cast.operand = e;
        c->as.cast.test = test;
        /* `as?` on a class pointer gives null where `as` traps. */
        c->as.cast.checked = !test && accept(p, TOKEN_QUESTION);
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

static bool read_handler(struct parser *p, struct handler *out,
                         bool required);

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
    if (!expect(p, TOKEN_ASSIGN) || (s->as.let.value = expression(p)) == NULL) {
        return NULL;
    }
    /* `let n = f(args) catch e { ... };` handles the error of the call
       that gives n its value. `alloc T(args)` carries the handler of the
       `construct` it runs. */
    if (check(p, TOKEN_CATCH)) {
        struct expr *v = s->as.let.value;
        if (v->kind == EXPR_ALLOC && v->as.alloc.value != NULL) {
            v = v->as.alloc.value;
        }
        if (v->kind != EXPR_CALL) {
            error_here(p, "`catch` follows a call");
            return NULL;
        }
        if (!read_handler(p, &v->as.call.handler, false)) {
            return NULL;
        }
    }
    return expect(p, TOKEN_SEMICOLON) ? s : NULL;
}

/* DESIGN: the arms of a switch are separated by commas, so the body of an
   arm carries no `;` of its own. It is a block, or one assignment or call
   that the comma or the closing brace ends. Anything longer takes a
   block. */
static struct stmt *arm_body(struct parser *p)
{
    const struct token *t = peek(p);
    struct stmt *s;
    struct expr *e;

    if (check(p, TOKEN_LBRACE)) {
        s = new_stmt(p, STMT_BLOCK, t);
        return (s->as.block = block(p)) == NULL ? NULL : s;
    }
    if ((e = expression(p)) == NULL) {
        return NULL;
    }
    if (is_assign_op(peek(p)->kind)) {
        s = new_stmt(p, STMT_ASSIGN, t);
        s->as.assign.op = next(p)->kind;
        s->as.assign.target = e;
        return (s->as.assign.value = expression(p)) == NULL ? NULL : s;
    }
    if (e->kind != EXPR_CALL) {
        error_here(p, "an arm of a `switch` is a call, an assignment or a "
                      "block");
        return NULL;
    }
    s = new_stmt(p, STMT_EXPR, t);
    s->as.expr = e;
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

/* `catch e { }`, `catch { }` or `catch fatal`. With required set the
   handler must be there. */
static bool read_handler(struct parser *p, struct handler *out, bool required)
{
    memset(out, 0, sizeof *out);
    out->pos = pos_of(peek(p));
    if (!accept(p, TOKEN_CATCH)) {
        if (required) {
            error_here(p, "expected `catch`");
            return false;
        }
        return true;
    }
    if (is_word(p, peek(p), "fatal")) {
        next(p);
        out->kind = HANDLE_FATAL;
        return true;
    }
    out->kind = HANDLE_BLOCK;
    if (peek(p)->kind == TOKEN_IDENT) {
        out->pos = pos_of(peek(p));
        if (!expect_name(p, &out->name)) {
            return false;
        }
    }
    return (out->body = block(p)) != NULL;
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
    /* DESIGN: `for i in lo..hi` and `for x in slice` read one variable,
       a range or a sequence, and a block. The checker and lowering give
       them the loop that the core would have written. */
    case TOKEN_FOR: {
        /* DESIGN: the binding is optional over a range, because a loop
           that repeats a block needs no counter. A name and `in` open the
           bound form, and anything else is the range itself. */
        bool bound = peek_at(p, 1)->kind == TOKEN_IDENT &&
                     is_word(p, peek_at(p, 2), "in");
        next(p);
        s = new_stmt(p, STMT_FOR, t);
        if (bound) {
            s->as.for_loop.name_pos = pos_of(peek(p));
            if (!expect_name(p, &s->as.for_loop.name)) {
                return NULL;
            }
            /* DESIGN: `in` is a contextual word, as `packed` and `align`
               are. The decision adds four keywords and `in` is not among
               them, so a program may still name a variable `in`. */
            next(p);
        }
        p->no_struct_literal = true;
        if (accept(p, TOKEN_AMP)) {
            s->as.for_loop.by_pointer = true;
            s->as.for_loop.over = expression(p);
        } else if ((s->as.for_loop.low = expression(p)) != NULL &&
                   accept(p, TOKEN_DOT_DOT)) {
            s->as.for_loop.high = expression(p);
        } else {
            s->as.for_loop.over = s->as.for_loop.low;
            s->as.for_loop.low = NULL;
        }
        /* `by k` steps a range. It is the contextual word that `parallel`
           uses for its chunk count. */
        if (s->as.for_loop.high != NULL && is_word(p, peek(p), "by")) {
            next(p);
            s->as.for_loop.step_pos = pos_of(peek(p));
            s->as.for_loop.step = expression(p);
        }
        p->no_struct_literal = false;
        if (p->panic ||
            (s->as.for_loop.over == NULL && s->as.for_loop.high == NULL)) {
            return NULL;
        }
        if ((s->as.for_loop.body = block(p)) == NULL) {
            return NULL;
        }
        return s;
    }
    case TOKEN_DO:
        next(p);
        s = new_stmt(p, STMT_DO_WHILE, t);
        if ((s->as.loop.body = block(p)) == NULL ||
            !expect(p, TOKEN_WHILE) ||
            (s->as.loop.cond = condition(p)) == NULL) {
            return NULL;
        }
        return s;
    /* DESIGN: `assert(cond)` keeps the source of its condition, so the
       failure names what was asserted. A release build removes the whole
       statement, which is why a call in the condition warns. */
    case TOKEN_ASSERT: {
        size_t from;
        size_t to;
        next(p);
        s = new_stmt(p, STMT_ASSERT, t);
        if (!expect(p, TOKEN_LPAREN)) {
            return NULL;
        }
        from = peek(p)->offset;
        if ((s->as.assertion.cond = expression(p)) == NULL) {
            return NULL;
        }
        to = p->pos == 0 ? from : p->all[p->origin[p->pos - 1]].offset +
                                  p->all[p->origin[p->pos - 1]].length;
        s->as.assertion.text.bytes = p->source + from;
        s->as.assertion.text.length = to > from ? to - from : 0;
        if (accept(p, TOKEN_COMMA)) {
            if (!check(p, TOKEN_STRING)) {
                error_here(p, "the message of an `assert` is a string "
                              "literal");
                return NULL;
            }
            s->as.assertion.message = peek(p)->value.text;
            next(p);
        }
        return expect(p, TOKEN_RPAREN) && expect(p, TOKEN_SEMICOLON) ? s
                                                                     : NULL;
    }
    /* DESIGN: `switch e { A => stmt, else => stmt }` names one value per
       arm and runs one statement. There is no fallthrough, so no arm
       needs a break, and `else` takes the rest. */
    case TOKEN_SWITCH: {
        struct list arms = {NULL, 0, 0, sizeof(struct switch_arm)};
        next(p);
        s = new_stmt(p, STMT_SWITCH, t);
        p->no_struct_literal = true;
        s->as.switch_stmt.value = expression(p);
        p->no_struct_literal = false;
        if (s->as.switch_stmt.value == NULL || !expect(p, TOKEN_LBRACE)) {
            return NULL;
        }
        while (!check(p, TOKEN_RBRACE) && !check(p, TOKEN_EOF)) {
            struct switch_arm arm;
            memset(&arm, 0, sizeof arm);
            arm.pos = pos_of(peek(p));
            if (accept(p, TOKEN_ELSE)) {
                if (s->as.switch_stmt.otherwise != NULL) {
                    error_here(p, "a `switch` has one `else`");
                    free(arms.data);
                    return NULL;
                }
                if (!expect(p, TOKEN_FAT_ARROW) ||
                    (s->as.switch_stmt.otherwise = arm_body(p)) == NULL) {
                    free(arms.data);
                    return NULL;
                }
            } else {
                if ((arm.value = expression(p)) == NULL ||
                    !expect(p, TOKEN_FAT_ARROW) ||
                    (arm.body = arm_body(p)) == NULL) {
                    free(arms.data);
                    return NULL;
                }
                list_push(&arms, &arm);
            }
            if (!accept(p, TOKEN_COMMA)) {
                break;
            }
        }
        s->as.switch_stmt.arms = list_finish(p, &arms, &s->as.switch_stmt.count);
        return expect(p, TOKEN_RBRACE) ? s : NULL;
    }
    /* DESIGN: `defer stmt;` records a statement that runs at every exit
       of the enclosing block, in reverse order of declaration. */
    case TOKEN_DEFER:
        next(p);
        s = new_stmt(p, STMT_DEFER, t);
        if ((s->as.deferred = statement(p)) == NULL) {
            return NULL;
        }
        if (s->as.deferred->kind == STMT_DEFER) {
            error_here(p, "a `defer` holds one statement, not another "
                          "`defer`");
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
    /* `yield v;` ends a handler with the value that takes the place of
       the result the failing call would have given. */
    case TOKEN_YIELD:
        next(p);
        s = new_stmt(p, STMT_YIELD, t);
        if (!check(p, TOKEN_SEMICOLON) &&
            (s->as.yielded = expression(p)) == NULL) {
            return NULL;
        }
        return expect(p, TOKEN_SEMICOLON) ? s : NULL;
    /* `try { } catch e { }` handles every failing call of the block. */
    case TOKEN_TRY:
        if (peek_at(p, 1)->kind != TOKEN_LBRACE) {
            break;
        }
        next(p);
        s = new_stmt(p, STMT_TRY, t);
        if ((s->as.try_block.body = block(p)) == NULL ||
            !read_handler(p, &s->as.try_block.handler, true)) {
            return NULL;
        }
        return s;
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
            /* A call that can fail carries its handler here. */
            if (e->kind == EXPR_CALL && check(p, TOKEN_CATCH) &&
                !read_handler(p, &e->as.call.handler, false)) {
                return NULL;
            }
            if (!check(p, TOKEN_SEMICOLON)) {
                expect(p, TOKEN_SEMICOLON);
                return NULL;
            }
            if (e->kind != EXPR_CALL && e->kind != EXPR_FREE &&
                e->kind != EXPR_OBJECT && e->kind != EXPR_JOIN) {
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
/* DESIGN: `self` is written as the first parameter and carries no type,
   because its type is always a pointer to the struct that declares the
   function. It reaches the parameter list as has_self, not as a param. */
static struct param *params(struct parser *p, bool allow_variadic,
                            bool *variadic, bool *has_self, size_t *count)
{
    struct list list = {NULL, 0, 0, sizeof(struct param)};

    if (!expect(p, TOKEN_LPAREN)) {
        return NULL;
    }
    if (has_self != NULL && check(p, TOKEN_SELF)) {
        next(p);
        *has_self = true;
        if (!accept(p, TOKEN_COMMA)) {
            return expect(p, TOKEN_RPAREN) ? list_finish(p, &list, count)
                                           : NULL;
        }
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

/* Whether the next token opens a function or a constant of a body.
   DESIGN: `pub` and `protected` stand before a field as well as before a
   function, so the test looks past the marker. A field is a name and a
   colon, and everything else at that point opens a member. */
static bool starts_member(struct parser *p)
{
    size_t i = 0;

    switch (peek(p)->kind) {
    case TOKEN_PUB:
    case TOKEN_PROTECTED:
    case TOKEN_EXPORT:
        i = 1;
        break;
    default:
        break;
    }
    switch (peek_at(p, i)->kind) {
    case TOKEN_FN:
    case TOKEN_EXPORT:
    case TOKEN_PUB:
    case TOKEN_ABSTRACT:
    case TOKEN_CONCRETE:
    case TOKEN_CONST:
    case TOKEN_STATIC:
        return true;
    default:
        return (is_word(p, peek_at(p, i), "final") ||
                is_word(p, peek_at(p, i), "operator")) &&
               peek_at(p, i + 1)->kind == TOKEN_FN;
    }
}

/* One function or constant declared between the braces of a struct, a
   union or an enum. An abstract function has no body and ends with `;`. */
static struct item *member(struct parser *p, const struct item *owner)
{
    struct item *m = node(p, sizeof *m);

    m->pos = pos_of(peek(p));
    m->doc = doc_before(p, TOKEN_DOC);
    m->note = doc_before(p, TOKEN_NOTE);
    m->owner = owner;
    m->exported = accept(p, TOKEN_EXPORT);
    m->pub = m->exported || accept(p, TOKEN_PUB);
    m->vis = m->pub ? VIS_PUB : VIS_PRIVATE;
    /* DESIGN: `protected` reaches the class and every class below it. The
       visibility table of the object model document gives it to a class
       member alone, so `internal` here names the level that fits. */
    if (!m->pub && accept(p, TOKEN_PROTECTED)) {
        m->vis = VIS_PROTECTED;
    } else if (check(p, TOKEN_INTERNAL)) {
        error_here(p, "`internal` marks a module item, and a class member "
                      "is `pub`, `protected` or neither");
        return NULL;
    }
    /* `final` before `fn` forbids replacement, as it forbids inheritance
       before `class`. It is a contextual word in both places. */
    if (is_word(p, peek(p), "final") && peek_at(p, 1)->kind == TOKEN_FN) {
        next(p);
        m->is_final = true;
    }
    /* `operator` before `fn` marks a function an operator calls. It is a
       contextual word and implies `pub`. */
    if (is_word(p, peek(p), "operator") && peek_at(p, 1)->kind == TOKEN_FN) {
        next(p);
        m->is_operator = true;
    }
    if (accept(p, TOKEN_ABSTRACT)) {
        m->contract = FN_ABSTRACT;
    } else if (accept(p, TOKEN_CONCRETE)) {
        m->contract = FN_CONCRETE;
    }
    /* A function of a contract is public wherever its struct is, and so
       is one an operator calls. */
    if (m->contract != FN_PLAIN || m->is_operator) {
        m->pub = true;
        m->vis = VIS_PUB;
    }
    /* DESIGN: `static atomic n: T = e;` declares a field of the class,
       not of the object. It is a global with the class's symbol, and it
       must be atomic, so that `parallel` keeps its guarantee. */
    if (m->contract == FN_PLAIN && accept(p, TOKEN_STATIC)) {
        bool atomic = accept(p, TOKEN_ATOMIC);
        m->kind = ITEM_CONST;
        m->is_static = true;
        m->name_pos = pos_of(peek(p));
        if (!expect_name(p, &m->name)) {
            return NULL;
        }
        if (!atomic) {
            char message[96];
            snprintf(message, sizeof message,
                     "`%.*s` is `static` and must be `atomic`",
                     (int)m->name.length, m->name.text);
            error_here(p, message);
            return NULL;
        }
        m->atomic = true;
        if (!expect(p, TOKEN_COLON) || (m->type = type(p)) == NULL ||
            !expect(p, TOKEN_ASSIGN) || (m->value = expression(p)) == NULL ||
            !expect(p, TOKEN_SEMICOLON)) {
            return NULL;
        }
        return m;
    }
    if (m->contract == FN_PLAIN && accept(p, TOKEN_CONST)) {
        m->kind = ITEM_CONST;
        m->name_pos = pos_of(peek(p));
        if (!expect_name(p, &m->name) || !expect(p, TOKEN_COLON) ||
            (m->type = type(p)) == NULL || !expect(p, TOKEN_ASSIGN) ||
            (m->value = expression(p)) == NULL ||
            !expect(p, TOKEN_SEMICOLON)) {
            return NULL;
        }
        return m;
    }
    if (!expect(p, TOKEN_FN)) {
        return NULL;
    }
    m->kind = ITEM_FN;
    m->name_pos = pos_of(peek(p));
    if (!expect_name(p, &m->name)) {
        return NULL;
    }
    /* DESIGN: `concrete fn Serializable::f` fills the table of that base
       or interface alone. The qualifier is read as a name and a `::`, so
       the function name that follows it takes the place of the first. */
    if (check(p, TOKEN_COLON_COLON)) {
        if (m->contract != FN_CONCRETE) {
            error_here(p, "a `::` qualifier belongs to a `concrete fn`");
            return NULL;
        }
        next(p);
        m->qualifier = m->name;
        m->qualifier_pos = m->name_pos;
        m->name_pos = pos_of(peek(p));
        if (!expect_name(p, &m->name)) {
            return NULL;
        }
    }
    m->params = params(p, false, &m->variadic, &m->has_self,
                       &m->param_count);
    if (p->panic ||
        (accept(p, TOKEN_ARROW) && (m->result = type(p)) == NULL)) {
        return NULL;
    }
    if (m->contract == FN_ABSTRACT) {
        return expect(p, TOKEN_SEMICOLON) ? m : NULL;
    }
    return (m->body = block(p)) == NULL ? NULL : m;
}

/* Read the functions and constants of a body into it->members. */
static bool members_of(struct parser *p, struct item *it)
{
    struct list list = {NULL, 0, 0, sizeof(struct item *)};

    while (!check(p, TOKEN_RBRACE) && !check(p, TOKEN_EOF)) {
        struct item *m = member(p, it);
        if (m == NULL) {
            free(list.data);
            return false;
        }
        list_push(&list, &m);
    }
    it->members = list_finish(p, &list, &it->member_count);
    return true;
}

/* A struct body holds fields alone. Report what the programmer wrote and
   name the kind that takes it. */
static bool struct_field_only(struct parser *p, const struct item *it)
{
    const char *what = NULL;
    char message[96];

    switch (peek(p)->kind) {
    case TOKEN_FN:
    case TOKEN_PUB:
    case TOKEN_EXPORT:
    case TOKEN_ABSTRACT:
    case TOKEN_CONCRETE:
        what = "a function";
        break;
    case TOKEN_CONST:
        what = "a constant";
        break;
    case TOKEN_STATIC:
        what = "a static field";
        break;
    case TOKEN_USE:
        what = "a `use` field";
        break;
    case TOKEN_INHERITS:
        what = "a base";
        break;
    case TOKEN_IMPLEMENTS:
        what = "an interface";
        break;
    case TOKEN_PROTECTED:
        what = "a protected member";
        break;
    default:
        return true;
    }
    snprintf(message, sizeof message,
             "a %s holds fields alone, and %s belongs to a class",
             it->kind == ITEM_UNION ? "union" : "struct", what);
    error_here(p, message);
    return false;
}

/* `class Circle { inherits Shape, implements ser: Serializable, ... }`.
   The base and the interfaces open the body, then the fields, then the
   constants and the functions. */
static struct item *class_item(struct parser *p, struct item *it)
{
    struct list fields = {NULL, 0, 0, sizeof(struct param)};

    next(p);
    it->kind = ITEM_CLASS;
    if (!expect_name(p, &it->name)) {
        return NULL;
    }
    if (is_word(p, peek(p), "align")) {
        next(p);
        if (!expect(p, TOKEN_LPAREN) || (it->align = expression(p)) == NULL ||
            !expect(p, TOKEN_RPAREN)) {
            return NULL;
        }
    }
    if (!expect(p, TOKEN_LBRACE)) {
        return NULL;
    }
    /* Fields first, comma separated, then the constants and functions,
       each ended by its own `;` or block. */
    while (!check(p, TOKEN_RBRACE) && !check(p, TOKEN_EOF) &&
           !starts_member(p)) {
        struct param field;
        memset(&field, 0, sizeof field);
        field.doc = doc_before(p, TOKEN_DOC);
        field.note = doc_before(p, TOKEN_NOTE);
        field.pos = pos_of(peek(p));
        /* The base is one name and no field of its own. The checker
           builds the `super` field from it. */
        if (check(p, TOKEN_INHERITS)) {
            if (it->base_name.length > 0) {
                error_here(p, "a class has one base");
                free(fields.data);
                return NULL;
            }
            it->base_pos = pos_of(peek(p));
            next(p);
            if (!expect_name(p, &it->base_name)) {
                free(fields.data);
                return NULL;
            }
            if (!accept(p, TOKEN_COMMA)) {
                break;
            }
            continue;
        }
        if (accept(p, TOKEN_IMPLEMENTS)) {
            field.form = FIELD_IMPL;
        } else if (accept(p, TOKEN_USE)) {
            field.form = FIELD_USE;
        }
        if (field.form == FIELD_PLAIN) {
            if (accept(p, TOKEN_PUB)) {
                field.vis = VIS_PUB;
            } else if (accept(p, TOKEN_PROTECTED)) {
                field.vis = VIS_PROTECTED;
            }
        }
        /* `own`, `atomic` and `mutable` are contextual words before a
           field name, so a field may still carry one of those names. */
        if (is_word(p, peek(p), "own") && peek_at(p, 1)->kind == TOKEN_IDENT) {
            next(p);
            field.owned = true;
        }
        if (is_word(p, peek(p), "mutable") &&
            peek_at(p, 1)->kind == TOKEN_IDENT) {
            next(p);
            field.writable = true;
        }
        if (check(p, TOKEN_ATOMIC) && peek_at(p, 1)->kind == TOKEN_IDENT) {
            next(p);
            field.atomic = true;
        }
        if (!expect_name(p, &field.name) || !expect(p, TOKEN_COLON) ||
            (field.type = type(p)) == NULL ||
            (accept(p, TOKEN_COLON) &&
             (field.bits = expression(p)) == NULL) ||
            (accept(p, TOKEN_ASSIGN) &&
             (field.value = expression(p)) == NULL)) {
            free(fields.data);
            return NULL;
        }
        list_push(&fields, &field);
        if (!accept(p, TOKEN_COMMA)) {
            break;
        }
    }
    it->params = list_finish(p, &fields, &it->param_count);
    if (!members_of(p, it)) {
        return NULL;
    }
    return expect(p, TOKEN_RBRACE) ? it : NULL;
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
    it->vis = it->pub ? VIS_PUB : VIS_PRIVATE;
    /* DESIGN: `internal` reaches every module under the same package
       root. It is a level of a module item, and never of a class
       member. */
    if (!it->pub && accept(p, TOKEN_INTERNAL)) {
        it->vis = VIS_INTERNAL;
    } else if (check(p, TOKEN_PROTECTED)) {
        error_here(p, "`protected` marks a class member, and a module item "
                      "is `pub`, `internal` or neither");
        return NULL;
    }
    /* DESIGN: export marks items that Anti defines for C. An extern fn is
       defined in C already. */
    if (it->exported && peek(p)->kind == TOKEN_EXTERN) {
        error_here(p, "an `extern fn` cannot be exported");
        return NULL;
    }
    /* DESIGN: worker marks a function that a `parallel` may run on
       another thread. It stands before fn, after pub, and no other item
       takes it. */
    /* DESIGN: `abstract` marks a class with an open function, and the
       contextual `final` forbids inheritance. Both stand before `class`,
       after `pub`. */
    if (peek(p)->kind == TOKEN_ABSTRACT && peek_at(p, 1)->kind == TOKEN_CLASS) {
        next(p);
        it->is_abstract = true;
    } else if (is_word(p, peek(p), "final") &&
               peek_at(p, 1)->kind == TOKEN_CLASS) {
        next(p);
        it->is_final = true;
    } else if (peek(p)->kind == TOKEN_SINGLETON &&
               peek_at(p, 1)->kind == TOKEN_CLASS) {
        next(p);
        it->is_singleton = true;
    }
    /* `operator fn` at module level gives a struct its operators, since
       a struct holds no functions of its own. */
    if (is_word(p, peek(p), "operator") && peek_at(p, 1)->kind == TOKEN_FN) {
        next(p);
        it->is_operator = true;
    }
    it->worker = accept(p, TOKEN_WORKER);
    if (it->worker && peek(p)->kind != TOKEN_FN) {
        error_here(p, "`worker` stands before `fn`");
        return NULL;
    }
    /* DESIGN: packed and align are contextual words. packed is one only
       directly before struct or union, and align only between the name of
       a struct or union and its opening brace. */
    if (is_word(p, peek(p), "packed") &&
        (peek_at(p, 1)->kind == TOKEN_STRUCT ||
         peek_at(p, 1)->kind == TOKEN_UNION ||
         peek_at(p, 1)->kind == TOKEN_CLASS)) {
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
        it->params = params(p, false, &it->variadic, NULL,
                            &it->param_count);
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
        it->params = params(p, true, &it->variadic, NULL,
                            &it->param_count);
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
        /* DESIGN: a struct body holds fields and nothing else. It is
           exactly the bytes C declares, so a function, a constant, a
           default or a base belongs to a class. */
        do {
            struct param field;
            memset(&field, 0, sizeof field);
            field.doc = doc_before(p, TOKEN_DOC);
            field.note = doc_before(p, TOKEN_NOTE);
            field.pos = pos_of(peek(p));
            if (!struct_field_only(p, it)) {
                free(fields.data);
                return NULL;
            }
            if (!expect_name(p, &field.name) || !expect(p, TOKEN_COLON) ||
                (field.type = type(p)) == NULL ||
                (accept(p, TOKEN_COLON) &&
                 (field.bits = expression(p)) == NULL)) {
                free(fields.data);
                return NULL;
            }
            if (check(p, TOKEN_ASSIGN)) {
                error_here(p, "a field of a struct has no default, which "
                              "belongs to a class");
                free(fields.data);
                return NULL;
            }
            list_push(&fields, &field);
        } while (accept(p, TOKEN_COMMA) && !check(p, TOKEN_RBRACE));
        it->params = list_finish(p, &fields, &it->param_count);
        return expect(p, TOKEN_RBRACE) ? it : NULL;
    }
    case TOKEN_CLASS:
        return class_item(p, it);
    case TOKEN_ENUM: {
        struct list values = {NULL, 0, 0, sizeof(struct param)};
        next(p);
        it->kind = ITEM_ENUM;
        if (!expect_name(p, &it->name)) {
            return NULL;
        }
        /* `enum Mode: u8` names the underlying type. Without one the type
           is c_int, which the checker fills in. */
        if (accept(p, TOKEN_COLON) && (it->base = type(p)) == NULL) {
            return NULL;
        }
        if (!expect(p, TOKEN_LBRACE)) {
            return NULL;
        }
        while (!check(p, TOKEN_RBRACE) && !check(p, TOKEN_EOF) &&
               !starts_member(p)) {
            struct param value;
            memset(&value, 0, sizeof value);
            value.doc = doc_before(p, TOKEN_DOC);
            value.note = doc_before(p, TOKEN_NOTE);
            value.pos = pos_of(peek(p));
            if (!expect_name(p, &value.name) ||
                (accept(p, TOKEN_ASSIGN) &&
                 (value.value = expression(p)) == NULL)) {
                free(values.data);
                return NULL;
            }
            list_push(&values, &value);
            if (!accept(p, TOKEN_COMMA)) {
                break;
            }
        }
        it->params = list_finish(p, &values, &it->param_count);
        if (!members_of(p, it)) {
            return NULL;
        }
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
    struct parser p = {source, NULL, tokens->items, NULL, NULL, 0, arena,
                       diags, false, true, false};
    struct module *m = arena_alloc(arena, sizeof *m);
    struct list imports = {NULL, 0, 0, sizeof(struct import)};
    struct list items = {NULL, 0, 0, sizeof(struct item *)};
    struct list dropped = {NULL, 0, 0, sizeof(struct dropped_doc)};
    struct token *kept = malloc(tokens->count * sizeof *kept);
    size_t *origin = malloc(tokens->count * sizeof *origin);
    bool *taken = calloc(tokens->count, sizeof *taken);
    size_t count = 0;
    size_t i;

    if (kept == NULL || origin == NULL || taken == NULL) {
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
    p.taken = taken;
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
    for (i = 0; i < tokens->count; i++) {
        const struct token *t = &tokens->items[i];
        struct dropped_doc d;
        if (!is_doc(t->kind) || taken[i]) {
            continue;
        }
        d.pos = pos_of(t);
        d.marker.text = source + t->offset;
        d.marker.length = marker_length(d.marker.text);
        d.module_form = t->kind == TOKEN_MODULE_DOC ||
                        t->kind == TOKEN_MODULE_NOTE;
        list_push(&dropped, &d);
    }
    m->dropped = list_finish(&p, &dropped, &m->dropped_count);
    free(kept);
    free(origin);
    free(taken);
    *out = m;
    return p.ok;
}
