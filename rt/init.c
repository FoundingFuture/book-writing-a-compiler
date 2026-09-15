#include "rt.h"

static int ready;

void anti_rt_init(void)
{
    ready = 1;
}

int anti_rt_ready(void)
{
    return ready;
}
