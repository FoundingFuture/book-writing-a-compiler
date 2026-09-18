#ifndef ANTIC_LOWER_H
#define ANTIC_LOWER_H

#include <stdbool.h>

#include "ast.h"
#include "diagnostic.h"
#include "ir.h"

/* Translate a module that passed semantic analysis into IR. The ir field
   of a local or parameter symbol names the temporary of its value, or of
   its slot address when the address is taken. For a function it holds the
   IR function index. Returns false after an error in diags. Semantic
   analysis rejects every module that lowering cannot translate, so no
   construct of the language reports one. */
bool lower_module(struct module *module, const char *module_name,
                  struct ir_module *out, struct diagnostics *diags,
                  bool no_reflect);

#endif
