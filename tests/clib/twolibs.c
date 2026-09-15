/* A C program that links two Anti libraries. */
#include <stdio.h>

#include "geo.h"
#include "other.h"

int main(void)
{
    Vec2 a = {1, 2};

    printf("%d\n", other_twice(geo_dot(a, a)));
    return 0;
}
