#ifndef ANTIC_TEXT_H
#define ANTIC_TEXT_H

#include <stddef.h>

/* A growable byte buffer that stays NUL-terminated. A zero-initialised
   struct text is empty and valid. */
struct text {
    char *data;
    size_t length;
    size_t capacity;
};

void text_append(struct text *t, const char *s);
void text_append_bytes(struct text *t, const void *bytes, size_t n);
void text_appendf(struct text *t, const char *format, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
const char *text_cstr(const struct text *t);
void text_free(struct text *t);

#endif
