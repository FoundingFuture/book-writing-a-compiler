#ifndef ANTIC_TARGET_H
#define ANTIC_TARGET_H

#include <stdbool.h>

#include "text.h"

/* The minimum macOS version of both macOS targets. */
enum { MACOS_MIN_MAJOR = 11, MACOS_MIN_MINOR = 0 };

/* The runtime calls the program's main through the symbol of function
   RUNTIME_ENTRY in module RUNTIME_MODULE. That module name is reserved. */
#define RUNTIME_MODULE "anti.rt"
#define RUNTIME_ENTRY "main"

enum target {
    TARGET_LINUX_X86_64,
    TARGET_LINUX_ARM64,
    TARGET_MACOS_X86_64,
    TARGET_MACOS_ARM64,
    TARGET_WINDOWS_X86_64,
    TARGET_WINDOWS_ARM64,
    TARGET_COUNT
};

const char *target_name(enum target t);

/* Store the target antic runs on. Returns false on a host that is not
   one of the six targets. */
bool target_host(enum target *t);
bool target_from_name(const char *name, enum target *t);

/* Append the symbol of function name in module to out. Returns false for
   a target whose symbol form is not specified yet. */
bool mangle(struct text *out, enum target t, const char *module,
            const char *name);

#endif
