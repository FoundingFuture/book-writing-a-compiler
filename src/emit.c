#include "emit.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "select.h"

/* DESIGN: one assembly file holds the whole program. Every Anti function
   is a local symbol, and only the runtime entry is global. */

static void emit_function(struct text *out, enum target t,
                          const struct ir_module *m,
                          const struct mach_function *f, bool module)
{
    const struct target_desc *desc = target_desc(t);
    struct text symbol = {0};
    struct names names;
    size_t b;
    size_t i;

    mach_function_symbol(&symbol, t, f->ir);
    names.target = t;
    names.function = text_cstr(&symbol);
    if (f->ir->exported || module) {
        text_appendf(out, "    .globl %s\n", text_cstr(&symbol));
    }
    /* DESIGN: in an object of one module, every function is global for the
       other modules and hidden. A shared library then exports only the
       export fns. COFF has no hidden symbols, and the .def file of a DLL
       names its exports. */
    if (module && !f->ir->exported &&
        target_info(t)->format != FORMAT_COFF) {
        text_appendf(out, "    %s %s\n",
                     target_info(t)->format == FORMAT_MACHO ? ".private_extern"
                                                           : ".hidden",
                     text_cstr(&symbol));
    }
    /* An ARM64 instruction is 4 bytes, and a function starts on one. */
    if (target_info(t)->arch == ARCH_ARM64) {
        text_append(out, "    .p2align 2\n");
    }
    text_appendf(out, "%s:\n", text_cstr(&symbol));
    /* A function with Windows unwind data. llvm-mc writes its pdata and
       xdata from the .seh_ lines. */
    if (f->unwind) {
        text_appendf(out, "    .seh_proc %s\n", text_cstr(&symbol));
    }
    for (b = 0; b < f->block_count; b++) {
        block_label(out, t, text_cstr(&symbol), b);
        text_append(out, ":\n");
        for (i = 0; i < f->blocks[b].count; i++) {
            text_append(out, "    ");
            desc->print(out, m, &f->blocks[b].insts[i], &names);
            text_append(out, "\n");
        }
    }
    if (f->unwind) {
        text_append(out, "    .seh_endproc\n");
    }
    text_free(&symbol);
}

/* The runtime reaches main through a second global name, RUNTIME_ENTRY
   of RUNTIME_MODULE, which .set defines. */
static void emit_entry(struct text *out, enum target t,
                       const struct ir_module *m, const char *module)
{
    struct text entry = {0};
    struct text main_symbol = {0};
    size_t i;

    for (i = 0; i < m->function_count; i++) {
        const struct ir_function *f = m->functions[i];
        if (!f->is_extern && f->module != NULL &&
            strcmp(f->module, module) == 0 && strcmp(f->name, "main") == 0) {
            mangle(&entry, t, RUNTIME_MODULE, RUNTIME_ENTRY);
            mangle(&main_symbol, t, module, "main");
            text_appendf(out, "    .globl %s\n", text_cstr(&entry));
            text_appendf(out, "    .set %s, %s\n", text_cstr(&entry),
                         text_cstr(&main_symbol));
        }
    }
    text_free(&entry);
    text_free(&main_symbol);
}

/* DESIGN: global data holds the bytes of literals, which no program writes
   to, so it goes to the read-only data section of each object format. */
static const char *const data_sections[] = {
    [FORMAT_ELF] = ".rodata",
    [FORMAT_MACHO] = "__TEXT,__const",
    [FORMAT_COFF] = ".rdata,\"dr\"",
};

/* DESIGN: data that holds an address is written once when the program
   loads, so a section mapped read-only from the start cannot hold it.
   Every format has a section for data the loader writes and then
   protects, and PE protects .rdata after its base relocations. */
static const char *const reloc_sections[] = {
    [FORMAT_ELF] = ".data.rel.ro",
    [FORMAT_MACHO] = "__DATA,__const",
    [FORMAT_COFF] = ".rdata,\"dr\"",
};

/* The global that the address at offset names, or NULL. */
static const struct ir_global *reloc_at(const struct ir_module *m,
                                        const struct ir_global *g,
                                        uint64_t offset)
{
    size_t i;

    for (i = 0; i < g->reloc_count; i++) {
        if (g->relocs[i].offset == offset) {
            return m->globals[g->relocs[i].global];
        }
    }
    return NULL;
}

/* The bytes of one global, with an address written as the symbol it
   names, so that the linker fills in the eight bytes. */
static void emit_global(struct text *out, enum target t,
                        const struct ir_module *m, const struct ir_global *g)
{
    struct text name = {0};
    uint64_t align;
    uint64_t k;
    int log2;
    bool open = false;

    for (log2 = 0, align = g->align; align > 1; align /= 2) {
        log2++;
    }
    if (log2 > 0) {
        text_appendf(out, "    .p2align %d\n", log2);
    }
    mangle(&name, t, g->module, g->name);
    text_appendf(out, "%s:\n", text_cstr(&name));
    text_free(&name);
    for (k = 0; k < g->size; k++) {
        const struct ir_global *target = reloc_at(m, g, k);
        if (target != NULL) {
            struct text symbol = {0};
            if (open) {
                text_append(out, "\n");
                open = false;
            }
            mangle(&symbol, t, target->module, target->name);
            text_appendf(out, "    .quad %s\n", text_cstr(&symbol));
            text_free(&symbol);
            k += 7;
            continue;
        }
        text_appendf(out, "%s0x%02x", open ? ", " : "    .byte ", g->bytes[k]);
        open = true;
        if (k % 16 == 15 || k + 1 == g->size) {
            text_append(out, "\n");
            open = false;
        }
    }
    if (open) {
        text_append(out, "\n");
    }
}

