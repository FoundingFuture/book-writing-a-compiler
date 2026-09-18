#ifndef ANTIC_EMIT_H
#define ANTIC_EMIT_H

#include <stdbool.h>
#include <stddef.h>

#include "ir.h"
#include "mach.h"
#include "target.h"
#include "text.h"

/* Append the assembly file of a whole program to out. functions holds the
   machine code after register allocation for each IR function, and NULL
   for an extern function. The function main of module becomes the runtime
   entry. Returns false and writes a message to error for a program that
   the emitter cannot write yet. */
bool emit_program(struct text *out, enum target t, const struct ir_module *m,
                  struct mach_function **functions, const char *module,
                  char *error, size_t error_size);

/* Append the assembly file of one module in dev mode. Every function of
   the module is global and hidden, and the functions of other modules are
   symbols that their own objects define. */
bool emit_module(struct text *out, enum target t, const struct ir_module *m,
                 struct mach_function **functions, const char *module,
                 char *error, size_t error_size);

/* Append a constructor that calls C function function when the library
   loads. */
void emit_constructor(struct text *out, enum target t, const char *function);

/* Append the assembly of an object that holds bytes in the section
   anti_package and defines no symbol. A static library for C keeps the copy
   of its package header in it. */
void emit_package(struct text *out, enum target t, const char *bytes,
                  size_t length);

/* Append anti_licenses, the read-only data object of the licence notice
   with a NUL after its length bytes. */
void emit_licenses(struct text *out, enum target t, const char *bytes,
                   size_t length);

#endif
