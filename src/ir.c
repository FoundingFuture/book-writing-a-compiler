#include "ir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Growable arrays of the module are plain heap arrays, released by
   ir_module_free. Names and global bytes go into the memory pool. */

static void *grow(void *items, size_t *capacity, size_t count, size_t size)
{
    size_t next;
    void *resized;

    if (count < *capacity) {
        return items;
    }
    next = *capacity == 0 ? 8 : *capacity * 2;
    resized = realloc(items, next * size);
    if (resized == NULL) {
        fputs("antic: out of memory\n", stderr);
        exit(70);
    }
    *capacity = next;
    return resized;
}

static const char *keep(struct arena *arena, const char *s)
{
    size_t n;
    char *copy;

    if (s == NULL) {
        return NULL;
    }
    n = strlen(s);
    copy = arena_alloc(arena, n + 1);
    memcpy(copy, s, n + 1);
    return copy;
}

void ir_module_init(struct ir_module *m, struct arena *arena,
                    const char *name)
{
    memset(m, 0, sizeof *m);
    m->arena = arena;
    m->name = keep(arena, name);
}

void ir_block_free(struct ir_block *b)
{
    size_t i;

    for (i = 0; i < b->count; i++) {
        free(b->insts[i].args);
    }
    free(b->insts);
    free(b);
}

void ir_function_free(struct ir_function *f)
{
    size_t i;

    for (i = 0; i < f->block_count; i++) {
        ir_block_free(f->blocks[i]);
    }
    free(f->blocks);
    free(f->params);
    free(f->temps);
    f->blocks = NULL;
    f->block_count = 0;
    f->params = NULL;
    f->param_count = 0;
    f->temps = NULL;
    f->temp_count = 0;
}

void ir_module_free(struct ir_module *m)
{
    size_t i;

    for (i = 0; i < m->function_count; i++) {
        ir_function_free(m->functions[i]);
    }
    free(m->functions);
    free(m->globals);
    free(m->aggs);
    free(m->syms);
    memset(m, 0, sizeof *m);
}

struct ir_vtype ir_scalar(enum ir_type type)
{
    struct ir_vtype v;

    v.type = type;
    v.agg = IR_NO_AGG;
    return v;
}

struct ir_vtype ir_aggregate(uint32_t agg)
{
    struct ir_vtype v;

    v.type = IR_AGG;
    v.agg = agg;
    return v;
}

uint32_t ir_agg_find(const struct ir_module *m, const char *name)
{
    size_t i;

    for (i = 0; i < m->agg_count; i++) {
        if (strcmp(m->aggs[i]->name, name) == 0) {
            return (uint32_t)i;
        }
    }
    return IR_NO_AGG;
}

/* DESIGN: an aggregate is identified by its name. A struct name carries
   its module, and an array name its length expression, so two entries
   with one name describe the same type. */
static uint32_t add_agg(struct ir_module *m, const struct ir_aggtype *key,
                        const struct ir_field *fields, size_t count)
{
    uint32_t found = ir_agg_find(m, key->name);
    struct ir_aggtype *t;
    size_t i;

    if (found != IR_NO_AGG) {
        return found;
    }
    t = arena_alloc(m->arena, sizeof *t);
    *t = *key;
    t->name = keep(m->arena, key->name);
    t->length_text = keep(m->arena, key->length_text);
    t->fields = arena_alloc(m->arena, (count + 1) * sizeof *t->fields);
    for (i = 0; i < count; i++) {
        t->fields[i] = fields[i];
        t->fields[i].name = keep(m->arena, fields[i].name);
    }
    t->field_count = count;
    m->aggs = grow(m->aggs, &m->agg_capacity, m->agg_count, sizeof *m->aggs);
    m->aggs[m->agg_count] = t;
    return (uint32_t)m->agg_count++;
}

uint32_t ir_struct_add(struct ir_module *m, enum ir_agg_kind kind,
                       const char *name, const struct ir_field *fields,
                       size_t count, bool packed, uint64_t align)
{
    struct ir_aggtype key;

    memset(&key, 0, sizeof key);
    key.kind = kind;
    key.name = name;
    key.packed = packed;
    key.align = align;
    return add_agg(m, &key, fields, count);
}

