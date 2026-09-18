#include "header.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The C name of a scalar type. DESIGN: a sized type maps to its <stdint.h>
   name, and int, uint and float to int64_t, uint64_t and double. c_long,
   c_ulong and c_wchar map to long, unsigned long and wchar_t. The other c_
   types are sized types, so c_int is int32_t. */
static const char *scalar_name(const struct type *t)
{
    switch (t->kind) {
    case TYPE_VOID: return "void";
    case TYPE_BOOL: return "bool";
    case TYPE_I8: return "int8_t";
    case TYPE_I16: return "int16_t";
    case TYPE_I32: return "int32_t";
    case TYPE_I64: return "int64_t";
    case TYPE_U8: return "uint8_t";
    case TYPE_U16: return "uint16_t";
    case TYPE_U32: return "uint32_t";
    case TYPE_U64: return "uint64_t";
    case TYPE_F32: return "float";
    case TYPE_F64: return "double";
    case TYPE_CLONG: return "long";
    case TYPE_CULONG: return "unsigned long";
    case TYPE_CWCHAR: return "wchar_t";
    default: return "void";
    }
}

/* Names that a parameter or a field cannot take in a header for C11 and
   C++17. They are the keywords of both and the macros of the included
   headers. */
static const char *const reserved_names[] = {
    "_Alignas", "_Alignof", "_Atomic", "_Bool", "_Complex", "_Generic",
    "_Imaginary", "_Noreturn", "_Static_assert", "_Thread_local", "alignas",
    "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool",
    "break", "case", "catch", "char", "char16_t", "char32_t", "class", "compl",
    "const", "const_cast", "constexpr", "continue", "decltype", "default",
    "delete", "do", "double", "dynamic_cast", "else", "enum", "explicit",
    "export", "extern", "false", "float", "for", "friend", "goto", "if",
    "inline", "int", "long", "mutable", "namespace", "new", "noexcept", "not",
    "not_eq", "nullptr", "offsetof", "operator", "or", "or_eq", "private",
    "protected", "public", "register", "reinterpret_cast", "restrict",
    "return", "short", "signed", "sizeof", "static", "static_assert",
    "static_cast", "struct", "switch", "template", "this", "thread_local",
    "throw", "true", "try", "typedef", "typeid", "typename", "union",
    "unsigned", "using", "virtual", "void", "volatile", "wchar_t", "while",
    "xor", "xor_eq", "NULL", "ANTI_ALIGNAS"};

/* Whether name is a macro of <stdint.h>: INT8_MAX, UINT64_C, SIZE_MAX and
   the other names of that header that consist of capitals, digits and _. */
