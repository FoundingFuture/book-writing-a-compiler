#ifndef ANTI_RT_TOML_H
#define ANTI_RT_TOML_H

#include <stdint.h>

#include "std.h"

/* A read of the TOML subset that anti.toml and the logger need: bare
   keys, `[table]` headers, `[[table]]` arrays, basic strings, integers,
   floats and booleans. Comments run from `#` to the end of the line.

   The document is a flat list of keys, each the path of its value with a
   dot between the parts. A repeated `[[sink]]` numbers its entries, so
   the first holds `sink.0.kind`. */
struct anti_toml;

/* Read the document. Returns NULL when the text is not the subset. */
struct anti_toml *anti_rt_toml_read(const unsigned char *bytes, int64_t len);

/* The number of keys the document holds. */
int64_t anti_rt_toml_count(const struct anti_toml *doc);

/* The path of the key at index, or an empty text past the end. */
struct anti_text anti_rt_toml_key(const struct anti_toml *doc, int64_t index);

/* The value of the key at index, as the document writes it, with the
   quotes of a string removed. */
struct anti_text anti_rt_toml_value(const struct anti_toml *doc,
                                    int64_t index);

/* The index of the key `path`, or -1 when the document has none. */
int64_t anti_rt_toml_find(const struct anti_toml *doc,
                          const unsigned char *path, int64_t len);

/* The line the value of the key at index came from, counting from 1. */
int64_t anti_rt_toml_line(const struct anti_toml *doc, int64_t index);

void anti_rt_toml_free(struct anti_toml *doc);

#endif