uint32_t ir_array_add(struct ir_module *m, const char *name,
                      struct ir_vtype element, uint32_t length,
                      const char *length_text)
{
    struct ir_aggtype key;
    struct ir_field field;

    memset(&key, 0, sizeof key);
    memset(&field, 0, sizeof field);
    key.kind = IR_AGG_ARRAY;
    key.name = name;
    key.length = length;
    key.length_text = length_text;
    field.type = element;
    return add_agg(m, &key, &field, 1);
}

static uint32_t add_sym(struct ir_module *m, const struct ir_sym *key)
{
    size_t i;

    for (i = 0; i < m->sym_count; i++) {
        const struct ir_sym *s = &m->syms[i];
        if (s->kind == key->kind && s->type == key->type &&
            s->value == key->value && s->of.type == key->of.type &&
            s->of.agg == key->of.agg && s->field == key->field &&
            s->op == key->op && s->a == key->a && s->b == key->b) {
            return (uint32_t)i;
        }
    }
    m->syms = grow(m->syms, &m->sym_capacity, m->sym_count, sizeof *m->syms);
    m->syms[m->sym_count] = *key;
    return (uint32_t)m->sym_count++;
}

static struct ir_sym sym_key(enum ir_sym_kind kind, enum ir_type type)
{
    struct ir_sym key;

    memset(&key, 0, sizeof key);
    key.kind = kind;
    key.type = type;
    key.of = ir_scalar(IR_VOID);
    key.a = IR_NO_AGG;
    key.b = IR_NO_AGG;
    return key;
}

uint32_t ir_sym_int(struct ir_module *m, enum ir_type type, uint64_t value)
{
    struct ir_sym key = sym_key(IR_SYM_INT, type);

    key.value = ir_int_op(type, value).as.integer;
    return add_sym(m, &key);
}

uint32_t ir_sym_size_of(struct ir_module *m, struct ir_vtype of)
{
    struct ir_sym key = sym_key(IR_SYM_SIZE_OF, IR_I64);

    key.of = of;
    return add_sym(m, &key);
}

uint32_t ir_sym_offset_of(struct ir_module *m, uint32_t agg, uint32_t field)
{
    struct ir_sym key = sym_key(IR_SYM_OFFSET_OF, IR_I64);

    key.of = ir_aggregate(agg);
    key.field = field;
    return add_sym(m, &key);
}

uint32_t ir_sym_op(struct ir_module *m, enum ir_op op, enum ir_type type,
                   uint32_t a, uint32_t b)
{
    struct ir_sym key = sym_key(IR_SYM_OP, type);

    key.op = (int)op;
    key.a = a;
    key.b = b;
    return add_sym(m, &key);
}

struct ir_operand ir_sym_operand(const struct ir_module *m, uint32_t sym)
{
    struct ir_operand o = {IR_SYM, IR_VOID, {0}};
    const struct ir_sym *s = &m->syms[sym];

    if (s->kind == IR_SYM_INT) {
        return ir_int_op(s->type, s->value);
    }
    o.type = s->type;
    o.as.index = sym;
    return o;
}

static struct ir_function *new_function(struct ir_module *m,
                                        const char *module, const char *name,
                                        enum ir_type result)
{
    struct ir_function *f = arena_alloc(m->arena, sizeof *f);

    m->functions = grow(m->functions, &m->function_capacity,
                        m->function_count, sizeof *m->functions);
    f->index = (uint32_t)m->function_count;
    f->module = keep(m->arena, module);
    f->name = keep(m->arena, name);
    f->result = result;
    f->result_agg = IR_NO_AGG;
    m->functions[m->function_count++] = f;
    return f;
}

struct ir_function *ir_function_add(struct ir_module *m, const char *module,
                                    const char *name, enum ir_type result,
                                    uint32_t result_agg)
{
    struct ir_function *f = new_function(m, module, name, result);
    f->result_agg = result_agg;
    return f;
}

struct ir_function *ir_extern_add(struct ir_module *m, const char *name,
                                  enum ir_type result, bool variadic)
{
    struct ir_function *f = new_function(m, NULL, name, result);
    f->is_extern = true;
    f->variadic = variadic;
    return f;
}

struct ir_function *ir_declare_add(struct ir_module *m, const char *module,
                                   const char *name, enum ir_type result,
                                   uint32_t result_agg)
{
    struct ir_function *f = new_function(m, module, name, result);
    f->result_agg = result_agg;
    f->is_extern = true;
    return f;
}

