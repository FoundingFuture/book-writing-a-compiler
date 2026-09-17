/* The C functions behind the modules anti.io, anti.text and anti.license of
   the standard library. */
#ifndef ANTI_STD_H
#define ANTI_STD_H

#include <stddef.h>
#include <stdint.h>

/* The layout of Anti's str, as a function returns it. */
struct anti_text {
    const unsigned char *ptr;
    int64_t len;
};

/* Write count bytes to standard output for stream 1, else standard error. */
void anti_rt_write(int32_t stream, const unsigned char *bytes, size_t count);

/* Flush both streams and end the process with status. */
void anti_rt_exit(int32_t status);

/* The str of a NUL-terminated C string, without the NUL. */
struct anti_text anti_rt_text_from_c(const unsigned char *bytes);

/* The lines of anti_licenses between its markers. */
struct anti_text anti_rt_license_text(void);

#endif
