/* geo.h, the C interface of com.example.geo, written by antic.
   Do not edit. A failure that Anti cannot report calls abort().
   The archive holds the Anti runtime. Link only one archive
   with a bundled runtime into a program. */
#ifndef GEO_H
#define GEO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#define ANTI_ALIGNAS(n) alignas(n)
extern "C" {
#else
#define ANTI_ALIGNAS(n) _Alignas(n)
#endif

/** A point on the plane. */
typedef struct Vec2 {
    int32_t x;
    int32_t y;
} Vec2;

/** An integer or a float in 8 bytes. */
typedef union Num {
    int64_t i;
    double d;
} Num;

/** Display flags. */
typedef struct Flags {
    uint32_t visible : 1;
    uint32_t layer : 4;
} Flags;

/** The largest layer. */
#define LAYERS ((int64_t)16)

/** The dot product of a and b. */
int32_t geo_dot(Vec2 a, Vec2 b);
/** v with both coordinates multiplied by k. */
Vec2 geo_scale(Vec2 v, int32_t k);
/** Half of the float in n. */
Num geo_half(Num n);
/** The layer of the flags at f, with visible set. */
uint32_t geo_layer(Flags *f);
/** The squared length of v, through a function pointer. */
int32_t geo_apply(int32_t (*f)(int32_t), Vec2 v);
/** 1 once the runtime is initialised. */
int32_t geo_ready(void);

#ifdef __cplusplus
}
#endif

#endif
