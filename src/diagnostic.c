#include "diagnostic.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static void add(struct diagnostics *d, int line, int column, bool warning,
                const char *format, va_list args)
{
    struct diagnostic *item;

    if (d->count == d->capacity) {
        size_t capacity = d->capacity == 0 ? 8 : d->capacity * 2;
        struct diagnostic *items =
            realloc(d->items, capacity * sizeof *items);
        if (items == NULL) {
            fputs("antic: out of memory\n", stderr);
            exit(70);
        }
        d->items = items;
        d->capacity = capacity;
    }
    item = &d->items[d->count++];
    item->line = line;
    item->column = column;
    item->warning = warning;
    vsnprintf(item->message, sizeof item->message, format, args);
}

void diagnostics_add(struct diagnostics *d, int line, int column,
                     const char *format, ...)
{
    va_list args;

    va_start(args, format);
    add(d, line, column, false, format, args);
    va_end(args);
}

void diagnostics_warn(struct diagnostics *d, int line, int column,
                      const char *format, ...)
{
    va_list args;

    va_start(args, format);
    add(d, line, column, true, format, args);
    va_end(args);
}

void diagnostics_free(struct diagnostics *d)
{
    free(d->items);
    d->items = NULL;
    d->count = 0;
    d->capacity = 0;
}
