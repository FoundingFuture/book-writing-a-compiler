---
title: "The parser and the syntax tree"
description: "A recursive descent parser in C for Anti, with precedence climbing, a syntax tree in tagged unions, doc comments on items and panic-mode error recovery."
summary: "Recursive descent with precedence climbing. The AST data structures in C. How declarations, statements and blocks are parsed, with dotted import paths, unions, bitfields, export and the contextual words packed and align. Doc comments attached to items. Error recovery so that one mistake produces one message."
date: 2026-09-13T22:32:00+02:00
lastmod: 2026-09-15T03:50:39+02:00
draft: false
weight: 50
tags: [compilers, programming-languages]
keywords: [recursive descent, precedence climbing, abstract syntax tree, tagged union, panic mode, contextual words, doc comment attachment, struct literal ambiguity]
---

## Previously

[Chapter 4, The lexer]({{% relref "/programming/writing-a-compiler/04-lexer" %}}), reads UTF-8 source into tokens by hand. It covers keywords, identifiers, numbers with separators and character literals. Comments do not nest, and the four doc comment markers each have a line form and a block form. Every token records its position for error messages. The lexer also reads ordinary, raw and byte string literals with hash delimiters.

## Parser output

The parser reads the token list of one module and builds its syntax tree. A syntax tree holds one node for each construct of the program, and a node points to the nodes of its parts. The tree keeps what later stages need, the structure and the positions, and drops the punctuation that only fixes the structure. The parser of antic follows the grammar of [chapter 2]({{% relref "/programming/writing-a-compiler/02-the-anti-language" %}}) rule by rule and reports every syntax error with one message.

```c
/* Build the syntax tree of one module from its tokens. Nodes go into the
   memory pool. Every syntax error goes to diags, one per mistake, and
   parsing continues after it. Returns true when no error occurred. The
   module is stored in *out in either case. */
bool parse(const char *source, const struct token_list *tokens,
           struct arena *arena, struct diagnostics *diags,
           struct module **out);
```

## Syntax tree in C

Each node is a C struct with a kind and a union. The kind says which member of the union is valid. This layout is a tagged union, and it keeps every kind of expression in one type, `struct expr`. A node records the line and column of its first token for error messages. Names point into the source text, which stays in memory for the whole compilation, so the parser copies no identifier.

```c
struct expr {
    enum expr_kind kind;
    struct pos pos;
    struct token_text spelling;     /* source text of a literal */
    struct type *type;              /* set by semantic analysis */
    struct symbol *symbol;          /* EXPR_NAME, set by semantic analysis */
    /* DESIGN: a pointer to a class converts to a pointer to one of its
       interfaces by adding the offset of the sub-object. The checker
       records the field here and lowering adds the offset, so every
       place a value flows into an interface slot is covered once. */
    const struct struct_field *to_iface;
    union {
        uint64_t integer;           /* EXPR_INT */
        uint32_t character;         /* EXPR_CHAR */
        bool boolean;               /* EXPR_BOOL */
        struct token_text text;     /* EXPR_FLOAT digits, EXPR_STRING, EXPR_BYTES */
        struct name name;           /* EXPR_NAME */
        struct {
            enum token_kind op;
            struct expr *operand;
        } unary;
        struct {
            enum token_kind op;
            struct expr *left;
            struct expr *right;
        } binary;
        struct {
            struct expr *operand;
            struct type_expr *type;
            bool checked;           /* `as?`, which gives null on a mismatch */
            bool test;              /* `is`, which gives a bool */
            bool from_sub;          /* the source may be a sub-object */
            const struct type *target;  /* the class of `is` and `as` */
        } cast;
        struct {
            struct expr *callee;
            struct expr **args;
            size_t arg_count;
            /* The class whose table holds the entry, when the call goes
               through one, and the name of that entry. dispatch is NULL
               for a direct call. */
            const struct type *dispatch;
            struct name entry;
            /* DESIGN: a call that can fail carries its handler. The
               checker refuses one that has none, so no program drops an
               error by writing nothing. */
            struct handler handler;
            struct expr *out;       /* the place the result is written to */
            /* `T(args)` and `alloc T(args)` build a value and run its
               `construct` with the arguments. */
            const struct type *builds;
            bool on_heap;
        } call;
        struct {
            struct expr *base;
            struct expr *index;
        } index;
        struct {
            struct expr *base;
            struct expr *low;
            struct expr *high;
        } slice;
        struct {
            struct expr *base;
            struct name name;
            uint32_t enum_value;    /* the index of an enum value, plus 1 */
            bool promoted;          /* the checker wrote it, not the program */
        } field;
        struct {
            struct name module;     /* empty when unqualified */
            struct name name;
            struct field_init *fields;
            size_t field_count;
        } struct_lit;
        struct {
            struct type_expr *element;
            struct field_init *fields;
            size_t field_count;
        } slice_lit;
        struct {
            struct expr **elements;
            size_t count;
        } array_lit;
        struct {
            struct expr *value;
            struct expr *count;
        } array_repeat;
        struct {
            struct type_expr *type;
            struct expr *count;     /* NULL in the literal form */
            struct expr *value;     /* `alloc T { ... }`, or NULL */
        } alloc;
        struct expr *free_pointer;  /* EXPR_FREE */
        struct {
            enum token_kind op;     /* dup, delete or destroy */
            struct expr *operand;
        } object;
        struct {
            struct expr *job;       /* EXPR_JOIN: the job or the slice */
            bool all;               /* join_all */
        } join;
        struct {
            enum atomic_op op;
            struct expr *place;     /* the atomic field */
            struct expr *a;         /* the value, or the expected one */
            struct expr *b;         /* compare_swap: the new value */
        } atomic;
        struct type_expr *size_of;  /* EXPR_SIZE_OF */
        struct {
            struct expr *array;     /* the array to split */
            struct expr *chunks;    /* the count after `by`, or NULL */
            struct expr *call;      /* the worker, called or named */
        } parallel;
        struct {
            struct expr *object;    /* the object to submit */
            struct expr *call;      /* the worker, called or named */
        } dispatch;
    } as;
};
```

