#include "target.h"

#include <string.h>

/* These names are also the directory names of the runtime archive,
   lib/<name>/, so a name here must match the one in libs/CMakeLists.txt. */
static const char *const names[TARGET_COUNT] = {
    [TARGET_LINUX_X86_64] = "linux-x86_64",
    [TARGET_LINUX_ARM64] = "linux-arm64",
    [TARGET_MACOS_X86_64] = "macos-x86_64",
    [TARGET_MACOS_ARM64] = "macos-arm64",
    [TARGET_WINDOWS_X86_64] = "windows-x86_64",
    [TARGET_WINDOWS_ARM64] = "windows-arm64",
};

const char *target_name(enum target t)
{
    return names[t];
}

bool target_from_name(const char *name, enum target *t)
{
    int i;

    for (i = 0; i < TARGET_COUNT; i++) {
        if (strcmp(name, names[i]) == 0) {
            *t = (enum target)i;
            return true;
        }
    }
    return false;
}

/* DESIGN: module.name on ELF and Mach-O, as docs/decisions.md settles.
   Mach-O prefixes every C-level symbol with '_'. The COFF form for
   Windows is specified in chapter 9. */
bool mangle(struct text *out, enum target t, const char *module,
            const char *name)
{
    switch (t) {
    case TARGET_MACOS_X86_64:
    case TARGET_MACOS_ARM64:
        text_appendf(out, "_%s.%s", module, name);
        return true;
    case TARGET_LINUX_X86_64:
    case TARGET_LINUX_ARM64:
        text_appendf(out, "%s.%s", module, name);
        return true;
    case TARGET_WINDOWS_X86_64:
    case TARGET_WINDOWS_ARM64:
    case TARGET_COUNT:
        break;
    }
    return false;
}

/* DESIGN: the host is fixed when antic is compiled, from the compiler's
   predefined macros. The antic_host_target test compares the result with
   the name CMake computes. */
bool target_host(enum target *t)
{
#if defined(__APPLE__) && defined(__aarch64__)
    *t = TARGET_MACOS_ARM64;
    return true;
#elif defined(__APPLE__) && defined(__x86_64__)
    *t = TARGET_MACOS_X86_64;
    return true;
#elif defined(__linux__) && defined(__aarch64__)
    *t = TARGET_LINUX_ARM64;
    return true;
#elif defined(__linux__) && defined(__x86_64__)
    *t = TARGET_LINUX_X86_64;
    return true;
#elif defined(_WIN32) && defined(_M_ARM64)
    *t = TARGET_WINDOWS_ARM64;
    return true;
#elif defined(_WIN32) && defined(_M_X64)
    *t = TARGET_WINDOWS_X86_64;
    return true;
#else
    (void)t;
    return false;
#endif
}
