#include "check.h"
#include <stdio.h>
#include <string.h>
#include "linker.h"
#include "notice.h"
#include "text.h"

static const char *const extra_unix[] = {"shapes.o", "libm.a"};
static const char *const extra_windows[] = {"shapes.obj"};

static const struct link_inputs unix_inputs = {
    "prog.o", "prog", "/rt", "/sdk", "15.4", "/usr/lib/x86_64-linux-gnu",
    NULL, 0, LINKER_PLATFORM, NULL, NULL
};

static const struct link_inputs windows_inputs = {
    "prog.obj", "prog.exe", "C:/rt", NULL, NULL, NULL, NULL, 0,
    LINKER_PLATFORM, NULL, NULL
};

static const struct link_inputs extra_inputs = {
    "prog.o", "prog", "/rt", "/sdk", "15.4", "/usr/lib/aarch64-linux-gnu",
    extra_unix, 2, LINKER_PLATFORM, NULL, NULL
};

static const struct link_inputs extra_windows_inputs = {
    "prog.obj", "prog.exe", "C:/rt", NULL, NULL, NULL, extra_windows, 1,
    LINKER_PLATFORM, NULL, NULL
};

/* lld of the runtime archive with the sysroot of the target. */
static const struct link_inputs lld_inputs = {
    "prog.o", "prog", "/rt", NULL, "26.5", NULL, extra_unix, 1, LINKER_LLD,
    "/rt/sysroot/t", "/rt/bin"
};

static const struct link_inputs lld_windows_inputs = {
    "prog.obj", "prog.exe", "/rt", NULL, NULL, NULL, NULL, 0, LINKER_LLD,
    "/rt/sysroot/t", "/rt/bin"
};

/* Build the command line of target t and compare it, joined by spaces. */
static void links(enum target t, const struct link_inputs *in,
                  const char *expected)
{
    struct link_command c;
    struct text joined = {0};
    size_t i;

    link_command(&c, t, in);
    for (i = 0; i < c.argc; i++) {
        text_appendf(&joined, "%s%s", i > 0 ? " " : "", c.argv[i]);
    }
    CHECK(c.argv[c.argc] == NULL);
    CHECK_STR(text_cstr(&joined), expected);
    text_free(&joined);
    link_command_free(&c);
}

/* rt/start.c names the runtime entry in the form that mangle writes for
   each object format, so that the link finds it. */
static void runtime_entry(void)
{
    static const enum target formats[] = {
        TARGET_LINUX_X86_64, TARGET_MACOS_ARM64, TARGET_WINDOWS_X86_64
    };
    struct text start = {0};
    char buffer[4096];
    FILE *f = fopen(ANTIC_SOURCE_DIR "/rt/start.c", "rb");
    size_t n;
    size_t i;

    CHECK(f != NULL);
    while (f != NULL && (n = fread(buffer, 1, sizeof buffer, f)) > 0) {
        text_append_bytes(&start, buffer, n);
    }
    if (f != NULL) {
        fclose(f);
    }
    for (i = 0; i < sizeof formats / sizeof formats[0]; i++) {
        struct text symbol = {0};
        struct text quoted = {0};
        mangle(&symbol, formats[i], RUNTIME_MODULE, RUNTIME_ENTRY);
        /* COFF symbols are C identifiers, and the others labels. */
        text_appendf(&quoted,
                     target_info(formats[i])->format == FORMAT_COFF
                         ? "%s(struct anti_slice args"
                         : "\"%s\"",
                     text_cstr(&symbol));
        CHECK(strstr(text_cstr(&start), text_cstr(&quoted)) != NULL);
        text_free(&symbol);
        text_free(&quoted);
    }
    text_free(&start);
}

/* rt/license.c finds the notice of the program by the markers that the
   emitter writes, spelled as C strings. */
