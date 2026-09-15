#include "notice.h"

#include <string.h>

static const char *or_empty(const char *s)
{
    return s != NULL ? s : "";
}

/* Whether a package before index i has the name of package i. */
static bool repeated(const struct package *const *packages, size_t i)
{
    size_t j;

    for (j = 0; j < i; j++) {
        if (strcmp(or_empty(packages[j]->name),
                   or_empty(packages[i]->name)) == 0) {
            return true;
        }
    }
    return false;
}

void notice_text(struct text *out, const struct package *const *packages,
                 size_t count)
{
    size_t i;
    size_t j;
    size_t k;

    text_append(out, NOTICE_BEGIN);
    for (i = 0; i < count; i++) {
        const struct package *p = packages[i];
        if (repeated(packages, i)) {
            continue;
        }
        text_appendf(out, "package %s %s %s\n", or_empty(p->name),
                     or_empty(p->version), or_empty(p->license));
        for (k = 0; k < p->attribution_count; k++) {
            text_appendf(out, "attribution %s\n", p->attribution[k]);
        }
    }
    for (i = 0; i < count; i++) {
        const char *text = or_empty(packages[i]->license_text);
        if (text[0] == '\0' || repeated(packages, i)) {
            continue;
        }
        for (j = 0; j < i; j++) {
            if (strcmp(or_empty(packages[j]->license_text), text) == 0) {
                break;
            }
        }
        if (j < i) {
            continue;
        }
        text_append(out, "text for");
        for (j = i; j < count; j++) {
            if (strcmp(or_empty(packages[j]->license_text), text) == 0 &&
                !repeated(packages, j)) {
                text_appendf(out, " %s", or_empty(packages[j]->name));
            }
        }
        text_appendf(out, "\n%s", text);
        if (text[strlen(text) - 1] != '\n') {
            text_append(out, "\n");
        }
    }
    text_append(out, NOTICE_END);
}
