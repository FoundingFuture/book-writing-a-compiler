#include "linker.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool ends_with(const char *s, const char *suffix)
{
    size_t n = strlen(s);
    size_t m = strlen(suffix);

    return n >= m && strcmp(s + n - m, suffix) == 0;
}

bool link_is_input(const char *path)
{
    return ends_with(path, LINK_OBJECT_SUFFIX) ||
           ends_with(path, LINK_ARCHIVE_SUFFIX) ||
           ends_with(path, LINK_COFF_OBJECT_SUFFIX) ||
           ends_with(path, LINK_COFF_ARCHIVE_SUFFIX);
}

/* The object of the program, then the extra inputs. */
static void add_inputs(struct link_command *c, const struct link_inputs *in);

static void add(struct link_command *c, const char *arg)
{
    c->argv[c->argc++] = arg;
    c->argv[c->argc] = NULL;
}

/* The string of the next argument that the command builds. Add it with
   add once it is complete, because appending moves its bytes. */
static struct text *next(struct link_command *c)
{
    return &c->strings[c->string_count++];
}

void link_runtime_library(struct text *out, const char *runtime, enum target t)
{
    /* DESIGN: MSVC names a static library name.lib, and the other
       toolchains libname.a. */
    text_appendf(out, "%s/%s/%s/%s", runtime, RUNTIME_LIB_DIR, target_name(t),
                 target_info(t)->format == FORMAT_COFF ? "anti_rt.lib"
                                                       : "libanti_rt.a");
}

const char *const *link_crt_dirs(enum target t)
{
    /* DESIGN: Debian and Ubuntu install glibc in the multiarch directory,
       other distributions in /usr/lib64 or /usr/lib. */
    static const char *const x86_64[] = {
        "/usr/lib/x86_64-linux-gnu", "/usr/lib64", "/usr/lib", NULL
    };
    static const char *const arm64[] = {
        "/usr/lib/aarch64-linux-gnu", "/usr/lib64", "/usr/lib", NULL
    };
    return target_info(t)->arch == ARCH_ARM64 ? arm64 : x86_64;
}

/* The program of the linker: flavour of lld in its directory, or the
   platform linker. */
static const char *program(struct link_command *c, const struct link_inputs *in,
                           const char *flavour, const char *platform)
{
    struct text *path;

    if (in->linker == LINKER_PLATFORM) {
        return platform;
    }
    if (in->lld_dir == NULL) {
        return flavour;
    }
    path = next(c);
    text_appendf(path, "%s/%s", in->lld_dir, flavour);
    return text_cstr(path);
}

/* The start of a Mach-O link: -arch, the versions and libSystem's root.
   ld64 of Apple takes the SDK that xcrun names, and ld64.lld the stubs of
   the sysroot. */
static void macos_start(struct link_command *c, enum target t,
                        const struct link_inputs *in, bool dylib)
{
    const char *linker = program(c, in, "ld64.lld", "ld");
    struct text *version = next(c);

    text_appendf(version, "%d.%d", MACOS_MIN_MAJOR, MACOS_MIN_MINOR);
    add(c, linker);
    if (dylib) {
        add(c, "-dylib");
    }
    add(c, "-S");
    add(c, "-arch");
    add(c, target_info(t)->arch == ARCH_ARM64 ? "arm64" : "x86_64");
    add(c, "-platform_version");
    add(c, "macos");
    add(c, text_cstr(version));
    add(c, in->sdk_version);
    add(c, "-syslibroot");
    add(c, in->linker == LINKER_LLD ? in->sysroot : in->sdk_path);
}

static void macos(struct link_command *c, enum target t,
                  const struct link_inputs *in)
{
    struct text *library = next(c);

    link_runtime_library(library, in->runtime, t);
    macos_start(c, t, in, false);
    add(c, "-o");
    add(c, in->executable);
    add_inputs(c, in);
    add(c, text_cstr(library));
    add(c, "-lSystem");
}

/* DESIGN: ld.lld links a Linux program statically against the musl of
   the sysroot. The executable is position-independent and has no dynamic
   linker, so it runs on any Linux kernel. It drops the debug sections,
   which come from the musl of Alpine and weigh more than the program.
   antic writes no debug information of its own, so nothing of the
   program is lost, and the symbol table stays. */
static void linux_lld(struct link_command *c, enum target t,
                      const struct link_inputs *in)
{
    static const char *const before[] = {"rcrt1.o", "crti.o"};
    static const char *const after[] = {"libc.a", "libclang_rt.builtins.a",
                                        "crtn.o"};
    const char *linker = program(c, in, "ld.lld", "ld");
    struct text *library = next(c);
    size_t i;

    link_runtime_library(library, in->runtime, t);
    add(c, linker);
    add(c, "-static");
    add(c, "-pie");
    add(c, "--no-dynamic-linker");
    add(c, "--strip-debug");
    add(c, "-o");
    add(c, in->executable);
    for (i = 0; i < 2; i++) {
        struct text *file = next(c);
        text_appendf(file, "%s/usr/lib/%s", in->sysroot, before[i]);
        add(c, text_cstr(file));
    }
    add_inputs(c, in);
    add(c, text_cstr(library));
    for (i = 0; i < 3; i++) {
        struct text *file = next(c);
        text_appendf(file, "%s/usr/lib/%s", in->sysroot, after[i]);
        add(c, text_cstr(file));
    }
}

