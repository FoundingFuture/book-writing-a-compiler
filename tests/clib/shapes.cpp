/* The header of com.example.shapes compiles as C++17, with the sizes that
   chapter 25 states for a packed and an aligned struct. */
#include "shapes.h"

static_assert(sizeof(Tight) == 5, "Tight has no padding");
static_assert(alignof(Wide) == 16, "Wide is aligned to 16");

int main()
{
    return 0;
}
