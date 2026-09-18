#include <stdio.h>
#include <stdlib.h>
#include "std.h"

/* DESIGN: the text of a failed assertion is built by the compiler and
   lives in the read-only data of the module, so this routine prints one
   string and ends the program. A release build removes every call of it
   and the strings with them. */
void anti_rt_assert_failed(const unsigned char *text, int64_t length)
{
    fflush(stdout);
    fwrite(text, 1, (size_t)length, stderr);
    fputc('\n', stderr);
    abort();
}
