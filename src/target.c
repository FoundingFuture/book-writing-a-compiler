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

/* DESIGN: one row per target, in the order of enum target. The triples
   select the object format in llvm-mc, and the minimum macOS version comes
   from the .build_version directive in the assembly. */
static const struct target_info infos[TARGET_COUNT] = {
    [TARGET_LINUX_X86_64] = {OS_LINUX, ARCH_X86_64, FORMAT_ELF,
                             CONVENTION_SYSV, "x86_64-unknown-linux-gnu",
                             ".o", ""},
    [TARGET_LINUX_ARM64] = {OS_LINUX, ARCH_ARM64, FORMAT_ELF,
                            CONVENTION_AAPCS64, "aarch64-unknown-linux-gnu",
                            ".o", ""},
    [TARGET_MACOS_X86_64] = {OS_MACOS, ARCH_X86_64, FORMAT_MACHO,
                             CONVENTION_SYSV, "x86_64-apple-macos", ".o", ""},
    [TARGET_MACOS_ARM64] = {OS_MACOS, ARCH_ARM64, FORMAT_MACHO,
                            CONVENTION_APPLE_ARM64, "arm64-apple-macos", ".o",
                            ""},
    [TARGET_WINDOWS_X86_64] = {OS_WINDOWS, ARCH_X86_64, FORMAT_COFF,
                               CONVENTION_WINDOWS_X64,
                               "x86_64-pc-windows-msvc", ".obj", ".exe"},
    [TARGET_WINDOWS_ARM64] = {OS_WINDOWS, ARCH_ARM64, FORMAT_COFF,
                              CONVENTION_WINDOWS_ARM64,
                              "aarch64-pc-windows-msvc", ".obj", ".exe"},
};

const char *target_name(enum target t)
{
    return names[t];
}

const struct target_info *target_info(enum target t)
{
    return &infos[t];
}

const char *object_format_name(enum object_format format)
{
    static const char *const formats[] = {
        [FORMAT_ELF] = "ELF", [FORMAT_MACHO] = "Mach-O", [FORMAT_COFF] = "COFF",
    };
    return formats[format];
}

const char *convention_name(enum convention convention)
{
    static const char *const conventions[] = {
        [CONVENTION_SYSV] = "System V AMD64",
        [CONVENTION_WINDOWS_X64] = "Windows x64",
        [CONVENTION_AAPCS64] = "AAPCS64",
        [CONVENTION_APPLE_ARM64] = "Apple ARM64",
        [CONVENTION_WINDOWS_ARM64] = "Windows ARM64",
    };
    return conventions[convention];
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

/* DESIGN: module.name on ELF and Mach-O, as docs/decisions.md settles, with
   the dots of the module path kept. Mach-O prefixes every C-level symbol
   with '_'. On COFF the symbol holds only letters, digits and '_': _A,
   each segment of the module path after its length, '_' and the function
   name. The lengths keep a '_' inside a name from producing the symbol of
   another pair. C reserves names that start with _A. */
bool mangle(struct text *out, enum target t, const char *module,
            const char *name)
{
    if (t >= TARGET_COUNT) {
        return false;
    }
    switch (infos[t].format) {
    case FORMAT_MACHO:
        text_appendf(out, "_%s.%s", module, name);
        break;
    case FORMAT_ELF:
        text_appendf(out, "%s.%s", module, name);
        break;
    case FORMAT_COFF:
        text_append(out, "_A");
        while (*module != '\0') {
            size_t n = strcspn(module, ".");
            text_appendf(out, "%zu%.*s", n, (int)n, module);
            module += n + (module[n] == '.');
        }
        text_appendf(out, "_%s", name);
        break;
    }
    return true;
}

/* DESIGN: a C symbol is the name of the item, with the `_` that Mach-O
   puts in front. A function of a class carries the name `T.f`, and a dot
   is no C identifier, so the symbol of an exported one is `T_f`. */
void c_symbol(struct text *out, enum target t, const char *name)
{
    size_t i;

    if (infos[t].format == FORMAT_MACHO) {
        text_append(out, "_");
    }
    for (i = 0; name[i] != '\0'; i++) {
        text_appendf(out, "%c", name[i] == '.' ? '_' : name[i]);
    }
}

/* DESIGN: llvm-mc leaves a label that starts with L out of a Mach-O symbol
   table. It leaves one that starts with .L out of ELF and COFF tables. */
void block_label(struct text *out, enum target t, const char *function,
                 size_t block)
{
    text_appendf(out, "%s%s.b%zu",
                 infos[t].format == FORMAT_MACHO ? "L" : ".L", function, block);
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