The union continues with one member for each other kind of expression. The fields `type` and `symbol` of an expression stay empty until semantic analysis fills them in chapter 6.

Statements and items follow the same pattern. A block holds an array of statement pointers, and a module holds its imports and its items. An item is a function, an external function, a struct, a union, an enum, a class or a constant.

```c
struct item {
    enum item_kind kind;
    struct pos pos;
    bool pub;
    bool exported;                  /* export, which implies pub */
    struct name name;
    struct pos name_pos;
    struct param *params;           /* parameters, or fields of a struct or union */
    size_t param_count;
    bool variadic;                  /* ITEM_EXTERN_FN */
    bool worker;                    /* ITEM_FN, may run on a worker */
    struct type_expr *result;       /* NULL without a result */
    struct block *body;             /* ITEM_FN */
    struct type_expr *type;         /* ITEM_CONST */
    struct expr *value;             /* ITEM_CONST */
    bool packed;                    /* ITEM_STRUCT, ITEM_UNION */
    struct expr *align;             /* ITEM_STRUCT, ITEM_UNION, or NULL */
    struct symbol *symbol;
    struct doc_text doc;            /* the /// text */
    struct doc_text note;           /* the //# text */
    struct item **members;          /* the functions and constants of a body */
    size_t member_count;
    enum fn_contract contract;      /* ITEM_FN */
    bool has_self;                  /* ITEM_FN: self is its first parameter */
    struct symbol *self;            /* ITEM_FN: the symbol of self */
    const char *runtime;            /* ITEM_FN of the root: its C symbol */
    const struct item *owner;       /* the class or enum that declares it */
    struct type_expr *base;         /* ITEM_ENUM: the underlying type, or NULL */
    struct name base_name;          /* ITEM_CLASS: the base after `inherits` */
    struct pos base_pos;
    bool is_abstract;               /* ITEM_CLASS, or ITEM_FN in a body */
    bool is_final;                  /* ITEM_CLASS, ITEM_FN */
    bool is_static;                 /* a static atomic field of a class */
    bool atomic;                    /* ITEM_CONST with is_static */
    bool is_singleton;              /* ITEM_CLASS with one instance */
    bool singleton_get;             /* the generated `get` of a singleton */
    bool is_operator;               /* ITEM_FN that an operator calls */
    enum visibility vis;
    struct name qualifier;          /* `concrete fn X::f`, the X */
    struct pos qualifier_pos;
};
```

