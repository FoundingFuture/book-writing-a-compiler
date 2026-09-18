#include "direct.h"

#include <stdio.h>

/* DESIGN: the direct path of chapter 3 recognises exactly one program
   shape by scanning characters. It has no tokens and no syntax tree.
   Chapter 4 replaces the scan with the lexer, and the back end chapters
   replace the whole path with the pipeline. */

struct scan {
    const char *source;
    size_t length;
    size_t pos;
    int line;
    int column;
};

static int peek(const struct scan *s)
{
    return s->pos < s->length ? (unsigned char)s->source[s->pos] : -1;
}

static void advance(struct scan *s)
{
    if (s->source[s->pos] == '\n') {
        s->line++;
        s->column = 1;
    } else {
        s->column++;
    }
    s->pos++;
}

static void skip_space(struct scan *s)
{
    for (;;) {
        int c = peek(s);
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            return;
        }
        advance(s);
    }
}

static bool is_word_char(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static bool fail(const struct scan *s, struct diagnostic *diag,
                 const char *message)
{
    diag->line = s->line;
    diag->column = s->column;
    snprintf(diag->message, sizeof diag->message, "%s", message);
    return false;
}

/* Match text at the current position after skipping whitespace. A word
   such as "return" must not continue with a letter, digit or '_'. */
static bool expect(struct scan *s, const char *text, bool word,
                   struct diagnostic *diag)
{
    char message[64];
    size_t n = 0;

    skip_space(s);
    while (text[n] != '\0') {
        if (s->pos + n >= s->length || s->source[s->pos + n] != text[n]) {
            snprintf(message, sizeof message, "expected `%s`", text);
            return fail(s, diag, message);
        }
        n++;
    }
    if (word && s->pos + n < s->length &&
        is_word_char((unsigned char)s->source[s->pos + n])) {
        snprintf(message, sizeof message, "expected `%s`", text);
        return fail(s, diag, message);
    }
    while (n-- > 0) {
        advance(s);
    }
    return true;
}

/* Read an optional '-' and decimal digits. The magnitude is checked
   against the range of int before it is negated. */
static bool integer(struct scan *s, int64_t *value, struct diagnostic *diag)
{
    const uint64_t max_positive = (uint64_t)INT64_MAX;
    uint64_t magnitude = 0;
    bool negative = false;
    struct scan start;

    skip_space(s);
    start = *s;
    if (peek(s) == '-') {
        negative = true;
        advance(s);
        skip_space(s);
    }
    if (peek(s) < '0' || peek(s) > '9') {
        return fail(s, diag, "expected an integer literal");
    }
    start.line = s->line;
    start.column = s->column;
    while (peek(s) >= '0' && peek(s) <= '9') {
        uint64_t digit = (uint64_t)(peek(s) - '0');
        if (magnitude > (max_positive + 1 - digit) / 10) {
            return fail(&start, diag, "integer literal does not fit int");
        }
        magnitude = magnitude * 10 + digit;
        advance(s);
    }
    if (magnitude > max_positive + (negative ? 1u : 0u)) {
        return fail(&start, diag, "integer literal does not fit int");
    }
    if (negative) {
        *value = magnitude == max_positive + 1 ? INT64_MIN
                                               : -(int64_t)magnitude;
    } else {
        *value = (int64_t)magnitude;
    }
    return true;
}

bool direct_parse(const char *source, size_t length, int64_t *value,
                  struct diagnostic *diag)
{
    struct scan s = {source, length, 0, 1, 1};

    if (!expect(&s, "fn", true, diag) || !expect(&s, "main", true, diag) ||
        !expect(&s, "(", false, diag) || !expect(&s, ")", false, diag) ||
        !expect(&s, "->", false, diag) || !expect(&s, "int", true, diag) ||
        !expect(&s, "{", false, diag) || !expect(&s, "return", true, diag) ||
        !integer(&s, value, diag) || !expect(&s, ";", false, diag) ||
        !expect(&s, "}", false, diag)) {
        return false;
    }
    skip_space(&s);
    if (s.pos != s.length) {
        return fail(&s, diag, "expected the end of the file");
    }
    return true;
}