/* The library directories of lld-link: the CRT and the SDK that xwin
   writes into the sysroot. Without a sysroot lld-link reads LIB, as
   link.exe does. */
static void windows_libpaths(struct link_command *c, enum target t,
                             const struct link_inputs *in)
{
    static const char *const dirs[] = {"crt/lib", "sdk/lib/um",
                                       "sdk/lib/ucrt"};
    const char *arch = target_info(t)->arch == ARCH_ARM64 ? "aarch64"
                                                          : "x86_64";
    size_t i;

    if (in->linker != LINKER_LLD || in->sysroot == NULL) {
        return;
    }
    for (i = 0; i < 3; i++) {
        struct text *dir = next(c);
        text_appendf(dir, "/LIBPATH:%s/%s/%s", in->sysroot, dirs[i], arch);
        add(c, text_cstr(dir));
    }
}

/* GNU ld: a position-independent executable with the glibc start
   files, crti.o first and crtn.o last, and the C library. */
static void linux_ld(struct link_command *c, enum target t,
                     const struct link_inputs *in)
{
    struct text *interpreter = next(c);
    struct text *start = next(c);
    struct text *crti = next(c);
    struct text *library = next(c);
    struct text *search = next(c);
    struct text *crtn = next(c);

    text_appendf(interpreter, "--dynamic-linker=%s",
                 target_info(t)->arch == ARCH_ARM64
                     ? "/lib/ld-linux-aarch64.so.1"
                     : "/lib64/ld-linux-x86-64.so.2");
    text_appendf(start, "%s/Scrt1.o", in->crt_dir);
    text_appendf(crti, "%s/crti.o", in->crt_dir);
    link_runtime_library(library, in->runtime, t);
    text_appendf(search, "-L%s", in->crt_dir);
    text_appendf(crtn, "%s/crtn.o", in->crt_dir);
    add(c, "ld");
    add(c, "-pie");
    add(c, "--strip-debug");
    add(c, text_cstr(interpreter));
    add(c, "-o");
    add(c, in->executable);
    add(c, text_cstr(start));
    add(c, text_cstr(crti));
    add_inputs(c, in);
    add(c, text_cstr(library));
    add(c, text_cstr(search));
    add(c, "-lc");
    add(c, text_cstr(crtn));
}

/* DESIGN: link.exe or lld-link with the C runtime of the machine. The
   Universal CRT is a part of Windows since Windows 10, so ucrt.lib links
   against what the machine already holds. Only vcruntime, the support
   code of the compiler, comes in statically, because that one ships with
   Visual Studio rather than with Windows. The program then needs no
   redistributable, and it weighs 23,040 bytes instead of 91,136. */
static void windows(struct link_command *c, enum target t,
                    const struct link_inputs *in)
{
    const char *linker = program(c, in, "lld-link", "link.exe");
    struct text *output = next(c);
    struct text *library = next(c);

    text_appendf(output, "/OUT:%s", in->executable);
    link_runtime_library(library, in->runtime, t);
    add(c, linker);
    add(c, "/NOLOGO");
    add(c, "/debug:none");
    add(c, "/SUBSYSTEM:CONSOLE");
    add(c, target_info(t)->arch == ARCH_ARM64 ? "/MACHINE:ARM64"
                                              : "/MACHINE:X64");
    add(c, text_cstr(output));
    windows_libpaths(c, t, in);
    add_inputs(c, in);
    add(c, text_cstr(library));
    add(c, "msvcrt.lib");
    add(c, "libvcruntime.lib");
    add(c, "ucrt.lib");
    /* DESIGN: printf and its family are inline in the headers of the
       UCRT, and ucrt.lib exports none of them. A program that calls
       one through `extern fn` needs the definitions of the older
       form, which this library holds. */
    add(c, "legacy_stdio_definitions.lib");
}

static void add_inputs(struct link_command *c, const struct link_inputs *in)
{
    size_t i;

    add(c, in->object);
    for (i = 0; i < in->extra_count; i++) {
        add(c, in->extra[i]);
    }
}

void link_command(struct link_command *c, enum target t,
                  const struct link_inputs *in)
{
    memset(c, 0, sizeof *c);
    c->argv = calloc(LINK_FIXED_ARGS + in->extra_count + 1, sizeof *c->argv);
    if (c->argv == NULL) {
        fputs("antic: out of memory\n", stderr);
        exit(70);
    }
    switch (target_info(t)->os) {
    case OS_MACOS: macos(c, t, in); break;
    case OS_LINUX:
        if (in->linker == LINKER_LLD) {
            linux_lld(c, t, in);
        } else {
            linux_ld(c, t, in);
        }
        break;
    case OS_WINDOWS: windows(c, t, in); break;
    }
}

void link_command_free(struct link_command *c)
{
    size_t i;

    for (i = 0; i < c->string_count; i++) {
        text_free(&c->strings[i]);
    }
    free((void *)c->argv);
}

