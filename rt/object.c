#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "object.h"

/* DESIGN: the default bodies of anti.rt.Object walk the field list of the
   descriptor. They are slow by design: a class that needs speed replaces
   the one it cares about with a concrete function. A build with
   --no-reflect has no field list, and these bodies then fall back to the
   class name and the address. */

/* DESIGN: the descriptor of the root belongs to the runtime. Every
   module of a program refers to this one, so `p is *Object` compares the
   same address wherever it is written, and no two modules define it. */
static const unsigned char object_name[] = "Object";

const struct anti_descriptor *const anti_rt_Object_ancestors[1] = {
    &anti_rt_Object_descriptor
};

const struct anti_descriptor anti_rt_Object_descriptor = {
    object_name, 6, NULL, (int64_t)sizeof(struct anti_object), 0,
    anti_rt_Object_ancestors, 0, NULL, NULL, 0, 0, NULL
};

const struct anti_descriptor *anti_rt_descriptor(const void *object)
{
    const struct anti_object *o = object;

    return o == NULL || o->table == NULL ? NULL : o->table[0];
}

/* DESIGN: a pointer to an interface sub-object points into the middle of
   an object. The descriptor of that sub-object's table records how far,
   so every operation that reads the whole object starts here. A class
   pointer has zero there and comes back unchanged. */
void *anti_rt_object_of(void *object)
{
    const struct anti_descriptor *d = anti_rt_descriptor(object);

    return d == NULL ? object : (char *)object - d->offset;
}

struct anti_text anti_rt_Object_type_name(struct anti_object *self)
{
    const struct anti_descriptor *d = anti_rt_descriptor(self);
    struct anti_text text;

    text.ptr = d == NULL ? (const unsigned char *)"" : d->name;
    text.len = d == NULL ? 0 : d->name_length;
    return text;
}

struct anti_text anti_rt_Object_to_text(struct anti_object *self)
{
    return anti_rt_Object_type_name(self);
}

/* The bytes of a field of the object, or NULL for a kind with no size
   here. The size of a pointer is the size of a pointer on this host,
   which is the host the object lives on. */
static size_t field_size(int64_t kind)
{
    switch (kind) {
    case ANTI_I8: return 1;
    case ANTI_I16: return 2;
    case ANTI_I32:
    case ANTI_F32: return 4;
    case ANTI_I64:
    case ANTI_F64: return 8;
    case ANTI_PTR: return sizeof(void *);
    case ANTI_CLONG: return sizeof(long);
    case ANTI_CWCHAR: return sizeof(int);
    default: return 0;
    }
}

/* Compare the fields the chain declares, from the root down. Two objects
   of different classes are never equal. */
int8_t anti_rt_Object_equals(struct anti_object *self,
                             struct anti_object *other)
{
    const struct anti_descriptor *d = anti_rt_descriptor(self);
    int64_t i;

    if (self == other) {
        return 1;
    }
    if (self == NULL || other == NULL ||
        d != anti_rt_descriptor(other)) {
        return 0;
    }
    for (; d != NULL; d = d->parent) {
        for (i = 0; i < d->field_count; i++) {
            const struct anti_field *f = &d->fields[i];
            size_t size = field_size(f->kind);
            if (size == 0) {
                continue;
            }
            if (memcmp((const char *)self + f->offset,
                       (const char *)other + f->offset, size) != 0) {
                return 0;
            }
        }
    }
    return 1;
}

/* FNV-1a over the same fields that equals compares, so two equal objects
   hash alike. */
uint64_t anti_rt_Object_hash(struct anti_object *self)
{
    const struct anti_descriptor *d = anti_rt_descriptor(self);
    uint64_t h = 1469598103934665603u;
    int64_t i;
    size_t k;

    for (; d != NULL; d = d->parent) {
        for (i = 0; i < d->field_count; i++) {
            const struct anti_field *f = &d->fields[i];
            const unsigned char *bytes =
                (const unsigned char *)self + f->offset;
            size_t size = field_size(f->kind);
            for (k = 0; k < size; k++) {
                h = (h ^ bytes[k]) * 1099511628211u;
            }
        }
    }
    return h;
}

