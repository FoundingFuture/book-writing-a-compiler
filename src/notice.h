#ifndef ANTIC_NOTICE_H
#define ANTIC_NOTICE_H

#include <stddef.h>

#include "sema.h"
#include "text.h"

/* The begin and end markers of the licence notice, by which a tool finds
   anti_licenses in any Anti binary. */
#define NOTICE_BEGIN "ANTI_LICENSES_BEGIN\n"
#define NOTICE_END "ANTI_LICENSES_END\n"

/* Append the licence notice of the packages of a program between the
   markers. Each package name has a line with version and licence and its
   attribution lines, once. Then each distinct licence text follows once,
   after the names of the packages it covers. */
void notice_text(struct text *out, const struct package *const *packages,
                 size_t count);

#endif
