/* Minimal assertion macros for the unit tests. A failed check prints its
   location and the test run continues, so one run reports every failure. */
#ifndef ANTIC_CHECK_H
#define ANTIC_CHECK_H

#include <stdio.h>
#include <string.h>

extern int check_failures;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            check_failures++;                                              \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,         \
                    __LINE__, #cond);                                      \
        }                                                                  \
    } while (0)

#define CHECK_STR(actual, expected)                                        \
    do {                                                                   \
        const char *check_a = (actual);                                    \
        const char *check_e = (expected);                                  \
        if (strcmp(check_a, check_e) != 0) {                               \
            check_failures++;                                              \
            fprintf(stderr, "%s:%d: expected\n%s\ngot\n%s\n", __FILE__,    \
                    __LINE__, check_e, check_a);                           \
        }                                                                  \
    } while (0)

#endif
