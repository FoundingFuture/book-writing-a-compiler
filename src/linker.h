#ifndef ANTIC_LINKER_H
#define ANTIC_LINKER_H

#include <stdbool.h>
#include <stddef.h>

#include "target.h"
#include "text.h"

/* The runtime archive keeps the libraries of each target in
   <runtime>/RUNTIME_LIB_DIR/<target>/. The sysroot that lld links against
   is <runtime>/RUNTIME_SYSROOT_DIR/<target>/, and the pinned LLVM tools
   are in <runtime>/RUNTIME_BIN_DIR/. A macOS sysroot names the version of
   its SDK in the file SYSROOT_SDK_VERSION. */
#define RUNTIME_LIB_DIR "lib"
#define RUNTIME_SYSROOT_DIR "sysroot"
#define RUNTIME_BIN_DIR "bin"
/* The library files of the standard library, a search root of antic. */
#define RUNTIME_STD_DIR "std"
/* The object beside the runtime library that a bundled archive carries
   instead of the licence text of rt/license.c. */
#define RUNTIME_LICENSE_STUB "anti_rt_license_stub"
#define SYSROOT_SDK_VERSION "sdk-version"

/* DESIGN: lld of the pinned LLVM release links for every target, and
   --linker platform selects the linker of the host's own toolchain. */
enum linker { LINKER_LLD, LINKER_PLATFORM };

/* The files of a link and the facts about the host that the command line
   needs. */
struct link_inputs {
    const char *object;
    const char *executable;
    const char *runtime;        /* the directory of the runtime archive */
    const char *sdk_path;       /* macOS: xcrun --show-sdk-path */
    const char *sdk_version;    /* macOS: xcrun --show-sdk-version */
    const char *crt_dir;        /* Linux: the directory that holds Scrt1.o */
    const char *const *extra;   /* object files and archives to link */
    size_t extra_count;
    enum linker linker;
    const char *sysroot;        /* lld: <runtime>/sysroot/<target> */
    const char *lld_dir;        /* lld: its directory, or NULL for PATH */
};

/* The suffixes of the object files and archives that antic passes to the
   linker. */
#define LINK_OBJECT_SUFFIX ".o"
#define LINK_ARCHIVE_SUFFIX ".a"
#define LINK_COFF_OBJECT_SUFFIX ".obj"
#define LINK_COFF_ARCHIVE_SUFFIX ".lib"

enum { LINK_FIXED_ARGS = 24, LINK_MAX_STRINGS = 12 };

/* A linker command line. The arguments that it builds live in strings,
   and argv holds the fixed arguments and every extra input. */
struct link_command {
    const char **argv;
    size_t argc;
    struct text strings[LINK_MAX_STRINGS];
    size_t string_count;
};

/* Build the command line that links in->object for target t. argv ends
   with NULL. */
void link_command(struct link_command *c, enum target t,
                  const struct link_inputs *in);
void link_command_free(struct link_command *c);

/* The version facts of a shared library, NULL when absent. The .def file
   names the exports of a DLL. */
struct shared_options {
    const char *def_file;
    const char *major;          /* --soname: the compatibility version */
    const char *version;        /* --soname: the full version */
};

/* Build the command line of a shared library of in->object at
   in->executable. */
void link_shared_command(struct link_command *c, enum target t,
                         const struct link_inputs *in,
                         const struct shared_options *s);

/* Build the llvm-ar command line that writes archive from the members. */
void archive_command(struct link_command *c, enum target t,
                     const char *llvm_ar, const char *archive,
                     const char *const *members, size_t count);

/* Build the command line that joins objects into one relocatable object
   for a bundled runtime, with the linker of in. */
void relocatable_command(struct link_command *c, enum target t,
                         const struct link_inputs *in, const char *output,
                         const char *const *objects, size_t count);

/* Append the command line that links a C program main.c with the static
   library, as antic prints it. A bundled runtime needs no runtime library. */
void link_line(struct text *out, enum target t, const char *library,
               const char *runtime, bool bundled);

/* Whether path names an object file or an archive for the linker. */
bool link_is_input(const char *path);

/* Append the path of the runtime library of target t below runtime. */
void link_runtime_library(struct text *out, const char *runtime, enum target t);

/* The directories that may hold the glibc start files for a Linux target,
   in the order of search, ending with NULL. */
const char *const *link_crt_dirs(enum target t);

#endif
