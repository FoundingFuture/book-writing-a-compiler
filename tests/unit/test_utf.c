#include "check.h"
#include <stdint.h>
#include <stdlib.h>
#include "text.h"
#include "utf.h"

/* The output of anti_utf8_repair as hex bytes separated by spaces. */
static void repairs(const unsigned char *in, size_t n, const char *expected)
{
    unsigned char *out = malloc(3 * n + 1);
    struct text hex = {0};
    size_t count;
    size_t i;

    count = anti_utf8_repair(in, n, out);
    for (i = 0; i < count; i++) {
        text_appendf(&hex, "%s%02X", i == 0 ? "" : " ", out[i]);
    }
    CHECK_STR(text_cstr(&hex), expected);
    text_free(&hex);
    free(out);
}

static void converts(const uint16_t *in, size_t n, const char *expected)
{
    unsigned char *out = malloc(3 * n + 1);
    struct text hex = {0};
    size_t count;
    size_t i;

    count = anti_utf16_to_utf8(in, n, out);
    for (i = 0; i < count; i++) {
        text_appendf(&hex, "%s%02X", i == 0 ? "" : " ", out[i]);
    }
    CHECK_STR(text_cstr(&hex), expected);
    text_free(&hex);
    free(out);
}

/* Split an ASCII command line and join the arguments with '|'. */
static void splits(const char *line, const char *expected)
{
    size_t n = strlen(line);
    uint16_t *units = malloc((n + 1) * sizeof *units);
    uint16_t *out = malloc((2 * n + 2) * sizeof *out);
    struct text joined = {0};
    size_t count;
    size_t arg;
    size_t i;

    for (i = 0; i <= n; i++) {
        units[i] = (uint16_t)(unsigned char)line[i];
    }
    count = anti_split_command_line(units, out);
    for (arg = 0, i = 0; arg < count; arg++, i++) {
        if (arg > 0) {
            text_append(&joined, "|");
        }
        for (; out[i] != 0; i++) {
            char c = (char)out[i];
            text_append_bytes(&joined, &c, 1);
        }
    }
    CHECK_STR(text_cstr(&joined), expected);
    text_free(&joined);
    free(units);
    free(out);
}

void test_utf(void)
{
    static const unsigned char valid[] = {'h', 0xC3, 0xA9, 0xF0, 0x90, 0x8C,
                                          0x82};
    static const unsigned char table8[] = {0xC0, 0xAF, 0xE0, 0x80, 0xBF,
                                           0xF0, 0x81, 0x82, 0x41};
    static const unsigned char table9[] = {0xED, 0xA0, 0x80, 0xED, 0xBF,
                                           0xBF, 0xED, 0xAF, 0x41};
    static const unsigned char table10[] = {0xF4, 0x91, 0x92, 0x93, 0xFF,
                                            0x41, 0x80, 0xBF, 0x42};
    static const unsigned char table11[] = {0xE1, 0x80, 0xE2, 0xF0, 0x91,
                                            0x92, 0xF1, 0xBF, 0x41};
    static const unsigned char c2[] = {0xC2, 0x41, 0x42};
    static const uint16_t pair[] = {0x41, 0xE9, 0xD800, 0xDF02};
    static const uint16_t lone[] = {0xDC00, 0xD800, 0x41, 0xD800};

    /* Well-formed UTF-8 stays unchanged. */
    repairs(valid, sizeof valid, "68 C3 A9 F0 90 8C 82");
    /* Tables 3-8 to 3-11 of the Unicode Standard: one U+FFFD, EF BF BD,
       per maximal subpart. */
    repairs(table8, sizeof table8,
            "EF BF BD EF BF BD EF BF BD EF BF BD EF BF BD EF BF BD EF BF BD "
            "EF BF BD 41");
    repairs(table9, sizeof table9,
            "EF BF BD EF BF BD EF BF BD EF BF BD EF BF BD EF BF BD EF BF BD "
            "EF BF BD 41");
    repairs(table10, sizeof table10,
            "EF BF BD EF BF BD EF BF BD EF BF BD EF BF BD 41 EF BF BD EF BF "
            "BD 42");
    repairs(table11, sizeof table11,
            "EF BF BD EF BF BD EF BF BD EF BF BD 41");
    /* Section 3.9.5: C2 41 42 keeps the A and the B. */
    repairs(c2, sizeof c2, "EF BF BD 41 42");

    /* UTF-16: a surrogate pair is one scalar value, and an unpaired
       surrogate becomes U+FFFD. */
    converts(pair, sizeof pair / sizeof pair[0], "41 C3 A9 F0 90 8C 82");
    converts(lone, sizeof lone / sizeof lone[0],
             "EF BF BD EF BF BD 41 EF BF BD");

    /* The examples of Microsoft's rules for C command-line arguments. */
    splits("prog \"a b c\" d e", "prog|a b c|d|e");
    splits("prog \"ab\\\"c\" \"\\\\\" d", "prog|ab\"c|\\|d");
    splits("prog a\\\\\\b d\"e f\"g h", "prog|a\\\\\\b|de fg|h");
    splits("prog a\\\\\\\"b c d", "prog|a\\\"b|c|d");
    splits("prog a\\\\\\\\\"b c\" d e", "prog|a\\\\b c|d|e");
    splits("prog a\"b\"\" c d", "prog|ab\" c d");
    /* The program name ends at a space outside quotes and keeps its
       backslashes. */
    splits("\"C:\\Program Files\\x.exe\"\targ  ", "C:\\Program Files\\x.exe|arg");
}
