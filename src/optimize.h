#ifndef ANTIC_OPTIMIZE_H
#define ANTIC_OPTIMIZE_H

#include "ir.h"

void ir_optimize(struct ir_module *program, const char *entry);

/* Optimize the functions of module alone, for an object of its own in dev
   mode. The functions of other modules become declarations, whose symbols
   the objects of those modules define. Every function of module stays. */
void ir_optimize_module(struct ir_module *program, const char *module);

/* Run the passes of ir_optimize on one function, without removing unused
   functions. The back end uses it after it folds symbolic values. */
void ir_optimize_function(struct ir_function *f);

/* DESIGN: an assertion reaches this pass as a branch to a block that
   lowering marked. A library file carries it, so the build that compiles
   the program decides. Cutting the branch makes the block unreachable,
   and the passes that follow remove the call and the text with it. */
void ir_drop_asserts(struct ir_module *program);

#endif