/* Append the bytes of a C string. */
static void put(struct anti_builder *b, const char *text)
{
    anti_rt_builder_append(b, (const unsigned char *)text,
                           (int64_t)strlen(text));
}

/* Append a name as a JSON string, with the two escapes a field name or a
   class name can hold. */
static void put_name(struct anti_builder *b, const unsigned char *bytes,
                     int64_t len)
{
    int64_t i;

    put(b, "\"");
    for (i = 0; i < len; i++) {
        if (bytes[i] == '"' || bytes[i] == '\\') {
            anti_rt_builder_append(b, (const unsigned char *)"\\", 1);
        }
        anti_rt_builder_append(b, bytes + i, 1);
    }
    put(b, "\"");
}

/* The signed value of an integer field, which the field list records by
   its width alone. */
static int64_t integer_at(const void *bytes, int64_t kind)
{
    switch (kind) {
    case ANTI_I8: return *(const int8_t *)bytes;
    case ANTI_I16: return *(const int16_t *)bytes;
    case ANTI_I32: return *(const int32_t *)bytes;
    case ANTI_CWCHAR: return *(const int *)bytes;
    case ANTI_CLONG: return *(const long *)bytes;
    default: return *(const int64_t *)bytes;
    }
}

static void serialize_into(struct anti_builder *b, const void *object,
                           const struct anti_descriptor *d);

/* One member of the object, without its name. */
static void put_field(struct anti_builder *b, const void *object,
                      const struct anti_field *f)
{
    const void *bytes = (const char *)object + f->offset;
    char number[48];

    switch (f->kind) {
    case ANTI_F32:
        snprintf(number, sizeof number, "%.9g", (double)*(const float *)bytes);
        put(b, number);
        return;
    case ANTI_F64:
        snprintf(number, sizeof number, "%.17g", *(const double *)bytes);
        put(b, number);
        return;
    case ANTI_PTR: {
        const void *value = *(const void *const *)bytes;
        if (value == NULL) {
            put(b, "null");
        } else if (f->owned && f->descriptor != NULL) {
            serialize_into(b, value, anti_rt_descriptor(value));
        } else {
            snprintf(number, sizeof number, "%llu",
                     (unsigned long long)(uintptr_t)value);
            put(b, number);
        }
        return;
    }
    case ANTI_AGG:
        if (f->descriptor != NULL) {
            serialize_into(b, bytes, f->descriptor);
        } else {
            put(b, "null");
        }
        return;
    case ANTI_VOID:
        put(b, "null");
        return;
    default:
        snprintf(number, sizeof number, "%lld",
                 (long long)integer_at(bytes, f->kind));
        put(b, number);
        return;
    }
}

/* The object as a JSON object: the class name under "type", then one
   member per field of the chain, the root's first. */
static void serialize_into(struct anti_builder *b, const void *object,
                           const struct anti_descriptor *d)
{
    const struct anti_descriptor *up;
    int64_t depth;
    int64_t i;

    if (object == NULL || d == NULL) {
        put(b, "null");
        return;
    }
    put(b, "{\"type\":");
    put_name(b, d->name, d->name_length);
    /* The ancestor list is indexed by depth, so the root comes first and
       the class itself last. */
    for (depth = 0; depth <= d->depth; depth++) {
        up = d->ancestors == NULL ? NULL : d->ancestors[depth];
        if (up == NULL) {
            continue;
        }
        for (i = 0; i < up->field_count; i++) {
            put(b, ",");
            put_name(b, up->fields[i].name, up->fields[i].name_length);
            put(b, ":");
            put_field(b, object, &up->fields[i]);
        }
    }
    put(b, "}");
}

/* DESIGN: the default `serialize` writes the JSON that `anti.json`
   defines. A field the descriptor cannot read is written as null. The
   field list records the width of a field and not its type. A `str` and
   any other aggregate without a descriptor of its own are such fields,
   and a class that wants them replaces the body. */
