#include <string.h>
#include "std.h"

/* The notice that antic links into every executable and shared library. */
extern const char anti_licenses[];

static const char begin[] = "ANTI_LICENSES_BEGIN\n";
static const char end[] = "ANTI_LICENSES_END\n";

struct anti_text anti_rt_license_text(void)
{
    struct anti_text text;
    const char *from = anti_licenses;
    const char *to;

    if (strncmp(from, begin, sizeof begin - 1) == 0) {
        from += sizeof begin - 1;
    }
    to = strstr(from, end);
    text.ptr = (const unsigned char *)from;
    text.len = to != NULL ? (int64_t)(to - from) : (int64_t)strlen(from);
    return text;
}
