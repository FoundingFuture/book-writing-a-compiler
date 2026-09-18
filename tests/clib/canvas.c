/* A C program that builds Anti classes, calls through the table and ends
   them. The header of com.example.canvas gives the layout, the table type
   and one prototype per public function. */
#include <stdio.h>
#include <stdlib.h>

#include "canvas.h"

int main(void)
{
    Square *s = (Square *)malloc(sizeof *s);
    Shape *base;

    anti_Square_init(s);
    s->side = 4;
    Shape_move(&s->base, 3, 5);
    base = &s->base;

    printf("%d %d %d\n", base->x, base->y, anti_Shape_area(base));
    printf("%d %d\n", Square_area(s), Shape_area(base));
    /* An interface sub-object is a field, so C takes its address and
       calls through the table that belongs to it. */
    printf("%d\n", anti_Ink_colour(anti_Square_as_Ink(s)));
    anti_Square_delete(s);
    return 0;
}
