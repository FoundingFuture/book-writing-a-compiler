/* shapes.h, the C interface of com.example.shapes, written by antic.
   Do not edit. A failure that Anti cannot report calls abort(). */
#ifndef SHAPES_H
#define SHAPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#define ANTI_ALIGNAS(n) alignas(n)
extern "C" {
#else
#define ANTI_ALIGNAS(n) _Alignas(n)
#endif

/** An integer or a float in 8 bytes. */
typedef union Num {
    int64_t i;
    double d;
} Num;

/** Five bytes without padding. */
#pragma pack(push, 1)
typedef struct Tight {
    uint8_t tag;
    int32_t value;
} Tight;
#pragma pack(pop)

typedef struct Wide {
    ANTI_ALIGNAS(16) int8_t a;
    uint32_t flags : 3;
} Wide;

typedef struct Holder {
    uint8_t tags[4];
    bool (*cb)(long, uint8_t *);
    struct Holder *next;
    Num n;
} Holder;

#define HALF 0.5f
#define ON true
static const char NAME[] = "shapes";

/** Set n bytes from p to the low byte of w. */
void fill(uint8_t *p, uint64_t n, wchar_t w);

#ifdef __cplusplus
}
#endif

#endif
