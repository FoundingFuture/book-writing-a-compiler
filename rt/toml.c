/* The TOML subset that anti.toml reads and the logger configures itself
   from. It is one pass over the text that writes a flat list of keys.

   DESIGN: the runtime holds the parser because the logger reads its file
   before `main` runs, where no Anti code has started. One parser then
   serves both, and anti.toml is a face over this list. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "toml.h"

struct pair {
    char *key;
    char *value;
    int64_t key_length;
    int64_t value_length;
    int64_t line;
};

struct anti_toml {
    struct pair *pairs;
    size_t count;
    size_t capacity;
};

struct reader {
    const unsigned char *bytes;
    int64_t len;
    int64_t pos;
    int64_t line;
    char table[256];
    size_t table_length;
    int failed;
};

static int at_end(const struct reader *r)
{
    return r->pos >= r->len;
}

static int peek(const struct reader *r)
{
    return at_end(r) ? -1 : r->bytes[r->pos];
}

static void skip_spaces(struct reader *r)
{
    while (!at_end(r) && (r->bytes[r->pos] == ' ' || r->bytes[r->pos] == '\t')) {
        r->pos++;
    }
}

/* Past the spaces, the comments and the line breaks. */
static void skip_blank(struct reader *r)
{
    while (!at_end(r)) {
        unsigned char c = r->bytes[r->pos];
        if (c == ' ' || c == '\t' || c == '\r') {
            r->pos++;
        } else if (c == '\n') {
            r->line++;
            r->pos++;
        } else if (c == '#') {
            while (!at_end(r) && r->bytes[r->pos] != '\n') {
                r->pos++;
            }
        } else {
            return;
        }
    }
}

static int is_bare(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-';
}

static char *keep(const char *text, int64_t length)
{
    char *copy = malloc((size_t)length + 1);

    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, text, (size_t)length);
    copy[length] = '\0';
    return copy;
}

static void add(struct anti_toml *doc, struct reader *r, const char *key,
                int64_t key_length, const char *value, int64_t value_length)
{
    struct pair *pair;

    if (doc->count == doc->capacity) {
        size_t capacity = doc->capacity == 0 ? 16 : doc->capacity * 2;
        struct pair *grown = realloc(doc->pairs, capacity * sizeof *grown);
        if (grown == NULL) {
            r->failed = 1;
            return;
        }
        doc->pairs = grown;
        doc->capacity = capacity;
    }
    pair = &doc->pairs[doc->count];
    pair->key = keep(key, key_length);
    pair->value = keep(value, value_length);
    pair->key_length = key_length;
    pair->value_length = value_length;
    pair->line = r->line;
    if (pair->key == NULL || pair->value == NULL) {
        r->failed = 1;
        return;
    }
    doc->count++;
}

/* The count of `[[name]]` tables seen so far under that name. */
static int64_t repeats(const struct anti_toml *doc, const char *name,
                       size_t length)
{
    int64_t seen = -1;
    size_t i;

    for (i = 0; i < doc->count; i++) {
        const char *key = doc->pairs[i].key;
        if (strncmp(key, name, length) == 0 && key[length] == '.') {
            int64_t number = strtoll(key + length + 1, NULL, 10);
            if (number > seen) {
                seen = number;
            }
        }
    }
    return seen + 1;
}

static void read_header(struct reader *r, struct anti_toml *doc)
{
    int array = 0;
    int64_t start;
    int64_t length;

    r->pos++;
    if (peek(r) == '[') {
        array = 1;
        r->pos++;
    }
    skip_spaces(r);
    start = r->pos;
    while (!at_end(r) && (is_bare(peek(r)) || peek(r) == '.')) {
        r->pos++;
    }
    length = r->pos - start;
    if (length <= 0 || (size_t)length >= sizeof r->table - 24) {
        r->failed = 1;
        return;
    }
    memcpy(r->table, r->bytes + start, (size_t)length);
    r->table_length = (size_t)length;
    r->table[length] = '\0';
    skip_spaces(r);
    if (peek(r) != ']') {
        r->failed = 1;
        return;
    }
    r->pos++;
    if (array) {
        if (peek(r) != ']') {
            r->failed = 1;
            return;
        }
        r->pos++;
        r->table_length += (size_t)snprintf(
            r->table + r->table_length, sizeof r->table - r->table_length,
            ".%lld", (long long)repeats(doc, r->table, r->table_length));
    }
}

