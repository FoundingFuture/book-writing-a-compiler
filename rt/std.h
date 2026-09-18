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

/* Print the text of a failed assertion to standard error and abort. The
   compiler built the text, so this adds only a newline. */
void anti_rt_assert_failed(const unsigned char *text, int64_t length);

/* Print the name of the class a checked cast wanted and abort. The name
   is not a C string, so its length comes with it. */
void anti_rt_cast_failed(const unsigned char *name, int64_t length);

/* The str of a NUL-terminated C string, without the NUL. */
struct anti_text anti_rt_text_from_c(const unsigned char *bytes);
struct anti_text anti_rt_text_slice(const unsigned char *bytes, int64_t len);

/* The fields of `anti.text.Builder`, which the class declares in this
   order after the table pointer that every object begins with. The
   runtime appends to one when it serializes an object. */
struct anti_builder {
    const void *table;
    unsigned char *room;
    int64_t length;
    int64_t capacity;
};

void anti_rt_builder_append(struct anti_builder *b,
                            const unsigned char *bytes, int64_t len);
struct anti_text anti_rt_builder_text(const struct anti_builder *b);
void anti_rt_builder_clear(struct anti_builder *b);

/* The lines of anti_licenses between its markers. */
struct anti_text anti_rt_license_text(void);

#endif

/* The value of errno, which is a macro and therefore not an Anti
   extern. */
int32_t anti_rt_errno(void);

/* The message of an errno value, as a NUL-terminated C string that the
   caller does not free. */
const unsigned char *anti_rt_errno_text(int32_t code);

/* Nanoseconds from a clock that never moves backwards, whose zero has no
   meaning of its own. */
int64_t anti_rt_monotonic(void);

/* Nanoseconds since 1970-01-01T00:00:00Z. */
int64_t anti_rt_wall(void);

/* Wait for the given nanoseconds, or return at once for a count that is
   not positive. */
void anti_rt_sleep(int64_t nanoseconds);