static bool stdint_macro(const struct name *name)
{
    static const char *const prefixes[] = {"INT", "UINT", "SIZE_", "PTRDIFF_",
                                           "SIG_ATOMIC_", "WCHAR_", "WINT_"};
    size_t i;

    for (i = 0; i < name->length; i++) {
        char c = name->text[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    for (i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++) {
        size_t n = strlen(prefixes[i]);
        if (name->length > n && memcmp(name->text, prefixes[i], n) == 0) {
            return true;
        }
    }
    return false;
}

/* DESIGN: the header keeps the Anti name of a parameter or a field. A
   name that C or C++ reserves gets a trailing _, as default_. */
static void c_name(char *out, size_t size, const struct name *name)
{
    size_t i;
    bool reserved = stdint_macro(name);

    for (i = 0; !reserved && i < sizeof reserved_names / sizeof *reserved_names;
         i++) {
        reserved = strlen(reserved_names[i]) == name->length &&
                   memcmp(reserved_names[i], name->text, name->length) == 0;
    }
    snprintf(out, size, "%.*s%s", (int)name->length, name->text,
             reserved ? "_" : "");
}

/* Append the C declaration of name with type t. owner is the aggregate
   whose definition holds the declaration, which names itself with its
   tag. */
static void declaration(struct text *out, const struct type *t,
                        const char *name, const struct type *owner)
{
    struct text inner = {0};
    size_t i;

    switch (t->kind) {
    case TYPE_POINTER:
        text_appendf(&inner, "*%s", name);
        declaration(out, t->element, text_cstr(&inner), owner);
        break;
    case TYPE_ARRAY:
        text_appendf(&inner, "%s[%" PRIu64 "]", name, t->length);
        declaration(out, t->element, text_cstr(&inner), owner);
        break;
    case TYPE_FN:
        /* An Anti function type is a function pointer in C. */
        text_appendf(&inner, "(*%s)(", name);
        for (i = 0; i < t->param_count; i++) {
            struct text param = {0};
            declaration(&param, t->params[i], "", owner);
            text_appendf(&inner, "%s%s", i > 0 ? ", " : "", text_cstr(&param));
            text_free(&param);
        }
        text_append(&inner, t->param_count == 0 ? "void)" : ")");
        declaration(out, t->result, text_cstr(&inner), owner);
        break;
    case TYPE_STRUCT:
    case TYPE_CLASS:
        if (t == owner) {
            text_appendf(out, "%s %.*s", t->is_union ? "union" : "struct",
                         (int)t->name.length, t->name.text);
        } else {
            text_appendf(out, "%.*s", (int)t->name.length, t->name.text);
        }
        text_appendf(out, "%s%s", name[0] != '\0' ? " " : "", name);
        break;
    case TYPE_ENUM:
        text_appendf(out, "%.*s%s%s", (int)t->name.length, t->name.text,
                     name[0] != '\0' ? " " : "", name);
        break;
    default:
        text_append(out, scalar_name(t));
        text_appendf(out, "%s%s", name[0] != '\0' ? " " : "", name);
        break;
    }
    text_free(&inner);
}

/* A doc comment as a C comment at indent, one line for one line of text
   and a gutter of stars otherwise. */
static void doc_comment(struct text *out, const struct doc_text *doc,
                        const char *indent)
{
    const char *p = doc->text;
    const char *end = doc->text + doc->length;

    if (doc->length == 0) {
        return;
    }
    if (memchr(p, '\n', doc->length) == NULL) {
        text_appendf(out, "%s/** %.*s */\n", indent, (int)doc->length, p);
        return;
    }
    text_appendf(out, "%s/**\n", indent);
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t n = nl != NULL ? (size_t)(nl - p) : (size_t)(end - p);
        text_appendf(out, "%s *%s%.*s\n", indent, n > 0 ? " " : "", (int)n, p);
        p += n + 1;
    }
    text_appendf(out, "%s */\n", indent);
}

struct emitted {
    const struct type **items;
    size_t count;
};

static bool was_emitted(const struct emitted *e, const struct type *t)
{
    size_t i;

    for (i = 0; i < e->count; i++) {
        if (e->items[i] == t) {
            return true;
        }
    }
    return false;
}

static void aggregate(struct text *out, const struct symbol *sym,
                      const struct interface *const *ifaces, size_t count,
                      struct emitted *done);

/* The export aggregate that type t holds by value, emitted first. */
static void emit_uses(struct text *out, const struct type *t,
                      const struct interface *const *ifaces, size_t count,
                      struct emitted *done)
{
    size_t i;
    size_t j;

    while (t->kind == TYPE_ARRAY) {
        t = t->element;
    }
    if (t->kind != TYPE_STRUCT || was_emitted(done, t)) {
        return;
    }
    for (i = 0; i < count; i++) {
        for (j = 0; j < ifaces[i]->item_count; j++) {
            if (ifaces[i]->items[j]->type == t) {
                aggregate(out, ifaces[i]->items[j], ifaces, count, done);
            }
        }
    }
}

/* DESIGN: an export struct or union becomes a typedef of the same name.
   packed becomes #pragma pack and align(N) an _Alignas on the first
   field, which C++ spells alignas. */
static void aggregate(struct text *out, const struct symbol *sym,
                      const struct interface *const *ifaces, size_t count,
                      struct emitted *done)
{
    const struct type *t = sym->type;
    const char *kind = t->is_union ? "union" : "struct";
    size_t i;

    if (was_emitted(done, t)) {
        return;
    }
    done->items[done->count++] = t;
    for (i = 0; i < t->field_count; i++) {
        emit_uses(out, t->fields[i].type, ifaces, count, done);
    }
    doc_comment(out, &sym->doc, "");
    if (t->packed) {
        text_append(out, "#pragma pack(push, 1)\n");
    }
    text_appendf(out, "typedef %s %.*s {\n", kind, (int)t->name.length,
                 t->name.text);
    for (i = 0; i < t->field_count; i++) {
        struct text field = {0};
        char buffer[128];
        c_name(buffer, sizeof buffer, &t->fields[i].name);
        doc_comment(out, &t->fields[i].doc, "    ");
        text_append(out, "    ");
        if (i == 0 && t->align != 0) {
            text_appendf(out, "ANTI_ALIGNAS(%" PRIu64 ") ", t->align);
        }
        if (type_field_is_unit_break(&t->fields[i])) {
            buffer[0] = '\0';
        }
        declaration(&field, t->fields[i].type, buffer, t);
        text_append(out, text_cstr(&field));
        if (t->fields[i].bits != 0 || type_field_is_unit_break(&t->fields[i])) {
            text_appendf(out, " : %u", (unsigned)t->fields[i].bits);
        }
        text_append(out, ";\n");
        text_free(&field);
    }
    text_appendf(out, "} %.*s;\n", (int)t->name.length, t->name.text);
    if (t->packed) {
        text_append(out, "#pragma pack(pop)\n");
    }
    text_append(out, "\n");
}

/* The public functions of the chain of t, base first, in table order. A
   name that repeats replaces the entry it repeats, as the table does. */
static size_t chain_functions(const struct type *t, const struct item **out,
                              size_t limit)
{
    size_t count = 0;
    size_t i;
    size_t j;

    if (t == NULL) {
        return 0;
    }
    if (t->kind == TYPE_CLASS) {
        count = chain_functions(t->base, out, limit);
    }
    for (i = 0; i < t->member_count; i++) {
        const struct item *m = t->members[i];
        if (m->kind != ITEM_FN || !m->pub) {
            continue;
        }
        for (j = 0; j < count; j++) {
            if (out[j]->name.length == m->name.length &&
                memcmp(out[j]->name.text, m->name.text, m->name.length) == 0) {
                out[j] = m;
                break;
            }
        }
        if (j == count && count < limit) {
            out[count++] = m;
        }
    }
    return count;
}

/* One prototype of a table entry or of a function of a class, with self
   written out as the first parameter. */
static void member_signature(struct text *out, const struct type *owner,
                             const struct item *m, const char *prefix,
                             bool pointer)
{
    const struct type *t = m->symbol->type;
    struct text inner = {0};
    struct text decl = {0};
    size_t i;

    if (pointer) {
        text_appendf(&inner, "(*%.*s)(", (int)m->name.length, m->name.text);
    } else {
        text_appendf(&inner, "%s%.*s_%.*s(", prefix, (int)owner->name.length,
                     owner->name.text, (int)m->name.length, m->name.text);
    }
    for (i = 0; i < t->param_count; i++) {
        struct text param = {0};
        char buffer[128];
        size_t k = i - (m->has_self ? 1 : 0);
        if (i == 0 && m->has_self) {
            text_appendf(&inner, "%.*s *self", (int)owner->name.length,
                         owner->name.text);
            continue;
        }
        if (m->symbol->params != NULL && k < m->param_count) {
            c_name(buffer, sizeof buffer, &m->symbol->params[k]);
        } else if (m->params != NULL && k < m->param_count) {
            c_name(buffer, sizeof buffer, &m->params[k].name);
        } else {
            snprintf(buffer, sizeof buffer, "a%zu", k);
        }
        declaration(&param, t->params[i], buffer, NULL);
        text_appendf(&inner, "%s%s", i > 0 ? ", " : "", text_cstr(&param));
        text_free(&param);
    }
    text_append(&inner, t->param_count == 0 ? "void)" : ")");
    declaration(&decl, t->result, text_cstr(&inner), NULL);
    text_append(out, text_cstr(&decl));
    text_free(&inner);
    text_free(&decl);
}

/* DESIGN: an export class becomes the nested layout and a table type. It
   also becomes an extern table and descriptor, one prototype per public
   function, and the helpers under the `anti_` prefix. An abstract class
   has no complete value, so it gets no table symbol and no `init`. */
static void class_view(struct text *out, const struct symbol *sym)
{
    const struct item *entries[64];
    const struct type *t = sym->type;
    int name_length = (int)t->name.length;
    const char *name_text = t->name.text;
    size_t count = chain_functions(t, entries, 64);
    struct text to_root = {0};
    const struct type *up;
    size_t i;
    size_t j;

    /* The table pointer sits in the root, so a wrapper reaches it
       through one `base` per level of the chain. */
    for (up = t; up != NULL && up->base != NULL; up = up->base) {
        text_append(&to_root, "base.");
    }

    text_appendf(out, "typedef struct %.*s %.*s;\n", name_length, name_text,
                 name_length, name_text);
    text_appendf(out, "typedef struct %.*s_vtable {\n"
                      "    const void *descriptor;\n"
                      "    /* The seven functions of anti.rt.Object. They "
                      "take and give Anti\n       values, so C reads their "
                      "slots and does not call them. */\n",
                 name_length, name_text);
    for (i = 0; i < count; i++) {
        if (entries[i]->runtime != NULL) {
            text_appendf(out, "    void *%.*s;\n",
                         (int)entries[i]->name.length, entries[i]->name.text);
            continue;
        }
        text_append(out, "    ");
        member_signature(out, t, entries[i], "", true);
        text_append(out, ";\n");
    }
    text_appendf(out, "} %.*s_vtable;\n\n", name_length, name_text);

    doc_comment(out, &sym->doc, "");
    text_appendf(out, "struct %.*s {\n", name_length, name_text);
    for (i = 0; i < t->field_count; i++) {
        struct text field = {0};
        char buffer[128];
        const struct struct_field *f = &t->fields[i];
        if (f->form == FIELD_TABLE) {
            text_appendf(out, "    const %.*s_vtable *vtable;\n", name_length,
                         name_text);
            continue;
        }
        if (f->form == FIELD_BASE) {
            bool root = f->type->base == NULL;
            text_appendf(out, "    %s%.*s base;\n", root ? "anti_" : "",
                         (int)f->type->name.length, f->type->name.text);
            continue;
        }
        c_name(buffer, sizeof buffer, &f->name);
        doc_comment(out, &f->doc, "    ");
        text_append(out, "    ");
        declaration(&field, f->type, buffer, t);
        text_append(out, text_cstr(&field));
        text_appendf(out, ";%s%s\n",
                     f->owned ? "   /* own */" : "",
                     f->vis == VIS_PUB ? ""
                     : f->vis == VIS_PROTECTED ? "   /* protected */"
                                               : "   /* private */");
        text_free(&field);
    }
    text_appendf(out, "};\n\n");

    text_appendf(out, "extern const anti_descriptor anti_%.*s_descriptor;\n",
                 name_length, name_text);
    if (t->has_abstract) {
        text_appendf(out, "/* %.*s is abstract: no table and no init, "
                          "because it has no complete value. */\n",
                     name_length, name_text);
    } else {
        text_appendf(out, "extern const %.*s_vtable anti_%.*s_vtable;\n"
                          "void anti_%.*s_init(%.*s *self);\n",
                     name_length, name_text, name_length, name_text,
                     name_length, name_text, name_length, name_text);
    }
    text_appendf(out,
                 "static inline void anti_%.*s_delete(%.*s *self)\n"
                 "{\n    anti_rt_delete(self);\n}\n"
                 "static inline void anti_%.*s_destroy(%.*s *self)\n"
                 "{\n    anti_rt_destroy(self);\n}\n"
                 "static inline %.*s *anti_%.*s_dup(%.*s *self)\n"
                 "{\n    return (%.*s *)anti_rt_dup(self);\n}\n\n",
                 name_length, name_text, name_length, name_text,
                 name_length, name_text, name_length, name_text,
                 name_length, name_text, name_length, name_text,
                 name_length, name_text, name_length, name_text);

    /* DESIGN: an interface sub-object is a field of the object, so C
       reaches the interface by taking its address. The table of that
       sub-object belongs to this class and holds thunks. */
    for (up = t; up != NULL; up = up->kind == TYPE_CLASS ? up->base : NULL) {
        for (i = 0; i < up->field_count; i++) {
            const struct struct_field *f = &up->fields[i];
            char buffer[128];
            if (f->form != FIELD_IMPL) {
                continue;
            }
            c_name(buffer, sizeof buffer, &f->name);
            text_appendf(out,
                         "extern const %.*s_vtable anti_%.*s_%.*s_vtable;\n"
                         "static inline %.*s *anti_%.*s_as_%.*s(%.*s *self)\n"
                         "{\n    return &self->%s%s;\n}\n",
                         (int)f->type->name.length, f->type->name.text,
                         name_length, name_text,
                         (int)f->type->name.length, f->type->name.text,
                         (int)f->type->name.length, f->type->name.text,
                         name_length, name_text,
                         (int)f->type->name.length, f->type->name.text,
                         name_length, name_text,
                         up == t ? "" : "base.", buffer);
        }
    }
    for (i = 0; i < count; i++) {
        if (entries[i]->runtime != NULL) {
            continue;
        }
        doc_comment(out, &entries[i]->doc, "");
        member_signature(out, t, entries[i], "", false);
        text_append(out, ";\n");
    }
    text_append(out, "\n");
    /* A wrapper per entry, which reads the table of the object. */
    for (i = 0; i < count; i++) {
        if (entries[i]->runtime != NULL) {
            continue;
        }
        text_append(out, "static inline ");
        member_signature(out, t, entries[i], "anti_", false);
        text_appendf(out,
                     "\n{\n    %s ((const %.*s_vtable *)self->%svtable)"
                     "->%.*s(self",
                     entries[i]->symbol->type->result->kind == TYPE_VOID
                         ? ""
                         : "return",
                     name_length, name_text, text_cstr(&to_root),
                     (int)entries[i]->name.length, entries[i]->name.text);
        for (j = 1; j < entries[i]->symbol->type->param_count; j++) {
            char buffer[128];
            size_t k = j - 1;
            if (entries[i]->symbol->params != NULL &&
                k < entries[i]->param_count) {
                c_name(buffer, sizeof buffer, &entries[i]->symbol->params[k]);
            } else if (entries[i]->params != NULL &&
                       k < entries[i]->param_count) {
                c_name(buffer, sizeof buffer, &entries[i]->params[k].name);
            } else {
                snprintf(buffer, sizeof buffer, "a%zu", k);
            }
            text_appendf(out, ", %s", buffer);
        }
        text_append(out, ");\n}\n");
    }
    text_append(out, "\n");
    text_free(&to_root);
}

static void constant(struct text *out, const struct symbol *sym)
{
    const struct const_value *v = sym->value;
    const struct type *t = sym->type;
    size_t i;

    doc_comment(out, &sym->doc, "");
    switch (v->kind) {
    case CONST_BOOL:
        text_appendf(out, "#define %.*s %s\n", (int)sym->name.length,
                     sym->name.text, v->as.boolean ? "true" : "false");
        return;
    case CONST_FLOAT:
        text_appendf(out, "#define %.*s %.17g%s\n", (int)sym->name.length,
                     sym->name.text, v->as.floating,
                     t->kind == TYPE_F32 ? "f" : "");
        return;
    case CONST_TEXT:
        text_appendf(out, "static const char %.*s[] = \"",
                     (int)sym->name.length, sym->name.text);
        for (i = 0; i < v->as.text.length; i++) {
            unsigned char c = (unsigned char)v->as.text.bytes[i];
            if (c == '"' || c == '\\') {
                text_appendf(out, "\\%c", c);
            } else if (c < 0x20 || c >= 0x7f) {
                text_appendf(out, "\\%03o", c);
            } else {
                text_appendf(out, "%c", c);
            }
        }
        text_append(out, "\";\n");
        return;
    default:
        text_appendf(out, "#define %.*s ((%s)%" PRId64 ")\n",
                     (int)sym->name.length, sym->name.text, scalar_name(t),
                     (int64_t)v->as.integer);
        return;
    }
}

/* A prototype names its parameters as the Anti function does. */
static void prototype(struct text *out, const struct symbol *sym)
{
    const struct type *t = sym->type;
    struct text inner = {0};
    struct text decl = {0};
    size_t i;

    doc_comment(out, &sym->doc, "");
    text_appendf(&inner, "%.*s(", (int)sym->name.length, sym->name.text);
    for (i = 0; i < t->param_count; i++) {
        struct text param = {0};
        char name[128];
        c_name(name, sizeof name, &sym->params[i]);
        declaration(&param, t->params[i], name, NULL);
        text_appendf(&inner, "%s%s", i > 0 ? ", " : "", text_cstr(&param));
        text_free(&param);
    }
    text_append(&inner, t->param_count == 0 ? "void)" : ")");
    declaration(&decl, t->result, text_cstr(&inner), NULL);
    text_appendf(out, "%s;\n", text_cstr(&decl));
    text_free(&inner);
    text_free(&decl);
}

void header_write(struct text *out, const char *name,
                  const struct interface *const *ifaces, size_t count,
                  bool bundled)
{
    struct emitted done = {0};
    size_t total = 0;
    size_t i;
    size_t j;
    bool any;

    for (i = 0; i < count; i++) {
        total += ifaces[i]->item_count;
    }
    done.items = calloc(total + 1, sizeof *done.items);
    text_appendf(out, "/* %s.h, the C interface of %s, written by antic.\n"
                      "   Do not edit. A failure that Anti cannot report calls "
                      "abort().%s */\n",
                 name, count > 0 ? ifaces[count - 1]->module : name,
                 bundled ? "\n   The archive holds the Anti runtime. Link only "
                           "one archive\n   with a bundled runtime into a "
                           "program."
                         : "");
    text_append(out, "#ifndef ");
    for (i = 0; name[i] != '\0'; i++) {
        char c = name[i];
        text_appendf(out, "%c", (c >= 'a' && c <= 'z') ? (char)(c - 32)
                                : ((c >= 'A' && c <= 'Z') ||
                                   (c >= '0' && c <= '9'))
                                    ? c
                                    : '_');
    }
    text_append(out, "_H\n#define ");
    for (i = 0; name[i] != '\0'; i++) {
        char c = name[i];
        text_appendf(out, "%c", (c >= 'a' && c <= 'z') ? (char)(c - 32)
                                : ((c >= 'A' && c <= 'Z') ||
                                   (c >= '0' && c <= '9'))
                                    ? c
                                    : '_');
    }
    text_append(out, "_H\n\n"
                     "#include <stdbool.h>\n"
                     "#include <stddef.h>\n"
                     "#include <stdint.h>\n\n"
                     "#ifdef __cplusplus\n"
                     "#define ANTI_ALIGNAS(n) alignas(n)\n"
                     "extern \"C\" {\n"
                     "#else\n"
                     "#define ANTI_ALIGNAS(n) _Alignas(n)\n"
                     "#endif\n\n");
    /* The root of every class chain, which the base of a class nests. */
    for (i = 0; i < count; i++) {
        for (j = 0; j < ifaces[i]->item_count; j++) {
            const struct symbol *sym = ifaces[i]->items[j];
            if (sym->exported && sym->kind == SYMBOL_STRUCT &&
                sym->type->kind == TYPE_CLASS) {
                text_append(out,
                    "/* The root of every class chain, and the record at "
                    "entry 0 of\n   every table. A C program reads the "
                    "layout and never builds one. */\n"
                    "typedef struct anti_descriptor anti_descriptor;\n"
                    "typedef struct anti_Object {\n"
                    "    const void *vtable;\n"
                    "} anti_Object;\n\n"
                    "/* Run the destruct chain of the object, free what it owns "
                    "and free it. */\n"
                    "void anti_rt_delete(void *object);\n"
                    "void anti_rt_destroy(void *object);\n"
                    "void *anti_rt_dup(void *object);\n\n");
                i = count;
                break;
            }
        }
    }
    for (i = 0; i < count; i++) {
        for (j = 0; j < ifaces[i]->item_count; j++) {
            const struct symbol *sym = ifaces[i]->items[j];
            if (sym->exported && sym->kind == SYMBOL_STRUCT) {
                if (sym->type->kind == TYPE_CLASS) {
                    class_view(out, sym);
                } else {
                    aggregate(out, sym, ifaces, count, &done);
                }
            }
        }
    }
    for (i = 0, any = false; i < count; i++) {
        for (j = 0; j < ifaces[i]->item_count; j++) {
            const struct symbol *sym = ifaces[i]->items[j];
            if (sym->exported && sym->kind == SYMBOL_CONST) {
                constant(out, sym);
                any = true;
            }
        }
    }
    text_append(out, any ? "\n" : "");
    for (i = 0, any = false; i < count; i++) {
        for (j = 0; j < ifaces[i]->item_count; j++) {
            const struct symbol *sym = ifaces[i]->items[j];
            if (sym->exported && sym->kind == SYMBOL_FN) {
                prototype(out, sym);
                any = true;
            }
        }
    }
    text_append(out, any ? "\n" : "");
    text_append(out, "#ifdef __cplusplus\n}\n#endif\n\n#endif\n");
    free((void *)done.items);
}
