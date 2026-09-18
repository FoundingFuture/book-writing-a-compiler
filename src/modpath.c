#include "modpath.h"

#include <stdio.h>
#include <string.h>

#include "lexer.h"

static bool is_lower_identifier(const char *s, size_t n)
{
    size_t i;

    if (n == 0 || (s[0] >= '0' && s[0] <= '9')) {
        return false;
    }
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return true;
}

/* The part of source under root, or NULL when root does not hold it. */
static const char *under(const char *source, const char *root)
{
    size_t n = strlen(root);

    while (n > 0 && root[n - 1] == '/') {
        n--;
    }
    if (n == 0 || strncmp(source, root, n) != 0 || source[n] != '/') {
        return NULL;
    }
    return source + n + 1;
}

bool module_path_of_source(const char *source, const char *const *roots,
                           size_t root_count, struct text *out, char *error,
                           size_t error_size)
{
    const char *rest = NULL;
    const char *dot = strrchr(source, '.');
    const char *end = dot != NULL && strcmp(dot, ".anti") == 0
                          ? dot
                          : source + strlen(source);
    size_t i;

    for (i = 0; i < root_count && rest == NULL; i++) {
        rest = under(source, roots[i]);
    }
    if (rest == NULL) {
        const char *slash = strrchr(source, '/');
        rest = slash != NULL ? slash + 1 : source;
    }
    while (rest < end) {
        const char *slash = memchr(rest, '/', (size_t)(end - rest));
        size_t n = slash != NULL ? (size_t)(slash - rest)
                                 : (size_t)(end - rest);
        if (!is_lower_identifier(rest, n)) {
            snprintf(error, error_size,
                     "`%.*s` in %s is not a lowercase identifier", (int)n,
                     rest, source);
            return false;
        }
        if (lexer_is_keyword(rest, n)) {
            snprintf(error, error_size, "`%.*s` in %s is a keyword", (int)n,
                     rest, source);
            return false;
        }
        text_appendf(out, "%s%.*s", out->length > 0 ? "." : "", (int)n, rest);
        rest += n + 1;
    }
    return true;
}

bool module_path_reserved(const char *path)
{
    return strncmp(path, "anti", 4) == 0 && (path[4] == '\0' || path[4] == '.');
}

size_t module_path_segments(const char *path)
{
    size_t n = 1;

    for (; *path != '\0'; path++) {
        n += *path == '.';
    }
    return n;
}

const char *module_path_last(const char *path)
{
    const char *dot = strrchr(path, '.');
    return dot != NULL ? dot + 1 : path;
}
