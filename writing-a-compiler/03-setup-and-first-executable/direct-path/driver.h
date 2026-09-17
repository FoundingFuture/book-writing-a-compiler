#ifndef ANTIC_DRIVER_H
#define ANTIC_DRIVER_H

#include <stdbool.h>

#include "target.h"

struct options {
    const char *input;          /* the .anti source file */
    const char *output;         /* NULL: the input path without .anti */
    const char *llvm_mc;        /* NULL: llvm-mc from PATH */
    const char *runtime;        /* directory holding lib/<target>/ */
    bool assembly_only;         /* -S: stop after writing assembly */
    enum target target;
};

/* Compile options->input and return the process exit status for antic. */
int driver_run(const struct options *options);

#endif
