#include <stdlib.h>
#include <string.h>

#include "std.h"

struct anti_text anti_rt_text_from_c(const unsigned char *bytes)
{
    struct anti_text text;

    text.ptr = bytes;
    text.len = (int64_t)strlen((const char *)bytes);
    return text;
}

struct anti_text anti_rt_text_slice(const unsigned char *bytes, int64_t len)
{
    struct anti_text text;

    text.ptr = bytes;
    text.len = len;
    return text;
}

/* DESIGN: the buffer of `anti.text.Builder` lives here. The default
   `serialize` of the root class writes an object into one, and the
   runtime holds that body. The class is a face over these three
   functions, so one buffer serves both sides. A mismatch of the layout
   breaks the builder test at once. */
void anti_rt_builder_append(struct anti_builder *b,
                            const unsigned char *bytes, int64_t len)
{
    if (len <= 0) {
        return;
    }
    if (b->length + len + 1 > b->capacity) {
        int64_t size = b->capacity == 0 ? 32 : b->capacity;
        unsigned char *room;
        while (size < b->length + len + 1) {
            size *= 2;
        }
        room = malloc((size_t)size);
        if (room == NULL) {
            return;
        }
        if (b->room != NULL) {
            memcpy(room, b->room, (size_t)b->length);
            free(b->room);
        }
        b->room = room;
        b->capacity = size;
    }
    memcpy(b->room + b->length, bytes, (size_t)len);
    b->length += len;
    b->room[b->length] = 0;
}

struct anti_text anti_rt_builder_text(const struct anti_builder *b)
{
    struct anti_text text;

    text.ptr = b->room == NULL ? (const unsigned char *)"" : b->room;
    text.len = b->length;
    return text;
}

void anti_rt_builder_clear(struct anti_builder *b)
{
    b->length = 0;
    if (b->room != NULL) {
        b->room[0] = 0;
    }
}
