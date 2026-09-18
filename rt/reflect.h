#ifndef ANTI_RT_REFLECT_H
#define ANTI_RT_REFLECT_H

#include <stdint.h>

#include "object.h"
#include "std.h"

/* The kinds a field record carries, which are the IR types of src/ir.h.
   A unit test pins the two together. */
#define ANTI_KIND_I8 1
#define ANTI_KIND_I16 2
#define ANTI_KIND_I32 3
#define ANTI_KIND_I64 4
#define ANTI_KIND_PTR 7

struct anti_text anti_rt_reflect_class_name(const struct anti_descriptor *d);
const struct anti_descriptor *anti_rt_reflect_parent(
    const struct anti_descriptor *d);
int64_t anti_rt_reflect_field_count(const struct anti_descriptor *d);
struct anti_text anti_rt_reflect_field_name(const struct anti_descriptor *d,
                                            int64_t index);
int64_t anti_rt_reflect_field_offset(const struct anti_descriptor *d,
                                     int64_t index);
int64_t anti_rt_reflect_field_kind(const struct anti_descriptor *d,
                                   int64_t index);
int64_t anti_rt_reflect_field_owned(const struct anti_descriptor *d,
                                    int64_t index);
int64_t anti_rt_reflect_function_count(const struct anti_descriptor *d);
struct anti_text anti_rt_reflect_function_name(const struct anti_descriptor *d,
                                               int64_t index);
int64_t anti_rt_reflect_function_slot(const struct anti_descriptor *d,
                                      int64_t index);
int64_t anti_rt_reflect_function_params(const struct anti_descriptor *d,
                                        int64_t index);
int64_t anti_rt_reflect_get(void *object, const struct anti_descriptor *d,
                            int64_t index);
void anti_rt_reflect_set(void *object, const struct anti_descriptor *d,
                         int64_t index, int64_t value);

#endif
