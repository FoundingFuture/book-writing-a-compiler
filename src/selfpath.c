/* readlink, stat and PATH_MAX are POSIX, outside the C11 library. */
#define _POSIX_C_SOURCE 200809L

/* DESIGN: an installed antic sits in bin/ of the runtime archive, so the
   directory above it holds lib/, std/ and sysroot/. Asking the system
   where the executable is lets a program compile without --runtime, and
   each system answers a different way. */

#include "selfpath.h"

#include <string.h>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

#if defined(_WIN32)

#include <stdlib.h>
#include <windows.h>

static bool self_path(struct text *out)
{
    wchar_t wide[MAX_PATH];
    char *path;
    DWORD length = GetModuleFileNameW(NULL, wide, MAX_PATH);
    int count;

    if (length == 0 || length >= MAX_PATH) {
        return false;
    }
    count = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    if (count <= 0) {
        return false;
    }
    path = malloc((size_t)count);
    if (path == NULL) {
        return false;
    }
    if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, path, count, NULL,
                            NULL) <= 0) {
        free(path);
        return false;
    }
    text_append(out, path);
    free(path);
    return true;
}

#elif defined(__APPLE__)

#include <mach-o/dyld.h>
#include <stdlib.h>

static bool self_path(struct text *out)
{
    uint32_t size = 0;
    char *path;

    _NSGetExecutablePath(NULL, &size);
    if (size == 0) {
        return false;
    }
    path = malloc(size);
    if (path == NULL || _NSGetExecutablePath(path, &size) != 0) {
        free(path);
        return false;
    }
    text_append(out, path);
    free(path);
    return true;
}

#else

#include <limits.h>
#include <unistd.h>

static bool self_path(struct text *out)
{
    char path[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", path, sizeof path - 1);

    if (length <= 0) {
        return false;
    }
    path[length] = '\0';
    text_append(out, path);
    return true;
}

#endif

#if defined(_WIN32)

bool directory_exists(const char *path)
{
    DWORD attributes = GetFileAttributesA(path);

    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

#else

bool directory_exists(const char *path)
{
    struct stat info;

    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

#endif

bool self_directory(struct text *out)
{
    struct text path = {0};
    const char *bytes;
    size_t cut;
    size_t i;

    if (!self_path(&path)) {
        text_free(&path);
        return false;
    }
    bytes = text_cstr(&path);
    cut = 0;
    for (i = 0; i < path.length; i++) {
        if (bytes[i] == '/' || bytes[i] == '\\') {
            cut = i;
        }
    }
    if (cut == 0) {
        text_free(&path);
        return false;
    }
    text_append_bytes(out, bytes, cut);
    text_free(&path);
    return true;
}
