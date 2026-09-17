/* A C program that calls every export fn of the library geo, with structs
   and unions by value, and prints the results. */
#include <stdio.h>

#include "geo.h"

static int32_t square(int32_t v)
{
    return v * v;
}

int main(void)
{
    Vec2 a = {3, 4};
    Vec2 b = {5, -6};
    Vec2 scaled = geo_scale(a, 10);
    Num n;
    Num half;
    Flags flags = {0, 9};

    n.d = 7.0;
    half = geo_half(n);
    printf("%d\n", geo_dot(a, b));
    printf("%d %d\n", scaled.x, scaled.y);
    printf("%g\n", half.d);
    printf("%u %u\n", geo_layer(&flags), flags.visible);
    printf("%d\n", geo_apply(square, a));
    printf("%d\n", (int)LAYERS);
    return 0;
}