The flags `pub` and `exported` record the visibility of an item, and semantic analysis sets `symbol`. The fields `packed` and `align` belong to a struct, a union or a class. The fields `doc` and `note` hold the text of the `///` and `//#` comments before the item. A parameter and a field share `struct param`. A field adds its own doc comments and, for a bitfield, the width expression `bits`.

```c
/* The text of the doc comments before a node, empty without one. The
   line form and the block form of a marker give the same text. */
struct doc_text {
    const char *text;
    size_t length;
};

/* How a field joins its class. A plain field is its own name. A `use`
   field promotes the names of its type onto the class that holds it, and
   an `implements` field holds the sub-object of an interface. The checker
   adds two more that no declaration writes. One is the base of a class,
   named by `inherits` and carried as the field `super`. The other is the
   table pointer of the root `anti.rt.Object`. */
enum field_form { FIELD_PLAIN, FIELD_USE, FIELD_BASE, FIELD_TABLE,
                  FIELD_IMPL };

/* DESIGN: the four levels of the object model document. A module item is
   private, internal or public. A class member is private, protected or
   public. The two ends of the range are shared, and the middle level
   differs, so one enumeration serves both. */
enum visibility { VIS_PRIVATE, VIS_PROTECTED, VIS_INTERNAL, VIS_PUB };

/* A parameter, or a field of a struct declaration. */
struct param {
    struct name name;
    struct pos pos;
    struct type_expr *type;
    struct symbol *symbol;
    struct doc_text doc;            /* fields only */
    struct doc_text note;           /* fields only */
    struct expr *bits;              /* the width of a bitfield, or NULL */
    struct expr *value;             /* a field default or an enum value */
    enum field_form form;           /* fields only */
    enum visibility vis;            /* fields only */
    bool owned;                     /* `own`: the object frees the memory */
    bool atomic;                    /* `atomic`: read and written by calls */
    bool writable;                  /* `mutable`: a singleton field to write */
};

enum item_kind {
    ITEM_FN,
    ITEM_EXTERN_FN,
    ITEM_STRUCT,
    ITEM_UNION,
    ITEM_CONST,
    ITEM_ENUM,
    ITEM_CLASS
};
```

An import holds its module path as one name that includes the dots, and an optional local name after `as`. A module holds the text of its `//!` and `//#!` comments, and the doc comments that nothing took.

```c
struct import {
    struct pos pos;
    struct pos module_pos;
    struct name module;
    struct name alias;              /* empty without as */
};

/* A doc comment that no item, field or module took. Its text is lost, so
   `--doc-warnings` reports it. */
struct dropped_doc {
    struct pos pos;
    struct name marker;             /* the marker, as the reader wrote it */
    bool module_form;               /* `//!` or `//#!`, which a module takes */
};

struct module {
    const char *file;               /* the source path, for a message */
    struct doc_text doc;            /* the `//!` text */
    struct doc_text note;           /* the `//#!` text */
    struct import *imports;
    size_t import_count;
    struct item **items;
    size_t item_count;
    struct dropped_doc *dropped;
    size_t dropped_count;
};
```

Every node comes from the memory pool of chapter 4. The compiler frees the whole tree at once when the compilation ends. While the parser reads a list, such as the arguments of a call, it collects the elements in a growable array. It copies them into the pool when the list is complete.

## Recursive descent

Each grammar rule of chapter 2 becomes one function of the parser. The function for a rule calls the functions for the rules inside it, and that recursion gives the method its name. The call stack of the parser therefore follows the nesting of the source. The rule `while_stmt = "while" cond "do" block` becomes a few lines that read `while`, a condition, `do` and a block.

```c
    case TOKEN_WHILE:
        next(p);
        s = new_stmt(p, STMT_WHILE, t);
        if ((s->as.loop.cond = condition(p)) == NULL || !expect(p, TOKEN_DO) ||
            (s->as.loop.body = block(p)) == NULL) {
            return NULL;
        }
        return s;