/* The value that follows `=`, with the quotes of a string removed. */
static void read_value(struct reader *r, const char **value, int64_t *length)
{
    int64_t start;

    skip_spaces(r);
    if (peek(r) == '"' || peek(r) == '\'') {
        int quote = peek(r);
        r->pos++;
        start = r->pos;
        while (!at_end(r) && peek(r) != quote && peek(r) != '\n') {
            r->pos++;
        }
        if (peek(r) != quote) {
            r->failed = 1;
            return;
        }
        *value = (const char *)r->bytes + start;
        *length = r->pos - start;
        r->pos++;
        return;
    }
    start = r->pos;
    while (!at_end(r) && peek(r) != '\n' && peek(r) != '#') {
        r->pos++;
    }
    *length = r->pos - start;
    while (*length > 0 && (r->bytes[start + *length - 1] == ' ' ||
                           r->bytes[start + *length - 1] == '\t' ||
                           r->bytes[start + *length - 1] == '\r')) {
        (*length)--;
    }
    *value = (const char *)r->bytes + start;
    if (*length == 0) {
        r->failed = 1;
    }
}

static void read_pair(struct reader *r, struct anti_toml *doc)
{
    char path[320];
    const char *value = NULL;
    int64_t value_length = 0;
    int64_t start = r->pos;
    int64_t length;
    int written;

    while (!at_end(r) && is_bare(peek(r))) {
        r->pos++;
    }
    length = r->pos - start;
    if (length <= 0) {
        r->failed = 1;
        return;
    }
    skip_spaces(r);
    if (peek(r) != '=') {
        r->failed = 1;
        return;
    }
    r->pos++;
    read_value(r, &value, &value_length);
    if (r->failed) {
        return;
    }
    written = r->table_length == 0
                  ? snprintf(path, sizeof path, "%.*s", (int)length,
                             r->bytes + start)
                  : snprintf(path, sizeof path, "%s.%.*s", r->table,
                             (int)length, r->bytes + start);
    if (written <= 0 || (size_t)written >= sizeof path) {
        r->failed = 1;
        return;
    }
    add(doc, r, path, written, value, value_length);
}

struct anti_toml *anti_rt_toml_read(const unsigned char *bytes, int64_t len)
{
    struct anti_toml *doc = calloc(1, sizeof *doc);
    struct reader r;

    if (doc == NULL) {
        return NULL;
    }
    memset(&r, 0, sizeof r);
    r.bytes = bytes;
    r.len = len;
    r.line = 1;
    while (!r.failed) {
        skip_blank(&r);
        if (at_end(&r)) {
            return doc;
        }
        if (peek(&r) == '[') {
            read_header(&r, doc);
        } else if (is_bare(peek(&r))) {
            read_pair(&r, doc);
        } else {
            r.failed = 1;
        }
    }
    anti_rt_toml_free(doc);
    return NULL;
}

int64_t anti_rt_toml_count(const struct anti_toml *doc)
{
    return doc == NULL ? 0 : (int64_t)doc->count;
}

static struct anti_text text_of(const char *bytes, int64_t length)
{
    struct anti_text text;

    text.ptr = (const unsigned char *)(bytes == NULL ? "" : bytes);
    text.len = bytes == NULL ? 0 : length;
    return text;
}

struct anti_text anti_rt_toml_key(const struct anti_toml *doc, int64_t index)
{
    if (doc == NULL || index < 0 || index >= (int64_t)doc->count) {
        return text_of(NULL, 0);
    }
    return text_of(doc->pairs[index].key, doc->pairs[index].key_length);
}

struct anti_text anti_rt_toml_value(const struct anti_toml *doc, int64_t index)
{
    if (doc == NULL || index < 0 || index >= (int64_t)doc->count) {
        return text_of(NULL, 0);
    }
    return text_of(doc->pairs[index].value, doc->pairs[index].value_length);
}

int64_t anti_rt_toml_find(const struct anti_toml *doc,
                          const unsigned char *path, int64_t len)
{
    size_t i;

    if (doc == NULL) {
        return -1;
    }
    for (i = 0; i < doc->count; i++) {
        if (doc->pairs[i].key_length == len &&
            memcmp(doc->pairs[i].key, path, (size_t)len) == 0) {
            return (int64_t)i;
        }
    }
    return -1;
}

int64_t anti_rt_toml_line(const struct anti_toml *doc, int64_t index)
{
    if (doc == NULL || index < 0 || index >= (int64_t)doc->count) {
        return 0;
    }
    return doc->pairs[index].line;
}

void anti_rt_toml_free(struct anti_toml *doc)
{
    size_t i;

    if (doc == NULL) {
        return;
    }
    for (i = 0; i < doc->count; i++) {
        free(doc->pairs[i].key);
        free(doc->pairs[i].value);
    }
    free(doc->pairs);
    free(doc);
}