uint32_t ir_temp(struct ir_function *f, enum ir_type type)
{
    size_t capacity = f->temp_capacity;

    f->temps = grow(f->temps, &capacity, f->temp_count, sizeof *f->temps);
    f->temp_capacity = (uint32_t)capacity;
    f->temps[f->temp_count] = type;
    return f->temp_count++;
}

uint32_t ir_param_add(struct ir_function *f, enum ir_type type, uint32_t agg)
{
    struct ir_param *p;

    f->params = grow(f->params, &f->param_capacity, f->param_count,
                     sizeof *f->params);
    p = &f->params[f->param_count++];
    p->type = type;
    p->ext = IR_EXT_NONE;
    p->agg = agg;
    p->temp = f->is_extern ? IR_NO_RESULT
                           : ir_temp(f, type == IR_AGG ? IR_PTR : type);
    return p->temp;
}

struct ir_block *ir_block_add(struct ir_function *f)
{
    struct ir_block *b = calloc(1, sizeof *b);

    if (b == NULL) {
        fputs("antic: out of memory\n", stderr);
        exit(70);
    }
    f->blocks = grow(f->blocks, &f->block_capacity, f->block_count,
                     sizeof *f->blocks);
    b->index = (uint32_t)f->block_count;
    f->blocks[f->block_count++] = b;
    return b;
}

struct ir_global *ir_global_add(struct ir_module *m, const char *module,
                                const char *name, const uint8_t *bytes,
                                uint64_t size, uint64_t align)
{
    struct ir_global *g = arena_alloc(m->arena, sizeof *g);

    m->globals = grow(m->globals, &m->global_capacity, m->global_count,
                      sizeof *m->globals);
    g->index = (uint32_t)m->global_count;
    g->module = keep(m->arena, module);
    g->name = keep(m->arena, name);
    g->bytes = arena_alloc(m->arena, size == 0 ? 1 : size);
    if (size > 0) {
        memcpy(g->bytes, bytes, size);
    }
    g->size = size;
    g->align = align;
    m->globals[m->global_count++] = g;
    return g;
}

struct ir_global *ir_global_add_value(struct ir_module *m, const char *module,
                                      const char *name,
                                      struct ir_const *value)
{
    struct ir_global *g = ir_global_add(m, module, name, NULL, 0, 0);

    g->bytes = NULL;
    g->value = value;
    return g;
}

struct ir_const *ir_const_agg(struct ir_module *m, struct ir_vtype type,
                              size_t count)
{
    struct ir_const *c = arena_alloc(m->arena, sizeof *c);

    c->kind = IR_CONST_AGG;
    c->type = type;
    c->item_count = count;
    c->items = arena_alloc(m->arena, (count == 0 ? 1 : count) *
                                     sizeof *c->items);
    return c;
}

bool ir_const_equal(const struct ir_const *a, const struct ir_const *b)
{
    size_t i;

    if (a == NULL || b == NULL) {
        return a == b;
    }
    if (a->kind != b->kind || a->scalar != b->scalar) {
        return false;
    }
    switch (a->kind) {
    case IR_CONST_NONE:
        return true;
    case IR_CONST_INT:
        return a->integer == b->integer;
    case IR_CONST_FLOAT:
        /* The bits, so that 0.0 and -0.0 are two constants. */
        return memcmp(&a->floating, &b->floating, sizeof a->floating) == 0;
    case IR_CONST_SYM:
        return a->sym == b->sym;
    case IR_CONST_ADDR:
        return a->global == b->global;
    default: /* IR_CONST_AGG */
        if (a->type.agg != b->type.agg || a->item_count != b->item_count) {
            return false;
        }
        for (i = 0; i < a->item_count; i++) {
            if (!ir_const_equal(&a->items[i], &b->items[i])) {
                return false;
            }
        }
        return true;
    }
}

void ir_global_reloc(struct ir_module *m, struct ir_global *g,
                     uint64_t offset, uint32_t target)
{
    struct ir_reloc *relocs =
        arena_alloc(m->arena, (g->reloc_count + 1) * sizeof *relocs);

    if (g->reloc_count > 0) {
        memcpy(relocs, g->relocs, g->reloc_count * sizeof *relocs);
    }
    relocs[g->reloc_count].offset = offset;
    relocs[g->reloc_count].global = target;
    g->relocs = relocs;
    g->reloc_count++;
}

