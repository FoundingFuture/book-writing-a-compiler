#include <string.h>
#include "std.h"

struct anti_text anti_rt_text_from_c(const unsigned char *bytes)
{
    struct anti_text text;

    text.ptr = bytes;
    text.len = (int64_t)strlen((const char *)bytes);
    return text;
}