static struct link_command *start(struct link_command *c, size_t extra)
{
    memset(c, 0, sizeof *c);
    c->argv = calloc(LINK_FIXED_ARGS + extra + 1, sizeof *c->argv);
    if (c->argv == NULL) {
        fputs("antic: out of memory\n", stderr);
        exit(70);
    }
    return c;
}

void link_shared_command(struct link_command *c, enum target t,
                         const struct link_inputs *in,
                         const struct shared_options *s)
{
    struct text *library;

    start(c, in->extra_count);
    library = next(c);
    link_runtime_library(library, in->runtime, t);
    switch (target_info(t)->os) {
    case OS_MACOS: {
        struct text *install = next(c);
        struct text *compat = next(c);
        const char *base = strrchr(in->executable, '/');
        text_appendf(install, "@rpath/%s",
                     base != NULL ? base + 1 : in->executable);
        text_appendf(compat, "%s.0.0", s->major != NULL ? s->major : "");
        macos_start(c, t, in, true);
        add(c, "-o");
        add(c, in->executable);
        if (s->major != NULL) {
            add(c, "-install_name");
            add(c, text_cstr(install));
            add(c, "-compatibility_version");
            add(c, text_cstr(compat));
            add(c, "-current_version");
            add(c, s->version);
        }
        add_inputs(c, in);
        add(c, text_cstr(library));
        add(c, "-lSystem");
        break;
    }
    case OS_LINUX: {
        const char *linker = program(c, in, "ld.lld", "ld");
        struct text *search = next(c);
        const char *base = strrchr(in->executable, '/');
        text_appendf(search, "-L%s", in->crt_dir);
        add(c, linker);
        add(c, "-shared");
        add(c, "--strip-debug");
        add(c, "-o");
        add(c, in->executable);
        if (s->major != NULL) {
            add(c, "-soname");
            add(c, base != NULL ? base + 1 : in->executable);
        }
        add_inputs(c, in);
        add(c, text_cstr(library));
        if (in->linker == LINKER_PLATFORM) {
            add(c, text_cstr(search));
            add(c, "-lc");
        }
        break;
    }
    case OS_WINDOWS: {
        const char *linker = program(c, in, "lld-link", "link.exe");
        struct text *output = next(c);
        struct text *def = next(c);
        text_appendf(output, "/OUT:%s", in->executable);
        text_appendf(def, "/DEF:%s", s->def_file != NULL ? s->def_file : "");
        add(c, linker);
        add(c, "/NOLOGO");
        add(c, "/debug:none");
        add(c, "/DLL");
        add(c, target_info(t)->arch == ARCH_ARM64 ? "/MACHINE:ARM64"
                                                  : "/MACHINE:X64");
        add(c, text_cstr(output));
        add(c, text_cstr(def));
        windows_libpaths(c, t, in);
        add_inputs(c, in);
        add(c, text_cstr(library));
        add(c, "msvcrt.lib");
        add(c, "libvcruntime.lib");
        add(c, "ucrt.lib");
        add(c, "legacy_stdio_definitions.lib");
        break;
    }
    }
}

void archive_command(struct link_command *c, enum target t,
                     const char *llvm_ar, const char *archive,
                     const char *const *members, size_t count)
{
    static const char *const formats[] = {
        [FORMAT_ELF] = "--format=gnu",
        [FORMAT_MACHO] = "--format=darwin",
        [FORMAT_COFF] = "--format=coff",
    };
    size_t i;

    start(c, count);
    add(c, llvm_ar);
    add(c, formats[target_info(t)->format]);
    add(c, "rcs");
    add(c, archive);
    for (i = 0; i < count; i++) {
        add(c, members[i]);
    }
}

/* DESIGN: ld64 turns hidden symbols into local ones in a relocatable
   object unless -keep_private_externs is given. The runtime symbols stay
   global, so two bundled runtimes in one program are a duplicate. ld64.lld
   writes no relocatable object, so Mach-O always joins with ld64. */
void relocatable_command(struct link_command *c, enum target t,
                         const struct link_inputs *in, const char *output,
                         const char *const *objects, size_t count)
{
    size_t i;

    start(c, count);
    add(c, target_info(t)->os == OS_MACOS ? "ld"
                                          : program(c, in, "ld.lld", "ld"));
    add(c, "-r");
    if (target_info(t)->os == OS_MACOS) {
        add(c, "-keep_private_externs");
        add(c, "-arch");
        add(c, target_info(t)->arch == ARCH_ARM64 ? "arm64" : "x86_64");
    }
    add(c, "-o");
    add(c, output);
    for (i = 0; i < count; i++) {
        add(c, objects[i]);
    }
}

void link_line(struct text *out, enum target t, const char *library,
               const char *runtime, bool bundled)
{
    bool windows = target_info(t)->os == OS_WINDOWS;

    text_appendf(out, "%s main.c %s", windows ? "cl" : "cc", library);
    if (!bundled) {
        text_append(out, " ");
        link_runtime_library(out, runtime, t);
    }
    if (target_info(t)->os == OS_LINUX) {
        text_append(out, " -lpthread -lm");
    }
}
