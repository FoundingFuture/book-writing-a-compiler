#include "arena.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* DESIGN: hand out memory from large blocks and free all of it at the end
   of the compilation. The compiler never frees one node or one token on
   its own, so it needs no per-object bookkeeping. */

struct arena_block {
    struct arena_block *next;
    size_t used;
    size_t size;
};

enum { BLOCK_SIZE = 64 * 1024 };

static size_t align_up(size_t n)
{
    size_t align = _Alignof(max_align_t);
    return (n + align - 1) / align * align;
}

void *arena_alloc(struct arena *a, size_t size)
{
    size_t header = align_up(sizeof(struct arena_block));
    struct arena_block *b = a->head;
    void *p;

    size = align_up(size == 0 ? 1 : size);
    if (b == NULL || b->size - b->used < size) {
        size_t block_size = header + (size > BLOCK_SIZE ? size : BLOCK_SIZE);
        b = malloc(block_size);
        if (b == NULL) {
            fputs("antic: out of memory\n", stderr);
            exit(70);
        }
        b->next = a->head;
        b->used = header;
        b->size = block_size;
        a->head = b;
    }
    p = (char *)b + b->used;
    b->used += size;
    memset(p, 0, size);
    return p;
}

void arena_free(struct arena *a)
{
    struct arena_block *b = a->head;

    while (b != NULL) {
        struct arena_block *next = b->next;
        free(b);
        b = next;
    }
    a->head = NULL;
}
