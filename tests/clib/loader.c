/* A C program that loads a shared Anti library at run time. The first call
   finds the runtime initialised by the library's constructor. */
#include <dlfcn.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    void *library;
    int (*ready)(void);

    if (argc < 2 || (library = dlopen(argv[1], RTLD_NOW)) == NULL) {
        fprintf(stderr, "cannot load the library\n");
        return 2;
    }
    ready = (int (*)(void))dlsym(library, "geo_ready");
    if (ready == NULL) {
        fprintf(stderr, "no geo_ready\n");
        return 3;
    }
    printf("%d\n", ready());
    return dlclose(library);
}