struct ir_operand ir_temp_op(const struct ir_function *f, uint32_t temp)
{
    struct ir_operand o = {IR_TEMP, IR_VOID, {0}};
    o.type = f->temps[temp];
    o.as.temp = temp;
    return o;
}

/* An integer constant keeps only the bits of its type, so that equal
   values have equal operands. */
struct ir_operand ir_int_op(enum ir_type type, uint64_t value)
{
    struct ir_operand o = {IR_INT, IR_VOID, {0}};
    o.type = type;
    switch (type) {
    case IR_I8:
        value &= 0xff;
        break;
    case IR_I16:
        value &= 0xffff;
        break;
    case IR_I32:
        value &= 0xffffffff;
        break;
    default:
        break;
    }
    o.as.integer = value;
    return o;
}

struct ir_operand ir_float_op(enum ir_type type, double value)
{
    struct ir_operand o = {IR_FLOAT, IR_VOID, {0}};
    o.type = type;
    o.as.floating = value;
    return o;
}

struct ir_operand ir_block_op(const struct ir_block *b)
{
    struct ir_operand o = {IR_BLOCK, IR_VOID, {0}};
    o.as.index = b->index;
    return o;
}

struct ir_operand ir_func_op(const struct ir_function *f)
{
    struct ir_operand o = {IR_FUNC, IR_PTR, {0}};
    o.as.index = f->index;
    return o;
}

struct ir_operand ir_global_op(const struct ir_global *g)
{
    struct ir_operand o = {IR_GLOBAL, IR_PTR, {0}};
    o.as.index = g->index;
    return o;
}

static struct ir_inst *append(struct ir_block *b, enum ir_op op,
                              enum ir_type type, uint32_t result)
{
    struct ir_inst *inst;

    b->insts = grow(b->insts, &b->capacity, b->count, sizeof *b->insts);
    inst = &b->insts[b->count++];
    memset(inst, 0, sizeof *inst);
    inst->of = ir_scalar(IR_VOID);
    inst->op = op;
    inst->type = type;
    inst->result = result;
    return inst;
}

void ir_inst_add(struct ir_block *b, const struct ir_inst *inst)
{
    struct ir_inst *copy = append(b, inst->op, inst->type, inst->result);

    *copy = *inst;
    copy->args = NULL;
    if (inst->arg_count > 0) {
        copy->args = malloc(inst->arg_count * sizeof *copy->args);
        if (copy->args == NULL) {
            fputs("antic: out of memory\n", stderr);
            exit(70);
        }
        memcpy(copy->args, inst->args, inst->arg_count * sizeof *copy->args);
    }
}

uint32_t ir_binary(struct ir_function *f, struct ir_block *b, enum ir_op op,
                   enum ir_type type, struct ir_operand x,
                   struct ir_operand y)
{
    uint32_t result = ir_temp(f, type);
    struct ir_inst *inst = append(b, op, type, result);

    inst->a = x;
    inst->b = y;
    return result;
}

uint32_t ir_unary(struct ir_function *f, struct ir_block *b, enum ir_op op,
                  enum ir_type type, struct ir_operand x)
{
    uint32_t result = ir_temp(f, type);
    append(b, op, type, result)->a = x;
    return result;
}

void ir_assign(struct ir_function *f, struct ir_block *b, uint32_t dst,
               struct ir_operand src)
{
    append(b, IR_COPY, f->temps[dst], dst)->a = src;
}

uint32_t ir_entry_slot(struct ir_function *f, struct ir_vtype of)
{
    struct ir_block *b = f->blocks[0];
    uint32_t result = ir_temp(f, IR_PTR);
    size_t at = 0;
    struct ir_inst *inst;

    while (at < b->count && b->insts[at].op == IR_SLOT) {
        at++;
    }
    b->insts = grow(b->insts, &b->capacity, b->count, sizeof *b->insts);
    memmove(&b->insts[at + 1], &b->insts[at],
            (b->count - at) * sizeof *b->insts);
    b->count++;
    inst = &b->insts[at];
    memset(inst, 0, sizeof *inst);
    inst->op = IR_SLOT;
    inst->type = IR_PTR;
    inst->result = result;
    inst->of = of;
    return result;
}

uint32_t ir_slot(struct ir_function *f, struct ir_block *b,
                 struct ir_vtype of)
{
    uint32_t result = ir_temp(f, IR_PTR);

