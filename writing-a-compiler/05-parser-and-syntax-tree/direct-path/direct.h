#ifndef ANTIC_DIRECT_H
#define ANTIC_DIRECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ast.h"
#include "diagnostic.h"

/* Check that module is the one program shape of chapter 3 and store N in
   *value. The shape is fn main() -> int { return N } with a semicolon
   after N. */
bool direct_check(const struct module *module, int64_t *value,
                  struct diagnostic *diag);

/* Lex, parse and check source. The first error of any stage comes back in
   diag. */
bool direct_parse(const char *source, size_t length, int64_t *value,
                  struct diagnostic *diag);

#endif
