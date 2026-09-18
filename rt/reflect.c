/* What anti.reflect reads. A descriptor is read-only data of the program,
   so every function here is a walk of it.

   DESIGN: the runtime holds the walk because a descriptor is C data with
   C layout. Anti reads the results as plain values, and nothing of the
   layout reaches the language. */
#include <stdint.h>
#include <string.h>

#include "object.h"
#include "reflect.h"

struct anti_text anti_rt_reflect_class_name(const struct anti_descriptor *d)
{
    struct anti_text text;

    text.ptr = d == NULL ? (const unsigned char *)"" : d->name;
    text.len = d == NULL ? 0 : d->name_length;
    return text;
}

const struct anti_descriptor *anti_rt_reflect_parent(
    const struct anti_descriptor *d)
{
    return d == NULL ? NULL : d->parent;
}

int64_t anti_rt_reflect_field_count(const struct anti_descriptor *d)
{
    return d == NULL ? 0 : d->field_count;
}

/* The record at index, or NULL past the end. */
static const struct anti_field *field_at(const struct anti_descriptor *d,
                                         int64_t index)
{
    if (d == NULL || d->fields == NULL || index < 0 ||
        index >= d->field_count) {
        return NULL;
    }
    return &d->fields[index];
}

struct anti_text anti_rt_reflect_field_name(const struct anti_descriptor *d,
                                            int64_t index)
{
    const struct anti_field *f = field_at(d, index);
    struct anti_text text;

    text.ptr = f == NULL ? (const unsigned char *)"" : f->name;
    text.len = f == NULL ? 0 : f->name_length;
    return text;
}

int64_t anti_rt_reflect_field_offset(const struct anti_descriptor *d,
                                     int64_t index)
{
    const struct anti_field *f = field_at(d, index);

    return f == NULL ? -1 : f->offset;
}

int64_t anti_rt_reflect_field_kind(const struct anti_descriptor *d,
                                   int64_t index)
{
    const struct anti_field *f = field_at(d, index);

    return f == NULL ? -1 : f->kind;
}

int64_t anti_rt_reflect_field_owned(const struct anti_descriptor *d,
                                    int64_t index)
{
    const struct anti_field *f = field_at(d, index);

    return f == NULL ? 0 : f->owned;
}

int64_t anti_rt_reflect_function_count(const struct anti_descriptor *d)
{
    return d == NULL ? 0 : d->function_count;
}

static const struct anti_function *function_at(const struct anti_descriptor *d,
                                               int64_t index)
{
    if (d == NULL || d->functions == NULL || index < 0 ||
        index >= d->function_count) {
        return NULL;
    }
    return &d->functions[index];
}

struct anti_text anti_rt_reflect_function_name(const struct anti_descriptor *d,
                                               int64_t index)
{
    const struct anti_function *f = function_at(d, index);
    struct anti_text text;

    text.ptr = f == NULL ? (const unsigned char *)"" : f->name;
    text.len = f == NULL ? 0 : f->name_length;
    return text;
}

int64_t anti_rt_reflect_function_slot(const struct anti_descriptor *d,
                                      int64_t index)
{
    const struct anti_function *f = function_at(d, index);

    return f == NULL ? -1 : f->slot;
}

int64_t anti_rt_reflect_function_params(const struct anti_descriptor *d,
                                        int64_t index)
{
    const struct anti_function *f = function_at(d, index);

    return f == NULL ? 0 : f->param_count;
}

/* The bytes of one field of the object, as an i64 of its width. A field
   wider than eight bytes gives the address of its first byte. */
int64_t anti_rt_reflect_get(void *object, const struct anti_descriptor *d,
                            int64_t index)
{
    const struct anti_field *f = field_at(d, index);
    unsigned char *at;
    int64_t value = 0;

    if (f == NULL || object == NULL) {
        return 0;
    }
    at = (unsigned char *)anti_rt_object_of(object) + f->offset;
    switch (f->kind) {
    case ANTI_KIND_I8: return (int64_t)*(int8_t *)at;
    case ANTI_KIND_I16: return (int64_t)*(int16_t *)at;
    case ANTI_KIND_I32: return (int64_t)*(int32_t *)at;
    case ANTI_KIND_I64: return *(int64_t *)at;
    case ANTI_KIND_PTR:
        memcpy(&value, at, sizeof(void *));
        return value;
    default:
        return (int64_t)(intptr_t)at;
    }
}

void anti_rt_reflect_set(void *object, const struct anti_descriptor *d,
                         int64_t index, int64_t value)
{
    const struct anti_field *f = field_at(d, index);
    unsigned char *at;

    if (f == NULL || object == NULL) {
        return;
    }
    at = (unsigned char *)anti_rt_object_of(object) + f->offset;
    switch (f->kind) {
    case ANTI_KIND_I8: *(int8_t *)at = (int8_t)value; return;
    case ANTI_KIND_I16: *(int16_t *)at = (int16_t)value; return;
    case ANTI_KIND_I32: *(int32_t *)at = (int32_t)value; return;
    case ANTI_KIND_I64: *(int64_t *)at = value; return;
    case ANTI_KIND_PTR: memcpy(at, &value, sizeof(void *)); return;
    default: return;
    }
}