```

The parser keeps a position in the token list and a few helpers around it. The helper `peek` returns the current token, and `accept` consumes it when it has the given kind. The helper `expect` consumes it or reports that it is missing.

```c
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
```

## Expressions

### Precedence climbing

The grammar of chapter 2 spells out one rule per precedence level, from `or_expr` down to `multiplicative`. Ten functions that each call the next would parse it. Precedence climbing replaces them with one function and a table of levels. Theodore Norvell gave the method its name, and he traces it to Martin Richards' compilers for CPL and BCPL[^1].

```c
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
```

The function `binary` reads one operand and then every operator whose level is at least `min`. For the right operand it calls itself with the level of the operator plus one. A following operator of the same level therefore ends that right operand and joins the loop instead, which groups each level from left to right.

```c
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
```

For `x = a + b * c - d;` the call with `min` 1 reads `a` and `+`. The right operand is parsed with `min` 10, so it takes `b * c` but stops at `-`. The loop then applies `-` to the result. The option `--dump-ast` prints the tree, with each node named after its grammar rule.

```text
function f
  block
    simple_stmt =
      ident x
      additive -
        additive +
          ident a
          multiplicative *
            ident b
            ident c
        ident d
```

### Casts and unary operators

The keyword `as` binds tighter than `*` and looser than a leading `-`. The function `cast` reads a unary expression and then any number of `as` with a type. The function `unary` reads `-`, `!`, `~`, `*` or `&` and calls itself for the operand, so `-x as u8` becomes a cast of `-x`.

```c
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
```

The operand of a unary operator is a postfix expression: a primary expression followed by calls, index or slice brackets and field names. The function `postfix` builds these from left to right, so `v.scale(2.0)[i].x` is a field of an index of a call of a field.

### Struct literals in conditions

An identifier followed by `{` starts a struct literal, as in `Vec2 { x: 1.0, y: 2.0 }`. After the condition of an `if` or a `while`, the `{` starts the body instead. Chapter 2 resolves this conflict: inside a condition a struct literal appears only within parentheses. The parser sets a flag while it reads a condition, and the rule for primary expressions checks it. Parentheses, brackets and call arguments clear the flag again.

```c
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
```

With the flag set, `if v == Vec2 { x: 1 } { }` reads the condition `v == Vec2` and a body that starts with `x`. The section on error recovery shows the error that follows.

## Statements and blocks

The function `statement` looks at the first token. A keyword selects `let`, `const`, `if`, `while`, `do`, `for`, `switch`, `assert`, `defer`, `yield`, `break`, `continue` or `return`. A `try` before `{` selects the block form of a handler, and a bare `{` a nested block. The remaining tokens start an expression. When an assignment operator follows the expression, the statement is an assignment. Without an assignment operator the expression must be a call, as chapter 2 specifies for expression statements.

A block reads statements until its closing brace. When a statement fails, the block skips to the start of the next one and continues.

```c
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
```

### Arms of a switch

The arms of a `switch` are separated by commas, so the body of an arm carries no `;` of its own. It is a block, or one assignment or call that the comma or the closing brace ends.

```c
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
```

Anything longer than one statement takes a block, which keeps the grammar of an arm short and the comma unambiguous.

### The for statement

A `for` reads its variable, the word `in`, a range or a sequence, and a block. It carries them into `STMT_FOR` rather than into the `while` it stands for, because `continue` must reach the step.

The word `in` is not a keyword. `for` is, and so are `assert`, `switch` and `defer`, but a program may still name a variable `in`. The parser reads it as a contextual word, the way it reads `packed` and `align`.

```c
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
```

The struct literal guard of the condition applies to the sequence too. Without it `for x in items { }` would read `items { }` as a literal.

## Items

Items stand at the top level of a module, after its imports. The function `parse` reads the imports and then items until the end of the file. The function `item` reads the words that may precede an item and then selects the item by its keyword.

```c
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
```

The calls of `doc_before` collect doc comments, which the section on doc comment attachment explains. A type in a parameter or a field starts with a type keyword, a name, `*`, `[` or `fn`. The type keywords of chapter 4, from `bool` to `c_wchar`, form one contiguous range of token kinds.

```c
static bool is_builtin_type(enum token_kind kind)
{
    return kind >= TOKEN_BOOL_TYPE && kind <= TOKEN_C_WCHAR;
}
```

### Export and visibility

The keyword `export` marks an item for use from C, as chapter 2 specifies, and it implies `pub`. The function `item` therefore sets `pub` for an exported item. It accepts `export` or `pub`, and never both. The sources `export pub fn` and `pub export fn` report `expected an item` at the second word. An `extern fn` is defined in C, so `export extern fn` reports `` an `extern fn` cannot be exported `` at `extern`. Semantic analysis checks which signatures `export` allows.

```anti
export fn dot(a: int) -> int
{
    return a;
}

export union U
{
    a: i32,
}

export const K: int = 3;
```

```text
export function dot
  param a
    type int
  result
    type int
  block
    return_stmt
      ident a
export union_decl U
  field a
    type i32
export const_decl K
  type int
  int_lit 3
```

### Structs, unions and bitfields

A struct and a union share one branch of `item` and differ only in the item kind. Both read at least one field, and a comma may follow the last field. A field is a name, a colon and a type. A second colon and an expression make the field a bitfield of that width, which the parser stores in `bits`.

```anti
struct Flags
{
    visible: u32 : 1,
    layer: u32 : 2 + 2,
}
```

```text
struct_decl Flags
  field visible
    type u32
    bits
      int_lit 1
  field layer
    type u32
    bits
      additive +
        int_lit 2
        int_lit 2
```

The parser accepts any expression as a width. Semantic analysis requires a sized integer type and a constant width from 1 to the bits of that type.

### Classes and enums

A class reads its header, then its fields, then the members of its body. The header takes `abstract`, `final` or `singleton` before `class`, the name, and an optional `align`. The body opens with an optional `inherits` and a base name, then any number of `implements` lines, each a field name and an interface. The fields follow, comma separated, and each may carry `pub`, `protected`, `use`, `own`, `atomic`, `mutable`, a bitfield width and a default. The members come last, each ended by its own `;` or block.

```anti
abstract class Shape
{
	kind: Kind,
	x: f32 = 0.0,
	own label: []byte,

	const MAX: f32 = 1000.0;
	static atomic count: int = 0;

	abstract fn area(self) -> f32;

	pub fn move(self, dx: f32)
	{
		self.x = self.x + dx;
	}
}
```

The function `starts_member` decides where the fields end. It answers true for `fn`, `pub`, `protected`, `export`, `abstract`, `concrete`, `const`, `static` and for `final` or `operator` before `fn`. A `pub` or `protected` before a name and a colon is a field, so the test looks past the marker. The parser reads fields until it answers true or the brace closes.

A struct body holds fields alone. The function `struct_field_only` reports what the programmer wrote and names the kind that takes it. A `fn` in a struct gives `` a struct holds fields alone, and a function belongs to a class ``.

An enum reads its name, an optional `: type` for the underlying integer, and its values. A value is a name and an optional `= expr`. Members follow the values, as in a class.

The parser stores a member function with the name it was written with. Semantic analysis gives its symbol the name `T.f`, which chapter 6 describes.

### Objects in expressions

`self` reads as a parameter of a member function. The function `params` takes it before the declared parameters and records it as `has_self`. `self.super` reads as a field access. No declared field carries that name, because `super` is a keyword. The postfix parser therefore accepts `super` as the one field name that a keyword spells.

`alloc T { ... }` puts one object on the heap. The parser reads `alloc` and, when no `(` follows, reads one expression. Semantic analysis requires a literal there, or a call of a class, which is `alloc Circle(2.0)` and runs the `construct` of the class with its arguments. The same call without `alloc` builds the object as a value. `alloc(T, n)` keeps the raw form.

`dup(p)`, `delete(p)` and `destroy(p)` read like `free(p)` and become one node with the operation on it. `delete` and `destroy` also stand as statements, because they return nothing.

`is` reads at the same level as `as` and takes a type on its right, so `p is *Circle` groups like a cast. `as?` is `as` followed by the new token `?`.

### Replacements and operators

`concrete fn` may name the table it fills. The parser reads the name after `fn`. A `::` after that name turns it into the qualifier, and the function name follows. `concrete fn Serializable::serialize` therefore lands in the item as the name `serialize` with the qualifier `Serializable`. A `::` after anything but a `concrete fn` is refused.

`operator fn add` is the contextual word `operator` before `fn`. The parser records it in `is_operator` and marks the function public, since an operator reaches every caller of the type.

### Handling an error

A call that can fail carries its handler in the call node. The parser reads a `catch` after a call, in a statement and after the value of a `let`, and `alloc T(args) catch ...` hands the handler to the call inside the `alloc`.

```anti
let n = parse_int(text) catch e { yield 0; };
open(path) catch fatal;
try { read(a); read(b); } catch e { report(e); }
```

A `catch` takes an optional name and then a block. The contextual word `fatal` stands in place of both. A `try` before a call marks the same handler as the `try` kind, and a `try` before `{` reads a block and its one handler. The `yield` statement stands on its own, with an optional value.

`switch` separates the value of an arm from its body with `=>`, the token the lexer of chapter 4 adds.

### Contextual words

A contextual word has a meaning in one position of the grammar and is an identifier everywhere else. Anti has seven: `packed`, `align`, `final`, `own`, `operator`, `mutable` and `by`. The lexer produces every one of them as `TOKEN_IDENT`. The function `is_word` tests whether a token is the identifier with a given spelling.

```c
/* Whether token t is the identifier word. */
static bool is_word(const struct parser *p, const struct token *t,
                    const char *word)
{
    return t->kind == TOKEN_IDENT && t->length == strlen(word) &&
           memcmp(p->source + t->offset, word, t->length) == 0;
}
```

The word `packed` counts only directly before `struct`, `union` or `class`, which the parser sees in the token after it. The word `align` counts only after the name of a struct, union or class, where the grammar allows no identifier. It takes an expression in parentheses, and semantic analysis requires a constant power of two. Neither test looks further than the next token. In the source below, `align` also names a field and a function, and `packed` names a field and a parameter.

```anti
packed struct P
{
    a: u8,
    align: i32,
}

union U align(16)
{
    packed: u8,
}

fn align(packed: int) -> int
{
    return packed;
}
```

```text
struct_decl P
  packed
  field a
    type u8
  field align
    type i32
union_decl U
  align
    int_lit 16
  field packed
    type u8
function align
  param packed
    type int
  result
    type int
  block
    return_stmt
      ident packed
```

Before any other token, `packed` is an identifier, and no item starts with an identifier. The source `packed fn g() {}` therefore reports `expected an item` at 1:1.

## Import paths

An import names a module by its module path, identifiers joined by dots. The function `module_path` reads the segments and builds the path as one name in the memory pool. The source may hold spaces between the tokens of a path, as in `com . niese`, and the stored name has none.

```c
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
```

The loop at the start of `parse` reads an import while the current token is `import`. It reads the path, an optional `as` with a local name and the semicolon.

```anti
import com.niese.geo as g;
import anti.text;
```

```text
import com.niese.geo as g
import anti.text
```

Each segment must be an identifier, so a keyword in a path is a syntax error. Semantic analysis refuses a path with an uppercase letter.

## Doc comment attachment

The lexer of chapter 4 produces doc comments as tokens, and the grammar rules never see them. The function `parse` copies every other token into a second array, `kept`. For each copy, the array `origin` records its index in the full token list. The parser holds both arrays.

```c
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
```

The function `doc_before` reads the full list between the previous kept token and the current one. It collects the doc comments of one kind there. Two comments of one kind join with a blank line, which separates two paragraphs of doc text, so no text is lost.

```c
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
```

Three places ask for doc comments. The function `parse` takes the `//!` and `//#!` text before the first import or item. The function `item` takes the `///` and `//#` text before the first word of an item, and its field loop takes it before each field name. A doc comment in any other place, such as inside a function body, documents nothing. The array `taken` records each comment that `doc_before` read, and the loop at the end of `parse` puts every other one into `m->dropped`, with the marker that the reader wrote. Chapter 6 turns those into warnings.

```c
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
```

The dump prints doc text in quotes on one line, with `\n` for a line feed. Comments before `pub` or `export` belong to the item, because `item` asks before it reads those words.

```anti
//! Guide.
//#! Notes.
import geometry;

/// Adds.
//# Fast "path".
pub fn f()
{
}

/// A point.
///
///     Two fields.
struct P
{
    /// Across.
    x: int,
}

/// First.

/// Second.
const K: int = 1;
```

```text
module_doc "Guide."
module_note "Notes."
import geometry
pub function f
  doc "Adds."
  note "Fast \"path\"."
  block
struct_decl P
  doc "A point.\n\n    Two fields."
  field x
    doc "Across."
    type int
const_decl K
  doc "First.\n\nSecond."
  type int
  int_lit 1
```

## Error recovery

After a mistake, every following token can look wrong, and reporting each one would repeat the same mistake. The parser of antic therefore uses panic mode. After the first message it reports nothing until it reaches a point where the input makes sense again.

```c
static void error_here(struct parser *p, const char *message)
{
    p->ok = false;
    if (p->panic) {
        return;
    }
    p->panic = true;
    diagnostics_add(p->diags, peek(p)->line, peek(p)->column, "%s", message);
}
```

Two synchronisation points end panic mode. Inside a block, the parser skips to the token after a semicolon, or up to a closing brace or a keyword that starts a statement.

```c
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
```

At the top level the parser skips to the next keyword that starts an item or an import, outside any braces. The words `packed` and `align` are identifiers and end no skip.

```c
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
```

An import holds no braces but may hold a keyword. In `import anti.fn.x;` the path stops at `fn` with `expected identifier`, and `sync_item` would stop at the same `fn` and read an item from it. After an error in an import the parser therefore skips to the end of the import's line, past its semicolon. A missing semicolon at the end of the line reports `` expected `;` `` at the next line, and the next item parses.

```c
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
```

The test file `tests/errors/syntax.anti` holds two mistakes, a missing expression and a missing semicolon.

```anti
fn main() -> int
{
    let a = ;
    let b = 1
    return 0;
}
```

antic reports each mistake once. After the first error the parser skips past the semicolon of line 3. After the second it stops at `return`, which starts a statement. A tab indents each statement and counts as one column.

```text
tests/errors/syntax.anti:3:10: error: expected an expression
tests/errors/syntax.anti:5:2: error: expected `;`
```

The condition `if v == Vec2 { x: 1 } { }` from the section on struct literals produces one message as well. The body `{ x: 1 }` contains `x`, which is not followed by a semicolon. The parser reports the missing semicolon at the colon and skips to the closing brace. The block `{ }` then parses as a nested block.

## Tree dump

The option `--dump-ast` prints the syntax tree, one node per line, indented by two spaces per level. For the function `scale` of chapter 1 it prints the tree that chapter 1 shows. The test `dump_ast_scale` compares the output with `tests/dump/scale.ast` byte for byte.

```sh
build/antic --dump-ast tests/dump/scale.anti
```

```text
function scale
  param x
    type int
  result
    type int
  block
    let_stmt k
      additive +
        int_lit 2
        int_lit 4
    return_stmt
      multiplicative *
        ident x
        ident k
```

An item line starts with `pub`, `protected` or `export` when the item has one of them. The lines `doc`, `note`, `packed`, `align`, `bits`, `use`, `implements`, `mutable`, `operator` and `default` follow the line of their item or field, as the dumps of this chapter show.

## Direct path on the tree

Since this chapter, the route of chapter 3 from source to assembly reads the syntax tree. Its `direct.c` lives in `direct-path/` of this chapter's folder, and the program `direct_path_05` builds it with the driver of chapter 3 and the parser of `src/`. The parser reports every syntax error. The route therefore only checks the program shape: one function `main` with no parameters, the result type `int` and one `return` of an integer literal.

```c
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
```

## Tests

Every tree and every diagnostic of this chapter is pinned. A source text is compared with its dump, and a faulty source with the positions and messages it must report. The build has zero warnings, and every `ctest` test passes.

## Next

[Chapter 6, Semantic analysis]({{% relref "/programming/writing-a-compiler/06-semantic-analysis" %}}), builds scopes and symbol tables, name resolution and the type checker without implicit conversions. It enforces mandatory initialisation and applies the method-call rewrite. Its constants computed from `size_of` stay symbolic. It checks unions, bitfields and export signatures and computes the pointer-free property of types that the threading chapter uses.

## References

[^1]: T. S. Norvell, *Parsing Expressions by Recursive Descent*, sections "Precedence climbing" and "Bibliographic Notes", https://www.engr.mun.ca/~theo/Misc/exp_parsing.htm
