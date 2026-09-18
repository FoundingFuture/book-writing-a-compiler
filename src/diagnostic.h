#ifndef ANTIC_DIAGNOSTIC_H
#define ANTIC_DIAGNOSTIC_H

#include <stdbool.h>
#include <stddef.h>

/* A compile error or warning at a position in the source. Lines and
   columns count from 1, and a column counts bytes. A warning does not
   stop the compilation. */
struct diagnostic {
    int line;
    int column;
    bool warning;
    char message[160];
};

/* The errors of one compilation, in the order they were found. A
   zero-initialised struct diagnostics is empty and valid. */
struct diagnostics {
    struct diagnostic *items;
    size_t count;
    size_t capacity;
};

void diagnostics_add(struct diagnostics *d, int line, int column,
                     const char *format, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 4, 5)))
#endif
    ;
void diagnostics_warn(struct diagnostics *d, int line, int column,
                      const char *format, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 4, 5)))
#endif
    ;
void diagnostics_free(struct diagnostics *d);

#endif
