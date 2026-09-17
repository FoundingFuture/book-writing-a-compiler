#ifndef ANTIC_ARENA_H
#define ANTIC_ARENA_H

#include <stddef.h>

/* Memory for data that lives until the whole compilation ends, such as
   token text and syntax tree nodes. arena_free releases all of it at once.
   A zero-initialised struct is empty and valid. */
struct arena_block;

struct arena {
    struct arena_block *head;
};

/* Return size bytes of zeroed memory, aligned for any object type. */
void *arena_alloc(struct arena *a, size_t size);
void arena_free(struct arena *a);

#endif
