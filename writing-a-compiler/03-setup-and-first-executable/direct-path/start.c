/* The C entry point of every Anti program. It calls the program's main
   through anti.rt.main, the runtime entry symbol that antic defines, and
   returns the result as the process exit code. */
#include <stdint.h>

/* A symbol with a dot is not a C identifier, so the declaration names it
   with an assembler label. Mach-O adds '_' to C symbols, ELF does not. */
#if defined(__APPLE__)
#define ANTI_ENTRY_SYMBOL "_anti.rt.main"
#else
#define ANTI_ENTRY_SYMBOL "anti.rt.main"
#endif

extern int64_t anti_main(void) __asm__(ANTI_ENTRY_SYMBOL);

int main(void)
{
    return (int)anti_main();
}