static void runtime_markers(void)
{
    struct text source = {0};
    char buffer[4096];
    FILE *f = fopen(ANTIC_SOURCE_DIR "/rt/license.c", "rb");
    size_t n;

    CHECK(f != NULL);
    while (f != NULL && (n = fread(buffer, 1, sizeof buffer, f)) > 0) {
        text_append_bytes(&source, buffer, n);
    }
    if (f != NULL) {
        fclose(f);
    }
    CHECK(strlen(NOTICE_BEGIN) > 1 && strlen(NOTICE_END) > 1);
    snprintf(buffer, sizeof buffer, "\"%.*s\\n\"",
             (int)strlen(NOTICE_BEGIN) - 1, NOTICE_BEGIN);
    CHECK(strstr(text_cstr(&source), buffer) != NULL);
    snprintf(buffer, sizeof buffer, "\"%.*s\\n\"",
             (int)strlen(NOTICE_END) - 1, NOTICE_END);
    CHECK(strstr(text_cstr(&source), buffer) != NULL);
    text_free(&source);
}

/* The runtime and the standard library carry the 0BSD licence, so a
   program owes no attribution for them. */
static void runtime_licence(const char *path)
{
    char buffer[256] = "";
    FILE *f = fopen(path, "rb");
    size_t n = 0;

    CHECK(f != NULL);
    if (f != NULL) {
        n = fread(buffer, 1, sizeof buffer - 1, f);
        fclose(f);
    }
    buffer[n] = '\0';
    CHECK(strncmp(buffer, "BSD Zero Clause License", 23) == 0);
}

static void joined(const struct link_command *c, const char *expected)
{
    struct text out = {0};
    size_t i;

    for (i = 0; i < c->argc; i++) {
        text_appendf(&out, "%s%s", i > 0 ? " " : "", c->argv[i]);
    }
    CHECK(c->argv[c->argc] == NULL);
    CHECK_STR(text_cstr(&out), expected);
    text_free(&out);
}

static void shared(enum target t, const struct link_inputs *in,
                   const struct shared_options *s, const char *expected)
{
    struct link_command c;

    link_shared_command(&c, t, in, s);
    joined(&c, expected);
    link_command_free(&c);
}

/* Libraries for C. llvm-ar writes an archive in the format of the object
   files, and the platform linker a shared library. The objects of a
   bundled runtime join into one relocatable object. */
