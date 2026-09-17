/* The header of com.example.shapes compiles as C11, with the sizes that
   chapter 25 states for a packed and an aligned struct. */
#include <stdalign.h>

#include "shapes.h"

_Static_assert(sizeof(Tight) == 5, "Tight has no padding");
_Static_assert(alignof(Wide) == 16, "Wide is aligned to 16");

int main(void)
{
    return 0;
}