void anti_rt_Object_serialize(struct anti_object *self, void *out)
{
    serialize_into(out, self, anti_rt_descriptor(self));
}

/* The root frees nothing. delete frees the `own` fields of each class of
   the chain and then the object itself. */
void anti_rt_Object_destruct(struct anti_object *self)
{
    (void)self;
}

/* The bytes and the length of the memory an `own` field points at. A
   pointer field owns one value of the class the field names, and a slice
   field owns its length in bytes of its element. */
static void **owned_at(void *object, const struct anti_field *f,
                       int64_t *size)
{
    void **slot = (void **)((char *)object + f->offset);

    if (f->kind == ANTI_PTR) {
        *size = f->descriptor != NULL ? f->descriptor->size : 0;
        return slot;
    }
    /* A slice is a pointer and a length, and the length counts elements
       of one byte for []byte. Anything wider needs the element size,
       which the field list does not hold, so the copy is shallow. */
    *size = f->kind == ANTI_AGG ? ((int64_t *)slot)[1] : 0;
    return slot;
}

/* A copy of every byte of the object, then a fresh copy of the memory
   behind each `own` field of the chain. */
void anti_rt_Object_copy(struct anti_object *self, struct anti_object *to)
{
    const struct anti_descriptor *d = anti_rt_descriptor(self);
    int64_t i;

    if (d == NULL) {
        return;
    }
    memcpy(to, self, (size_t)d->size);
    for (; d != NULL; d = d->parent) {
        for (i = 0; i < d->field_count; i++) {
            const struct anti_field *f = &d->fields[i];
            int64_t size = 0;
            void **from;
            void **into;
            if (!f->owned) {
                continue;
            }
            from = owned_at(self, f, &size);
            into = owned_at(to, f, &size);
            if (*from == NULL || size <= 0) {
                continue;
            }
            *into = malloc((size_t)size);
            if (*into != NULL) {
                memcpy(*into, *from, (size_t)size);
            }
        }
    }
}

/* The entry of a table, which the compiler fills with the function the
   concrete class ended with. */
static void *table_entry(const void *object, enum anti_entry entry)
{
    const struct anti_object *o = object;

    return o == NULL || o->table == NULL ? NULL : (void *)o->table[entry];
}

void *anti_rt_dup(void *object)
{
    const struct anti_descriptor *d;

    object = anti_rt_object_of(object);
    d = anti_rt_descriptor(object);
    void (*copy)(struct anti_object *, struct anti_object *) =
        (void (*)(struct anti_object *, struct anti_object *))
            table_entry(object, ANTI_ENTRY_COPY);
    void *made;

    if (d == NULL || copy == NULL) {
        return NULL;
    }
    made = malloc((size_t)d->size);
    if (made != NULL) {
        copy(object, made);
    }
    return made;
}

/* DESIGN: the destruct body of the concrete class runs first and the root's
   last. A class therefore tears down what it added before its base does.
   The `own` fields of each level are freed after every body has run, so
   a body still reads what it owns. */
void anti_rt_destroy(void *object)
{
    const struct anti_descriptor *d;
    const struct anti_descriptor *level;
    int64_t i;

    object = anti_rt_object_of(object);
    d = anti_rt_descriptor(object);

    if (d == NULL) {
        return;
    }
    for (level = d; level != NULL; level = level->parent) {
        if (level->destruct != NULL) {
            level->destruct(object);
        }
    }
    for (level = d; level != NULL; level = level->parent) {
        for (i = 0; i < level->field_count; i++) {
            const struct anti_field *f = &level->fields[i];
            int64_t size = 0;
            void **slot;
            if (!f->owned) {
                continue;
            }
            slot = owned_at(object, f, &size);
            free(*slot);
            *slot = NULL;
        }
    }
}

void anti_rt_delete(void *object)
{
    object = anti_rt_object_of(object);
    anti_rt_destroy(object);
    free(object);
}
