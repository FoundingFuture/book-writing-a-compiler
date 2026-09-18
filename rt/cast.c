#include <stdio.h>
#include <stdlib.h>
#include "std.h"

/* DESIGN: `p as *T` on a class pointer traps when the object is of no
   class below T. The compiler passes the name of T, which lives in the
   read-only data of the module. This routine prints one message and ends
   the program. `p as? *T` gives null instead and never calls it. */
void anti_rt_cast_failed(const unsigned char *name, int64_t length)
{
    fflush(stdout);
    fputs("cast failed: the object is no ", stderr);
    fwrite(name, 1, (size_t)length, stderr);
    fputc('\n', stderr);
    abort();
}