static void libraries(void)
{
    static const char *const members[] = {"geo.o", "geo.package.o"};
    static const char *const joined_inputs[] = {"geo.o", "init.c.o",
                                                "utf.c.o"};
    struct shared_options none = {NULL, NULL, NULL};
    struct shared_options versioned = {NULL, "1", "1.2.4"};
    struct shared_options def = {"geo.def", NULL, NULL};
    struct link_inputs in = unix_inputs;
    struct link_inputs win = windows_inputs;
    struct link_command c;
    struct text line = {0};

    archive_command(&c, TARGET_MACOS_ARM64, "/tc/llvm-ar", "libgeo.a",
                    members, 2);
    joined(&c, "/tc/llvm-ar --format=darwin rcs libgeo.a geo.o "
               "geo.package.o");
    link_command_free(&c);
    archive_command(&c, TARGET_LINUX_ARM64, "llvm-ar", "libgeo.a", members, 1);
    joined(&c, "llvm-ar --format=gnu rcs libgeo.a geo.o");
    link_command_free(&c);
    archive_command(&c, TARGET_WINDOWS_X86_64, "llvm-ar", "geo.lib", members,
                    1);
    joined(&c, "llvm-ar --format=coff rcs geo.lib geo.o");
    link_command_free(&c);

    in.object = "geo.o";
    in.executable = "libgeo.dylib";
    shared(TARGET_MACOS_ARM64, &in, &none,
           "ld -dylib -arch arm64 -platform_version macos 11.0 15.4 "
           "-syslibroot /sdk -o libgeo.dylib geo.o "
           "/rt/lib/macos-arm64/libanti_rt.a -lSystem");
    shared(TARGET_MACOS_X86_64, &in, &versioned,
           "ld -dylib -arch x86_64 -platform_version macos 11.0 15.4 "
           "-syslibroot /sdk -o libgeo.dylib -install_name @rpath/libgeo.dylib "
           "-compatibility_version 1.0.0 -current_version 1.2.4 geo.o "
           "/rt/lib/macos-x86_64/libanti_rt.a -lSystem");
    in.executable = "libgeo.so.1";
    shared(TARGET_LINUX_X86_64, &in, &versioned,
           "ld -shared -o libgeo.so.1 -soname libgeo.so.1 geo.o "
           "/rt/lib/linux-x86_64/libanti_rt.a -L/usr/lib/x86_64-linux-gnu -lc");
    in.executable = "libgeo.so";
    shared(TARGET_LINUX_ARM64, &in, &none,
           "ld -shared -o libgeo.so geo.o /rt/lib/linux-arm64/libanti_rt.a "
           "-L/usr/lib/x86_64-linux-gnu -lc");
    win.object = "geo.obj";
    win.executable = "geo.dll";
    shared(TARGET_WINDOWS_ARM64, &win, &def,
           "link.exe /NOLOGO /DLL /MACHINE:ARM64 /OUT:geo.dll /DEF:geo.def "
           "geo.obj C:/rt/lib/windows-arm64/anti_rt.lib libcmt.lib "
           "libvcruntime.lib libucrt.lib");

    relocatable_command(&c, TARGET_MACOS_ARM64, &in, "joined.o",
                        joined_inputs, 3);
    joined(&c, "ld -r -keep_private_externs -arch arm64 -o joined.o geo.o "
               "init.c.o utf.c.o");
    link_command_free(&c);
    relocatable_command(&c, TARGET_LINUX_X86_64, &in, "joined.o",
                        joined_inputs, 3);
    joined(&c, "ld -r -o joined.o geo.o init.c.o utf.c.o");
    link_command_free(&c);

    /* lld links the libraries too. ld64.lld writes no relocatable object,
       so a bundled runtime on macOS still joins with ld -r. A shared
       library on Linux links no C library, whose symbols the process
       provides. */
    in = lld_inputs;
    in.object = "geo.o";
    in.extra_count = 0;
    in.executable = "libgeo.dylib";
    shared(TARGET_MACOS_ARM64, &in, &none,
           "/rt/bin/ld64.lld -dylib -arch arm64 -platform_version macos 11.0 "
           "26.5 -syslibroot /rt/sysroot/t -o libgeo.dylib geo.o "
           "/rt/lib/macos-arm64/libanti_rt.a -lSystem");
    in.executable = "libgeo.so.1";
    shared(TARGET_LINUX_X86_64, &in, &versioned,
           "/rt/bin/ld.lld -shared -o libgeo.so.1 -soname libgeo.so.1 geo.o "
           "/rt/lib/linux-x86_64/libanti_rt.a");
    win = lld_windows_inputs;
    win.object = "geo.obj";
    win.executable = "geo.dll";
    shared(TARGET_WINDOWS_ARM64, &win, &def,
           "/rt/bin/lld-link /NOLOGO /DLL /MACHINE:ARM64 /OUT:geo.dll "
           "/DEF:geo.def /LIBPATH:/rt/sysroot/t/crt/lib/aarch64 "
           "/LIBPATH:/rt/sysroot/t/sdk/lib/um/aarch64 "
           "/LIBPATH:/rt/sysroot/t/sdk/lib/ucrt/aarch64 geo.obj "
           "/rt/lib/windows-arm64/anti_rt.lib libcmt.lib libvcruntime.lib "
           "libucrt.lib");
    relocatable_command(&c, TARGET_MACOS_ARM64, &in, "joined.o",
                        joined_inputs, 3);
    joined(&c, "ld -r -keep_private_externs -arch arm64 -o joined.o geo.o "
               "init.c.o utf.c.o");
    link_command_free(&c);
    relocatable_command(&c, TARGET_LINUX_ARM64, &in, "joined.o",
                        joined_inputs, 3);
    joined(&c, "/rt/bin/ld.lld -r -o joined.o geo.o init.c.o utf.c.o");
    link_command_free(&c);

    link_line(&line, TARGET_LINUX_X86_64, "libgeo.a", "/rt", false);
    CHECK_STR(text_cstr(&line), "cc main.c libgeo.a "
                                "/rt/lib/linux-x86_64/libanti_rt.a -lpthread -lm");
    text_free(&line);
    link_line(&line, TARGET_MACOS_ARM64, "libgeo.a", "/rt", false);
    CHECK_STR(text_cstr(&line), "cc main.c libgeo.a "
                                "/rt/lib/macos-arm64/libanti_rt.a");
    text_free(&line);
    link_line(&line, TARGET_WINDOWS_X86_64, "geo.lib", "C:/rt", true);
    CHECK_STR(text_cstr(&line), "cl main.c geo.lib");
    text_free(&line);
}

