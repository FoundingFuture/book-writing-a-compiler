#include <stdio.h>
#include <stdlib.h>
#include "std.h"

/* DESIGN: anti.io writes through the C streams, so its output and the
   output of printf in one program keep their order. */
void anti_rt_write(int32_t stream, const unsigned char *bytes, size_t count)
{
    fwrite(bytes, 1, count, stream == 1 ? stdout : stderr);
}

void anti_rt_exit(int32_t status)
{
    fflush(stdout);
    fflush(stderr);
    exit(status);
}