    append(b, IR_SLOT, IR_PTR, result)->of = of;
    return result;
}

uint32_t ir_load(struct ir_function *f, struct ir_block *b, enum ir_type type,
                 struct ir_operand pointer)
{
    uint32_t result = ir_temp(f, type);
    append(b, IR_LOAD, type, result)->a = pointer;
    return result;
}

void ir_store(struct ir_function *f, struct ir_block *b, enum ir_type type,
              struct ir_operand value, struct ir_operand pointer)
{
    struct ir_inst *inst = append(b, IR_STORE, type, IR_NO_RESULT);

    (void)f;
    inst->a = value;
    inst->b = pointer;
}

uint32_t ir_ptradd(struct ir_function *f, struct ir_block *b,
                   struct ir_operand pointer, struct ir_operand offset)
{
    return ir_binary(f, b, IR_PTRADD, IR_PTR, pointer, offset);
}

void ir_memcopy(struct ir_function *f, struct ir_block *b,
                struct ir_operand dst, struct ir_operand src,
                struct ir_vtype of)
{
    struct ir_inst *inst = append(b, IR_MEMCOPY, IR_VOID, IR_NO_RESULT);

    (void)f;
    inst->a = dst;
    inst->b = src;
    inst->of = of;
}

uint32_t ir_addr(struct ir_function *f, struct ir_block *b,
                 struct ir_operand target)
{
    return ir_unary(f, b, IR_ADDR, IR_PTR, target);
}

uint32_t ir_bitload(struct ir_function *f, struct ir_block *b,
                    enum ir_type type, struct ir_operand pointer, uint32_t agg,
                    uint32_t field)
{
    uint32_t result = ir_temp(f, type);
    struct ir_inst *inst = append(b, IR_BITLOAD, type, result);

    inst->a = pointer;
    inst->of = ir_aggregate(agg);
    inst->field = field;
    return result;
}

void ir_bitstore(struct ir_function *f, struct ir_block *b, enum ir_type type,
                 struct ir_operand value, struct ir_operand pointer,
                 uint32_t agg, uint32_t field)
{
    struct ir_inst *inst = append(b, IR_BITSTORE, type, IR_NO_RESULT);

    (void)f;
    inst->a = value;
    inst->b = pointer;
    inst->of = ir_aggregate(agg);
    inst->field = field;
}

uint32_t ir_call(struct ir_function *f, struct ir_block *b, enum ir_type type,
                 struct ir_operand callee, const struct ir_operand *args,
                 size_t arg_count)
{
    uint32_t result = type == IR_VOID ? IR_NO_RESULT
                                      : ir_temp(f, type == IR_AGG ? IR_PTR
                                                                  : type);
    struct ir_inst *inst = append(b, IR_CALL, type, result);

    inst->a = callee;
    inst->arg_count = arg_count;
    if (arg_count > 0) {
        inst->args = malloc(arg_count * sizeof *inst->args);
        if (inst->args == NULL) {
            fputs("antic: out of memory\n", stderr);
            exit(70);
        }
        memcpy(inst->args, args, arg_count * sizeof *inst->args);
    }
    return result;
}

uint32_t ir_call_indirect(struct ir_function *f, struct ir_block *b,
                          enum ir_type type, struct ir_operand target,
                          const struct ir_function *signature,
                          const struct ir_operand *args, size_t arg_count)
{
    uint32_t result = ir_call(f, b, type, target, args, arg_count);

    b->insts[b->count - 1].b = ir_func_op(signature);
    return result;
}

void ir_jump(struct ir_function *f, struct ir_block *b,
             const struct ir_block *target)
{
    (void)f;
    append(b, IR_JUMP, IR_VOID, IR_NO_RESULT)->a = ir_block_op(target);
}

void ir_branch(struct ir_function *f, struct ir_block *b,
               struct ir_operand cond, const struct ir_block *then_block,
               const struct ir_block *else_block)
{
    struct ir_inst *inst = append(b, IR_BRANCH, IR_VOID, IR_NO_RESULT);

    (void)f;
    inst->a = cond;
    inst->b = ir_block_op(then_block);
    inst->c = ir_block_op(else_block);
}

void ir_ret(struct ir_function *f, struct ir_block *b, enum ir_type type,
            struct ir_operand value)
{
    (void)f;
    append(b, IR_RET, type, IR_NO_RESULT)->a = value;
}
