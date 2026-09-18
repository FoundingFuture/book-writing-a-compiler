#include "text.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Grow to hold extra more bytes plus the terminating NUL. The compiler
   stops on an allocation failure, because it has no way to continue. */
static void reserve(struct text *t, size_t extra)
{
    size_t needed = t->length + extra + 1;
    size_t capacity = t->capacity == 0 ? 64 : t->capacity;
    char *data;

    if (needed <= t->capacity) {
        return;
    }
    while (capacity < needed) {
        capacity *= 2;
    }
    data = realloc(t->data, capacity);
    if (data == NULL) {
        fputs("antic: out of memory\n", stderr);
        exit(70);
    }
    t->data = data;
    t->capacity = capacity;
}

void text_append(struct text *t, const char *s)
{
    size_t n = strlen(s);

    reserve(t, n);
    memcpy(t->data + t->length, s, n + 1);
    t->length += n;
}

void text_append_bytes(struct text *t, const void *bytes, size_t n)
{
    reserve(t, n);
    memcpy(t->data + t->length, bytes, n);
    t->length += n;
    t->data[t->length] = '\0';
}

void text_appendf(struct text *t, const char *format, ...)
{
    va_list args;
    int n;

    va_start(args, format);
    n = vsnprintf(NULL, 0, format, args);
    va_end(args);
    if (n < 0) {
        return;
    }
    reserve(t, (size_t)n);
    va_start(args, format);
    vsnprintf(t->data + t->length, (size_t)n + 1, format, args);
    va_end(args);
    t->length += (size_t)n;
}

const char *text_cstr(const struct text *t)
{
    return t->data == NULL ? "" : t->data;
}

void text_free(struct text *t)
{
    free(t->data);
    t->data = NULL;
    t->length = 0;
    t->capacity = 0;
}
