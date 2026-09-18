#ifndef ANTIC_PROCESS_H
#define ANTIC_PROCESS_H

#include "text.h"

/* Run the program argv[0], found through PATH when it contains no '/',
   with the NULL-terminated argument list argv. Return its exit status, or
   -1 when it could not start or ended by a signal. The program inherits
   standard output and standard error. */
int process_run(const char *const argv[]);

/* Run a program as process_run does and append its standard output to
   out. */
int process_capture(const char *const argv[], struct text *out);

#endif
