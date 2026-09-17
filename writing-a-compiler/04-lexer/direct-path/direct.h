#ifndef ANTIC_DIRECT_H
#define ANTIC_DIRECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "diagnostic.h"
#include "lexer.h"

/* Recognise the one program shape of chapter 3 in a token list and store
   N in *value. The shape is fn main() -> int { return N } with a
   semicolon after N. */
bool direct_parse_tokens(const char *source, const struct token_list *tokens,
                         int64_t *value, struct diagnostic *diag);

/* Lex source and run direct_parse_tokens. A lexical error comes back as
   the first diagnostic. */
bool direct_parse(const char *source, size_t length, int64_t *value,
                  struct diagnostic *diag);

#endif
