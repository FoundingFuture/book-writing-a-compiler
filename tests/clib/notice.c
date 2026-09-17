/* A bundled archive carries no notice, so the length is 0. */
#include <stdio.h>

#include "notice.h"

int main(void)
{
    printf("%d\n", notice_length());
    return 0;
}