void test_link(void)
{
    libraries();
    runtime_entry();
    runtime_markers();
    runtime_licence(ANTIC_SOURCE_DIR "/rt/LICENSE");
    runtime_licence(ANTIC_SOURCE_DIR "/std/LICENSE");
    links(TARGET_MACOS_ARM64, &unix_inputs,
          "ld -arch arm64 -platform_version macos 11.0 15.4 -syslibroot /sdk "
          "-o prog prog.o /rt/lib/macos-arm64/libanti_rt.a -lSystem");
    links(TARGET_MACOS_X86_64, &unix_inputs,
          "ld -arch x86_64 -platform_version macos 11.0 15.4 -syslibroot /sdk "
          "-o prog prog.o /rt/lib/macos-x86_64/libanti_rt.a -lSystem");
    links(TARGET_LINUX_X86_64, &unix_inputs,
          "ld -pie --dynamic-linker=/lib64/ld-linux-x86-64.so.2 -o prog "
          "/usr/lib/x86_64-linux-gnu/Scrt1.o /usr/lib/x86_64-linux-gnu/crti.o "
          "prog.o /rt/lib/linux-x86_64/libanti_rt.a "
          "-L/usr/lib/x86_64-linux-gnu -lc /usr/lib/x86_64-linux-gnu/crtn.o");
    links(TARGET_LINUX_ARM64, &unix_inputs,
          "ld -pie --dynamic-linker=/lib/ld-linux-aarch64.so.1 -o prog "
          "/usr/lib/x86_64-linux-gnu/Scrt1.o /usr/lib/x86_64-linux-gnu/crti.o "
          "prog.o /rt/lib/linux-arm64/libanti_rt.a "
          "-L/usr/lib/x86_64-linux-gnu -lc /usr/lib/x86_64-linux-gnu/crtn.o");
    links(TARGET_WINDOWS_X86_64, &windows_inputs,
          "link.exe /NOLOGO /SUBSYSTEM:CONSOLE /MACHINE:X64 /OUT:prog.exe "
          "prog.obj C:/rt/lib/windows-x86_64/anti_rt.lib libcmt.lib "
          "libvcruntime.lib libucrt.lib");
    links(TARGET_WINDOWS_ARM64, &windows_inputs,
          "link.exe /NOLOGO /SUBSYSTEM:CONSOLE /MACHINE:ARM64 /OUT:prog.exe "
          "prog.obj C:/rt/lib/windows-arm64/anti_rt.lib libcmt.lib "
          "libvcruntime.lib libucrt.lib");

    /* ld64.lld links with the .tbd stubs of the sysroot, and ld.lld with
       musl as a static position-independent executable. lld-link takes the
       libraries of the sysroot, or of the LIB variable without one. */
    links(TARGET_MACOS_X86_64, &lld_inputs,
          "/rt/bin/ld64.lld -arch x86_64 -platform_version macos 11.0 26.5 "
          "-syslibroot /rt/sysroot/t -o prog prog.o shapes.o "
          "/rt/lib/macos-x86_64/libanti_rt.a -lSystem");
    links(TARGET_LINUX_ARM64, &lld_inputs,
          "/rt/bin/ld.lld -static -pie --no-dynamic-linker -o prog "
          "/rt/sysroot/t/usr/lib/rcrt1.o /rt/sysroot/t/usr/lib/crti.o prog.o "
          "shapes.o /rt/lib/linux-arm64/libanti_rt.a "
          "/rt/sysroot/t/usr/lib/libc.a "
          "/rt/sysroot/t/usr/lib/libclang_rt.builtins.a "
          "/rt/sysroot/t/usr/lib/crtn.o");
    links(TARGET_WINDOWS_X86_64, &lld_windows_inputs,
          "/rt/bin/lld-link /NOLOGO /SUBSYSTEM:CONSOLE /MACHINE:X64 "
          "/OUT:prog.exe /LIBPATH:/rt/sysroot/t/crt/lib/x86_64 "
          "/LIBPATH:/rt/sysroot/t/sdk/lib/um/x86_64 "
          "/LIBPATH:/rt/sysroot/t/sdk/lib/ucrt/x86_64 prog.obj "
          "/rt/lib/windows-x86_64/anti_rt.lib libcmt.lib libvcruntime.lib "
          "libucrt.lib");
    {
        struct link_inputs no_sysroot = lld_windows_inputs;
        no_sysroot.sysroot = NULL;
        no_sysroot.lld_dir = NULL;
        links(TARGET_WINDOWS_ARM64, &no_sysroot,
              "lld-link /NOLOGO /SUBSYSTEM:CONSOLE /MACHINE:ARM64 "
              "/OUT:prog.exe prog.obj /rt/lib/windows-arm64/anti_rt.lib "
              "libcmt.lib libvcruntime.lib libucrt.lib");
    }

    /* Object files and archives from the command line follow the object
       of the program, before the runtime library. */
    links(TARGET_MACOS_ARM64, &extra_inputs,
          "ld -arch arm64 -platform_version macos 11.0 15.4 -syslibroot /sdk "
          "-o prog prog.o shapes.o libm.a /rt/lib/macos-arm64/libanti_rt.a "
          "-lSystem");
    links(TARGET_LINUX_ARM64, &extra_inputs,
          "ld -pie --dynamic-linker=/lib/ld-linux-aarch64.so.1 -o prog "
          "/usr/lib/aarch64-linux-gnu/Scrt1.o /usr/lib/aarch64-linux-gnu/crti.o "
          "prog.o shapes.o libm.a /rt/lib/linux-arm64/libanti_rt.a "
          "-L/usr/lib/aarch64-linux-gnu -lc /usr/lib/aarch64-linux-gnu/crtn.o");
    links(TARGET_WINDOWS_ARM64, &extra_windows_inputs,
          "link.exe /NOLOGO /SUBSYSTEM:CONSOLE /MACHINE:ARM64 /OUT:prog.exe "
          "prog.obj shapes.obj C:/rt/lib/windows-arm64/anti_rt.lib libcmt.lib "
          "libvcruntime.lib libucrt.lib");

    /* The directories that may hold the start files of glibc, in order. */
    CHECK_STR(link_crt_dirs(TARGET_LINUX_ARM64)[0], "/usr/lib/aarch64-linux-gnu");
    CHECK_STR(link_crt_dirs(TARGET_LINUX_ARM64)[1], "/usr/lib64");
    CHECK_STR(link_crt_dirs(TARGET_LINUX_ARM64)[2], "/usr/lib");
    CHECK(link_crt_dirs(TARGET_LINUX_ARM64)[3] == NULL);
    CHECK_STR(link_crt_dirs(TARGET_LINUX_X86_64)[0], "/usr/lib/x86_64-linux-gnu");
}
