#ifndef ANTIC_SELFPATH_H
#define ANTIC_SELFPATH_H

#include <stdbool.h>

#include "text.h"

/* Append the directory that holds the running antic to out, without a
   trailing separator. Returns false when the system does not say where
   the executable is. */
bool self_directory(struct text *out);

/* Whether path names a directory. */
bool directory_exists(const char *path);

#endif
