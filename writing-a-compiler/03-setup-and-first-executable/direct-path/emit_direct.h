#ifndef ANTIC_EMIT_DIRECT_H
#define ANTIC_EMIT_DIRECT_H

#include <stdbool.h>
#include <stdint.h>

#include "target.h"
#include "text.h"

/* Append the assembly text of the chapter 3 program to out: a main in
   module that returns value. Returns false for a target without a
   direct path. */
bool emit_direct(struct text *out, enum target t, const char *module,
                 int64_t value);

#endif
