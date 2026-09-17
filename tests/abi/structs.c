/* C functions that take and return structs by value. The program test
   abi_structs calls them from Anti, so that clang checks the struct rules
   of each calling convention in antic. */
#include <stdint.h>

struct Pair { int64_t a; double b; };
struct Big { int64_t a, b, c; };
struct Rgb { uint8_t r, g, b; };
struct V2 { float x, y; };
struct Mixed { int32_t i; float f; };
struct Quad { double a, b, c, d; };
struct Odd { int16_t a; int8_t b; uint8_t c[3]; };
struct Tri { double a, b, c; };
struct Huge { int64_t v[12]; float f; };

struct Pair abi_pair(struct Pair p, int64_t k)
{
    struct Pair r = {p.a + k, p.b * 2.0};
    return r;
}

struct Big abi_big(struct Big b)
{
    struct Big r = {b.c, b.b * 10, b.a};
    return r;
}

struct Rgb abi_rgb(struct Rgb c)
{
    struct Rgb r = {c.b, (uint8_t)(c.g + 1), c.r};
    return r;
}

struct V2 abi_v2(struct V2 a, struct V2 b)
{
    struct V2 r = {a.x + b.x, a.y * b.y};
    return r;
}

struct Mixed abi_mixed(struct Mixed m)
{
    struct Mixed r = {m.i * 3, m.f - 1.0f};
    return r;
}

struct Quad abi_quad(struct Quad q)
{
    struct Quad r = {q.d, q.c, q.b, q.a + 0.5};
    return r;
}

struct Odd abi_odd(struct Odd o)
{
    struct Odd r = {(int16_t)(o.a - 1), (int8_t)(o.b * 2),
                    {o.c[2], o.c[1], o.c[0]}};
    return r;
}

double abi_many(struct Rgb a, struct Pair b, struct Big c, struct V2 d,
                struct Mixed e, struct Quad f, struct Odd g, int64_t h,
                double i, struct Pair j, struct Big k)
{
    return (double)(a.r + a.g + a.b) + (double)b.a + b.b +
           (double)(c.a + c.b + c.c) + d.x + d.y + (double)e.i + e.f + f.a +
           f.b + f.c + f.d + (double)(g.a + g.b + g.c[0] + g.c[1] + g.c[2]) +
           (double)h + i + (double)j.a + j.b + (double)(k.a - k.b + k.c);
}

/* Seven doubles leave one float register, so t, v and v2 go to the stack.
   Each argument has its own weight, so a misplaced one changes the sum. */
double abi_stack(double d0, double d1, double d2, double d3, double d4,
                 double d5, double d6, struct Tri t, struct V2 v, float f,
                 struct Rgb c, uint8_t u, struct Pair p, int64_t i0,
                 int64_t i1, int64_t i2, int64_t i3, int64_t i4, int64_t i5,
                 struct Rgb c2, int16_t s, struct V2 v2)
{
    return d0 + 2 * d1 + 3 * d2 + 4 * d3 + 5 * d4 + 6 * d5 + 7 * d6 +
           8 * t.a + 9 * t.b + 10 * t.c + 11 * v.x + 12 * v.y + 13 * f +
           14 * (c.r + 2 * c.g + 3 * c.b) + 15 * u + 16 * (double)p.a +
           17 * p.b + 18 * (double)i0 + 19 * (double)i1 + 20 * (double)i2 +
           21 * (double)i3 + 22 * (double)i4 + 23 * (double)i5 +
           24 * (c2.r + 2 * c2.g + 3 * c2.b) + 25 * s + 26 * v2.x +
           27 * v2.y;
}

/* A 104-byte struct: antic copies it with memcpy, and the result goes
   through the address that the caller passes. */
struct Huge abi_huge(int64_t a, struct Huge h, double d, struct Huge g,
                     int64_t b)
{
    struct Huge r;
    int i;

    for (i = 0; i < 12; i++) {
        r.v[i] = h.v[i] * a + g.v[11 - i] * b;
    }
    r.f = h.f + g.f + (float)d;
    return r;
}

union Num { int64_t i; double d; };
union FF { float a; float b; };
union Mix { float f; int32_t i; };
struct Holder { int8_t tag; union Num n; };

/* Unions pass under the struct rules with every field at offset 0. FF is
   a float aggregate of one member, and Mix is an integer. */
union Num abi_num(union Num n)
{
    union Num r;

    r.d = n.d * 2.0;
    return r;
}

union FF abi_ff(union FF u, union FF v)
{
    union FF r;

    r.a = u.a + v.b;
    return r;
}

union Mix abi_mix(union Mix m)
{
    union Mix r;

    r.i = m.i + 1;
    return r;
}

struct Holder abi_holder(struct Holder h)
{
    struct Holder r;

    r.tag = (int8_t)(h.tag + 1);
    r.n.i = h.n.i * 3;
    return r;
}

struct __attribute__((packed)) Tight { int8_t a; int32_t b; };
typedef struct { _Alignas(16) int8_t a; } Wide;

/* A packed struct with a field away from its alignment passes in memory
   on System V. A 16-aligned struct starts at an even register on AAPCS64
   outside Apple, and on the stack at a multiple of 16. */
struct Tight abi_tight(struct Tight t, int64_t k)
{
    struct Tight r;

    r.a = (int8_t)(t.a + 1);
    r.b = t.b + (int32_t)k;
    return r;
}

int64_t abi_wide(int64_t a, Wide w, int64_t b, int64_t c, int64_t d,
                 int64_t e, int64_t f, Wide w2, int64_t g)
{
    return a + 2 * w.a + 3 * b + 4 * c + 5 * d + 6 * e + 7 * f + 8 * w2.a +
           9 * g;
}

struct Bits { uint32_t visible : 1; uint32_t layer : 4; int8_t level : 3; uint16_t pad : 9; };

/* Bitfields follow the System V rule on Linux and macOS and the MSVC rule
   on Windows. */
struct Bits abi_bits(struct Bits b)
{
    struct Bits r = b;

    r.layer = b.layer + 1;
    /* gcc warns for every assignment to a narrow signed bitfield that it
       cannot range-check, and the field holds three bits. The test passes
       -2, and -4 is the smallest value the field holds. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#endif
    r.level = (int8_t)(b.level * 2);
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
    r.pad = (uint16_t)(b.pad + 7);
    return r;
}

int64_t anti_twice(int64_t x);

/* A C caller of an export fn of the Anti program. */
int64_t abi_calls_anti(int64_t x)
{
    return anti_twice(x) + 1;
}
