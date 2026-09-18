#include "mach.h"

#include <inttypes.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "select.h"

struct mach_inst *mach_append(struct mach_block *b)
{
    struct mach_inst *inst;

    if (b->count == b->capacity) {
        size_t capacity = b->capacity == 0 ? 16 : b->capacity * 2;
        struct mach_inst *insts = realloc(b->insts, capacity * sizeof *insts);
        if (insts == NULL) {
            fputs("antic: out of memory\n", stderr);
            exit(70);
        }
        b->insts = insts;
        b->capacity = capacity;
    }
    inst = &b->insts[b->count++];
    memset(inst, 0, sizeof *inst);
    return inst;
}

uint32_t mach_slot_add(struct mach_function *f, uint64_t size,
                       uint64_t align)
{
    if (f->slot_count == f->slot_capacity) {
        size_t capacity = f->slot_capacity == 0 ? 8 : f->slot_capacity * 2;
        struct mach_slot *slots = realloc(f->slots, capacity * sizeof *slots);
        if (slots == NULL) {
            fputs("antic: out of memory\n", stderr);
            exit(70);
        }
        f->slots = slots;
        f->slot_capacity = capacity;
    }
    f->slots[f->slot_count].size = size;
    f->slots[f->slot_count].align = align;
    f->slots[f->slot_count].offset = 0;
    return (uint32_t)f->slot_count++;
}

uint32_t mach_vreg_add(struct mach_function *f, bool fp)
{
    if (f->vreg_count >= f->fp_capacity) {
        size_t capacity = f->fp_capacity == 0 ? 64 : f->fp_capacity * 2;
        bool *classes;
        while (capacity <= f->vreg_count) {
            capacity *= 2;
        }
        classes = realloc(f->fp, capacity * sizeof *classes);
        if (classes == NULL) {
            fputs("antic: out of memory\n", stderr);
            exit(70);
        }
        f->fp = classes;
        f->fp_capacity = capacity;
    }
    f->fp[f->vreg_count] = fp;
    return f->vreg_count++;
}

void mach_function_free(struct mach_function *f)
{
    size_t i;

    for (i = 0; i < f->block_count; i++) {
        free(f->blocks[i].insts);
    }
    free(f->blocks);
    free(f->slots);
    free(f->fp);
    f->fp = NULL;
    f->blocks = NULL;
    f->block_count = 0;
    f->slots = NULL;
    f->slot_count = 0;
}

void mach_print(struct text *out, const struct target_desc *target,
                const struct ir_module *m, const struct mach_function *f)
{
    size_t b;
    size_t i;

    text_appendf(out, "%s.%s:\n", f->ir->module, f->ir->name);
    for (b = 0; b < f->block_count; b++) {
        text_appendf(out, "b%zu:\n", b);
        for (i = 0; i < f->blocks[b].count; i++) {
            text_append(out, "    ");
            target->print(out, m, &f->blocks[b].insts[i], NULL);
            text_append(out, "\n");
        }
    }
}

void mach_symbol(struct text *out, const struct ir_module *m,
                 const struct names *names, const struct mach_operand *o)
{
    const struct ir_function *f;
    const struct ir_global *g;

    switch (o->kind) {
    case MACH_BLOCK:
        if (names == NULL) {
            text_appendf(out, "b%" PRId64, o->value);
        } else {
            block_label(out, names->target, names->function,
                        (size_t)o->value);
        }
        break;
    case MACH_FUNC:
        f = m->functions[o->value];
        if (names == NULL) {
            text_appendf(out, "%s%s%s", f->module != NULL ? f->module : "",
                         f->module != NULL ? "." : "", f->name);
        } else {
            mach_function_symbol(out, names->target, f);
        }
        break;
    case MACH_GLOBAL:
        g = m->globals[o->value];
        if (names == NULL) {
            text_appendf(out, "%s.%s", g->module, g->name);
        } else if (g->exported) {
            c_symbol(out, names->target, g->name);
        } else {
            mangle(out, names->target, g->module, g->name);
        }
        break;
    case MACH_NAME:
        if (names == NULL) {
            text_append(out, o->name);
        } else {
            c_symbol(out, names->target, o->name);
        }
        break;
    default:
        break;
    }
}

void mach_function_symbol(struct text *out, enum target t,
                          const struct ir_function *f)
{
    if (f->module == NULL || f->exported) {
        c_symbol(out, t, f->name);
    } else {
        mangle(out, t, f->module, f->name);
    }
}
