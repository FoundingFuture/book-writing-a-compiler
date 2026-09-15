#include "check.h"
#include "target.h"
#include "text.h"

static void mangles(enum target t, const char *module, const char *name,
                    const char *expected)
{
    struct text out = {0};
    CHECK(mangle(&out, t, module, name));
    CHECK_STR(text_cstr(&out), expected);
    text_free(&out);
}

static void names_c(enum target t, const char *name, const char *expected)
{
    struct text out = {0};
    c_symbol(&out, t, name);
    CHECK_STR(text_cstr(&out), expected);
    text_free(&out);
}

static void labels(enum target t, const char *function, size_t block,
                   const char *expected)
{
    struct text out = {0};
    block_label(&out, t, function, block);
    CHECK_STR(text_cstr(&out), expected);
    text_free(&out);
}

void test_target(void)
{
    enum target t = TARGET_COUNT;

    /* A C function keeps the symbol a C compiler writes for it. */
    names_c(TARGET_MACOS_ARM64, "puts", "_puts");
    names_c(TARGET_LINUX_X86_64, "puts", "puts");
    names_c(TARGET_WINDOWS_ARM64, "__chkstk", "__chkstk");
    /* Block labels stay out of the symbol table. */
    labels(TARGET_MACOS_X86_64, "_main.scale", 2, "L_main.scale.b2");
    labels(TARGET_LINUX_ARM64, "main.scale", 0, ".Lmain.scale.b0");
    labels(TARGET_WINDOWS_X86_64, "_A4main_scale", 11, ".L_A4main_scale.b11");

    CHECK_STR(target_name(TARGET_MACOS_ARM64), "macos-arm64");
    CHECK_STR(target_name(TARGET_LINUX_X86_64), "linux-x86_64");
    CHECK_STR(target_name(TARGET_WINDOWS_ARM64), "windows-arm64");
    CHECK(target_from_name("linux-arm64", &t) && t == TARGET_LINUX_ARM64);
    CHECK(!target_from_name("macos-ppc", &t));

    mangles(TARGET_MACOS_ARM64, "return42", "main", "_return42.main");
    mangles(TARGET_MACOS_X86_64, "geometry", "length", "_geometry.length");
    mangles(TARGET_LINUX_ARM64, "geometry", "length", "geometry.length");
    mangles(TARGET_WINDOWS_X86_64, "geometry", "length", "_A8geometry_length");
    mangles(TARGET_WINDOWS_ARM64, "my_mod", "f_x", "_A6my_mod_f_x");
    mangles(TARGET_WINDOWS_ARM64, "a", "b_c", "_A1a_b_c");
    mangles(TARGET_WINDOWS_ARM64, "a_b", "c", "_A3a_b_c");
    /* A module path keeps its dots on ELF and Mach-O. COFF prefixes every
       segment with its length. */
    mangles(TARGET_LINUX_X86_64, "anti.collection.list", "push",
            "anti.collection.list.push");
    mangles(TARGET_WINDOWS_X86_64, "anti.collection.list", "push",
            "_A4anti10collection4list_push");
    mangles(TARGET_WINDOWS_ARM64, "anti.rt", "main", "_A4anti2rt_main");

    /* The target matrix. */
    CHECK(target_info(TARGET_LINUX_ARM64)->os == OS_LINUX);
    CHECK(target_info(TARGET_LINUX_ARM64)->arch == ARCH_ARM64);
    CHECK(target_info(TARGET_LINUX_ARM64)->format == FORMAT_ELF);
    CHECK(target_info(TARGET_LINUX_ARM64)->convention == CONVENTION_AAPCS64);
    CHECK(target_info(TARGET_MACOS_X86_64)->format == FORMAT_MACHO);
    CHECK(target_info(TARGET_MACOS_X86_64)->convention == CONVENTION_SYSV);
    CHECK(target_info(TARGET_MACOS_ARM64)->convention ==
          CONVENTION_APPLE_ARM64);
    CHECK(target_info(TARGET_WINDOWS_X86_64)->convention ==
          CONVENTION_WINDOWS_X64);
    CHECK(target_info(TARGET_WINDOWS_ARM64)->format == FORMAT_COFF);
    CHECK_STR(target_info(TARGET_WINDOWS_ARM64)->triple,
              "aarch64-pc-windows-msvc");
    CHECK_STR(target_info(TARGET_WINDOWS_X86_64)->object_suffix, ".obj");
    CHECK_STR(target_info(TARGET_WINDOWS_X86_64)->executable_suffix, ".exe");
    CHECK_STR(target_info(TARGET_LINUX_X86_64)->object_suffix, ".o");
    CHECK_STR(target_info(TARGET_LINUX_X86_64)->executable_suffix, "");
    CHECK_STR(object_format_name(FORMAT_MACHO), "Mach-O");
    CHECK_STR(convention_name(CONVENTION_WINDOWS_ARM64), "Windows ARM64");
}

void test_host_target(void)
{
    enum target t = TARGET_COUNT;
    CHECK(target_host(&t));
    CHECK(t != TARGET_COUNT);
}
