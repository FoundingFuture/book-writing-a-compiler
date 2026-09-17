#ifndef ANTIC_ANTL_H
#define ANTIC_ANTL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arena.h"
#include "ir.h"
#include "sema.h"
#include "text.h"
#include "types.h"

/* A library file holds the interface of one module and the unoptimised IR
   of all its functions. Its bytes depend only on the source, so every
   host writes the same file. */

#define ANTL_SUFFIX ".antl"
#define ANTL_VERSION 13

/* Append the library file of a checked and lowered module to out, with
   the package header of iface. strip_docs leaves the doc text out. */
void antl_write(struct text *out, const struct interface *iface,
                const struct ir_module *ir, bool strip_docs);

/* Append the start of the library file of iface up to its imports and doc
   text, which antl_header reads. A static library for C holds it as the
   copy of its package header. */
void antl_write_header(struct text *out, const struct interface *iface);

/* Read the package header, the module name, the imports and the module's
   doc text of a library file into out, with no items. Returns false and writes a message to error when the file is
   not a library file of this version. */
bool antl_header(const uint8_t *data, size_t size, struct arena *arena,
                 struct interface *out, char *error, size_t error_size);

/* Read a library file. Its types go into types, and its functions and
   globals are appended to program. libraries holds the interfaces read
   before, which include every import of the file. Returns NULL and writes
   a message to error when the file cannot be used. */
struct interface *antl_read(const uint8_t *data, size_t size,
                            const struct interface *const *libraries,
                            size_t library_count, struct types *types,
                            struct arena *arena, struct ir_module *program,
                            char *error, size_t error_size);

#endif
