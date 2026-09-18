#ifndef ANTIC_MODPATH_H
#define ANTIC_MODPATH_H

#include <stdbool.h>
#include <stddef.h>

#include "text.h"

/* A module path names a module: lowercase identifiers joined by dots, as
   com.niese.geo, that mirror the directories under a search root. */

/* Append the module path of source file source to out. The path is the
   file's path under the first root that holds it, with dots for slashes
   and without the suffix. Outside every root it is the file name alone.
   Returns false and writes a message to error for a segment that is not a
   lowercase identifier or that is a keyword. */
bool module_path_of_source(const char *source, const char *const *roots,
                           size_t root_count, struct text *out, char *error,
                           size_t error_size);

/* Whether path starts with the segment anti, which the language's own
   libraries use. */
bool module_path_reserved(const char *path);

size_t module_path_segments(const char *path);

/* The last segment, the name an import declares. */
const char *module_path_last(const char *path);

#endif
