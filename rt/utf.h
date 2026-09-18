/* Text conversions of the runtime entry. They use no platform functions,
   so the unit tests check them on any host. */
#ifndef ANTI_UTF_H
#define ANTI_UTF_H

#include <stddef.h>
#include <stdint.h>

size_t anti_utf8_repair(const unsigned char *in, size_t n, unsigned char *out);
size_t anti_utf16_to_utf8(const uint16_t *in, size_t n, unsigned char *out);
size_t anti_split_command_line(const uint16_t *line, uint16_t *out);

#endif
