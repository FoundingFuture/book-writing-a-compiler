#include "std.h"

/* The licence text in a static library for C. A bundled runtime carries
   this object instead of the one of rt/license.c, because an archive has
   no anti_licenses notice to read. The notice of such a library comes
   from its package header, which `anti license --from-archive` reads. */
struct anti_text anti_rt_license_text(void)
{
    struct anti_text text;

    text.ptr = NULL;
    text.len = 0;
    return text;
}
