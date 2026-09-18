/* The root class of the object model and the data behind reflection. The
   compiler builds a descriptor and a table for every class. It lays both
   out with the C rules, so these declarations mirror what it writes. */
#ifndef ANTI_OBJECT_H
#define ANTI_OBJECT_H

#include <stdint.h>

#include "std.h"

/* The kind of a field, the value of enum ir_type in src/ir.h. */
enum anti_kind {
    ANTI_VOID,
    ANTI_I8,
    ANTI_I16,
    ANTI_I32,
    ANTI_I64,
    ANTI_F32,
    ANTI_F64,
    ANTI_PTR,
    ANTI_AGG,
    ANTI_CLONG,
    ANTI_CWCHAR
};

struct anti_descriptor;
struct anti_object;

/* One field a class declares. A field of class type carries its own
   descriptor, so a walk reaches the whole object. */
struct anti_field {
    const unsigned char *name;
    int64_t name_length;
    int64_t offset;
    int64_t kind;
    int64_t owned;
    const struct anti_descriptor *descriptor;
};

/* One public function of the chain of a class, with the entry of the
   table that holds it. A program reads the list and calls through the
   table, so reflection needs no name of its own. */
struct anti_function {
    const unsigned char *name;
    int64_t name_length;
    int64_t slot;
    int64_t param_count;
};

/* The record at entry 0 of the table of a class. The destruct entry is the
   body the class declares, not the one it inherits, because delete runs
   one body per level of the chain. */
struct anti_descriptor {
    const unsigned char *name;
    int64_t name_length;
    const struct anti_descriptor *parent;
    int64_t size;
    int64_t depth;
    const struct anti_descriptor *const *ancestors;
    int64_t field_count;
    const struct anti_field *fields;
    void (*destruct)(struct anti_object *self);
    /* How far the sub-object of an interface sits from the start of the
       object. The descriptor of a class has zero here, and the one an
       interface table points at has the offset of its sub-object. */
    int64_t offset;
    int64_t function_count;
    const struct anti_function *functions;
};

/* DESIGN: the seven functions of the root take the entries after the
   descriptor, in this order, in the table of every class. The order is
   root_names in src/lower.c, and a unit test pins the two together. */
enum anti_entry {
    ANTI_ENTRY_DESCRIPTOR,
    ANTI_ENTRY_TYPE_NAME,
    ANTI_ENTRY_TO_TEXT,
    ANTI_ENTRY_EQUALS,
    ANTI_ENTRY_HASH,
    ANTI_ENTRY_SERIALIZE,
    ANTI_ENTRY_DROP,
    ANTI_ENTRY_COPY
};

/* Every object starts with the address of its table, and entry 0 of a
   table is the descriptor of its class. */
struct anti_object {
    const struct anti_descriptor *const *table;
};

/* The descriptor of an object, or NULL when its table is not set. */
const struct anti_descriptor *anti_rt_descriptor(const void *object);

/* A copy of the object on the heap, of its concrete size, made by the
   copy entry of its table. */
void *anti_rt_dup(void *object);

/* Run the destruct body of each class of the chain, free the memory behind
   each `own` field, and free the object. */
void anti_rt_delete(void *object);

/* The same without the final free, for an object that is not on the
   heap of its own. */
void anti_rt_destroy(void *object);

/* The descriptor of the root and its one ancestor, which every module
   of a program shares. */
extern const struct anti_descriptor anti_rt_Object_descriptor;
extern const struct anti_descriptor *const anti_rt_Object_ancestors[1];

/* The start of the object that object points into. A pointer to an
   interface sub-object moves back by the offset its descriptor holds. */
void *anti_rt_object_of(void *object);

/* The seven functions of anti.rt.Object. A class replaces any of them
   with a concrete function of the same name. */
struct anti_text anti_rt_Object_type_name(struct anti_object *self);
struct anti_text anti_rt_Object_to_text(struct anti_object *self);
int8_t anti_rt_Object_equals(struct anti_object *self,
                             struct anti_object *other);
uint64_t anti_rt_Object_hash(struct anti_object *self);
void anti_rt_Object_serialize(struct anti_object *self, void *out);
void anti_rt_Object_destruct(struct anti_object *self);
void anti_rt_Object_copy(struct anti_object *self, struct anti_object *to);

#endif