/* DESIGN: a global that holds an address goes to the section for data the
   loader writes, and every other global to the read-only section. The two
   are written in one pass each, so each section is named once. */
static void emit_data(struct text *out, enum target t,
                      const struct ir_module *m)
{
    enum object_format format = target_info(t)->format;
    bool relocated = false;
    size_t i;

    text_appendf(out, "    .section %s\n", data_sections[format]);
    for (i = 0; i < m->global_count; i++) {
        if (m->globals[i]->reloc_count > 0) {
            relocated = true;
            continue;
        }
        emit_global(out, t, m, m->globals[i]);
    }
    if (!relocated) {
        return;
    }
    text_appendf(out, "    .section %s\n", reloc_sections[format]);
    for (i = 0; i < m->global_count; i++) {
        if (m->globals[i]->reloc_count > 0) {
            emit_global(out, t, m, m->globals[i]);
        }
    }
}

static bool emit(struct text *out, enum target t, const struct ir_module *m,
                 struct mach_function **functions, const char *module,
                 bool one_module, char *error, size_t error_size)
{
    const struct target_info *info = target_info(t);
    size_t i;
    size_t j;

    /* An address takes the eight bytes at its offset, so it lies inside
       the data that holds it. A library file read from disk is the one
       source of a global that does not. */
    for (i = 0; i < m->global_count; i++) {
        const struct ir_global *g = m->globals[i];
        for (j = 0; j < g->reloc_count; j++) {
            if (g->relocs[j].offset + 8 > g->size) {
                snprintf(error, error_size,
                         "the address at %" PRIu64 " of `%s.%s` ends past "
                         "its %" PRIu64 " bytes", g->relocs[j].offset,
                         g->module, g->name, g->size);
                return false;
            }
        }
    }
    if (info->format == FORMAT_MACHO) {
        text_appendf(out, "    .build_version macos, %d, %d\n",
                     MACOS_MIN_MAJOR, MACOS_MIN_MINOR);
    }
    text_append(out, "    .text\n");
    emit_entry(out, t, m, module);
    for (i = 0; i < m->function_count; i++) {
        if (functions[i] != NULL) {
            emit_function(out, t, m, functions[i], one_module);
        }
    }
    if (m->global_count > 0) {
        emit_data(out, t, m);
    }
    /* Without this note GNU ld may mark the stack executable. */
    if (info->format == FORMAT_ELF) {
        text_append(out, "    .section .note.GNU-stack,\"\",@progbits\n");
    }
    return true;
}

bool emit_program(struct text *out, enum target t, const struct ir_module *m,
                  struct mach_function **functions, const char *module,
                  char *error, size_t error_size)
{
    return emit(out, t, m, functions, module, false, error, error_size);
}

bool emit_module(struct text *out, enum target t, const struct ir_module *m,
                 struct mach_function **functions, const char *module,
                 char *error, size_t error_size)
{
    return emit(out, t, m, functions, module, true, error, error_size);
}

/* DESIGN: a shared library initialises the runtime in a constructor. The
   loader calls every function whose address lies in .init_array on ELF,
   __mod_init_func on Mach-O and .CRT$XCU on COFF. */
void emit_constructor(struct text *out, enum target t, const char *function)
{
    static const char *const sections[] = {
        [FORMAT_ELF] = ".init_array,\"aw\"",
        [FORMAT_MACHO] = "__DATA,__mod_init_func,mod_init_funcs",
        [FORMAT_COFF] = ".CRT$XCU,\"dr\"",
    };
    struct text symbol = {0};

    c_symbol(&symbol, t, function);
    text_appendf(out, "    .section %s\n    .p2align 3\n    .quad %s\n",
                 sections[target_info(t)->format], text_cstr(&symbol));
    text_free(&symbol);
}

void emit_licenses(struct text *out, enum target t, const char *bytes,
                   size_t length)
{
    struct text symbol = {0};
    size_t k;

    c_symbol(&symbol, t, "anti_licenses");
    text_appendf(out, "    .section %s\n    .globl %s\n%s:\n",
                 data_sections[target_info(t)->format], text_cstr(&symbol),
                 text_cstr(&symbol));
    for (k = 0; k <= length; k++) {
        unsigned char c = k < length ? (unsigned char)bytes[k] : 0;
        text_appendf(out, "%s0x%02x", k % 16 == 0 ? "    .byte " : ", ", c);
        if (k % 16 == 15 || k == length) {
            text_append(out, "\n");
        }
    }
    text_free(&symbol);
}

/* DESIGN: the copy of the package header in a static library is an object
   file, because Apple's linker refuses an archive member of another kind.
   No symbol names the section, so no link takes the member. */
void emit_package(struct text *out, enum target t, const char *bytes,
                  size_t length)
{
    static const char *const sections[] = {
        [FORMAT_ELF] = ".anti_package,\"\",@progbits",
        [FORMAT_MACHO] = "__DATA,__anti_package",
        [FORMAT_COFF] = ".anti_package,\"dr\"",
    };
    size_t k;

    if (target_info(t)->format == FORMAT_MACHO) {
        text_appendf(out, "    .build_version macos, %d, %d\n",
                     MACOS_MIN_MAJOR, MACOS_MIN_MINOR);
    }
    text_appendf(out, "    .section %s\n", sections[target_info(t)->format]);
    for (k = 0; k < length; k++) {
        text_appendf(out, "%s0x%02x", k % 16 == 0 ? "    .byte " : ", ",
                     (unsigned char)bytes[k]);
        if (k % 16 == 15 || k + 1 == length) {
            text_append(out, "\n");
        }
    }
}
