/* The ABI probe in C. For every struct and union it prints the size and
   the alignment, and for every field the offset and the value of that
   field. probe.anti prints the same for the same types, so any difference
   in layout shows as a difference in the output.

   DESIGN: a field is printed as the bytes of an object cleared first and
   then given that one field. The image carries the offset and the value
   together, and neither language needs an offsetof. Both clear the
   object, so every byte of the image is defined. A case that cannot clear
   the object prints the value of each field instead. C leaves the padding
   of a struct unspecified once a value is stored in it (C11 6.2.6.1p6). */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

struct Wide
{
    int8_t a;
    long b;
    wchar_t c;
    int16_t d;
};

union Mixed
{
    int8_t a;
    int32_t b;
    double c;
    int64_t d;
};

struct Flags
{
    uint32_t visible : 1;
    uint32_t layer : 4;
    int8_t level : 3;
    uint16_t pad : 9;
};

struct Spread
{
    int8_t a;
    uint32_t b : 5;
    int64_t c : 40;
    uint8_t d : 7;
};

#pragma pack(push, 1)
struct Tight
{
    int8_t a;
    int32_t b;
    int16_t c;
};

struct TightBits
{
    uint8_t a : 3;
    uint32_t b : 7;
};
#pragma pack(pop)

struct Aligned
{
    _Alignas(16) int8_t a;
    int32_t b;
};

union BitUnion
{
    uint32_t a : 3;
    uint8_t b : 7;
};

struct Breaks
{
    uint8_t a;
    uint32_t : 0;
    uint8_t b : 3;
    uint64_t : 0;
    uint8_t c : 5;
};

static void dump(const char *name, const void *value, size_t size)
{
    const unsigned char *p = value;
    size_t i;

    printf("%s", name);
    for (i = 0; i < size; i++) {
        printf(" %02x", p[i]);
    }
    printf("\n");
}

#define TYPE(T, name)                                                     \
    printf("%s size %d align %d\n", name, (int)sizeof(T),                  \
           (int)_Alignof(T))

#define FIELD(T, name, f)                                                 \
    do {                                                                  \
        T v;                                                              \
        memset(&v, 0, sizeof v);                                          \
        v.f = 1;                                                          \
        dump(name "." #f, &v, sizeof v);                                  \
    } while (0)

int main(void)
{
    TYPE(struct Wide, "Wide");
    FIELD(struct Wide, "Wide", a);
    FIELD(struct Wide, "Wide", b);
    FIELD(struct Wide, "Wide", c);
    FIELD(struct Wide, "Wide", d);
    TYPE(union Mixed, "Mixed");
    FIELD(union Mixed, "Mixed", a);
    FIELD(union Mixed, "Mixed", b);
    FIELD(union Mixed, "Mixed", c);
    FIELD(union Mixed, "Mixed", d);
    TYPE(struct Flags, "Flags");
    FIELD(struct Flags, "Flags", visible);
    FIELD(struct Flags, "Flags", layer);
    FIELD(struct Flags, "Flags", level);
    FIELD(struct Flags, "Flags", pad);
    TYPE(struct Spread, "Spread");
    FIELD(struct Spread, "Spread", a);
    FIELD(struct Spread, "Spread", b);
    FIELD(struct Spread, "Spread", c);
    FIELD(struct Spread, "Spread", d);
    TYPE(struct Tight, "Tight");
    FIELD(struct Tight, "Tight", a);
    FIELD(struct Tight, "Tight", b);
    FIELD(struct Tight, "Tight", c);
    TYPE(struct TightBits, "TightBits");
    FIELD(struct TightBits, "TightBits", a);
    FIELD(struct TightBits, "TightBits", b);
    TYPE(struct Aligned, "Aligned");
    FIELD(struct Aligned, "Aligned", a);
    FIELD(struct Aligned, "Aligned", b);
    TYPE(union BitUnion, "BitUnion");
    FIELD(union BitUnion, "BitUnion", a);
    FIELD(union BitUnion, "BitUnion", b);
    TYPE(struct Breaks, "Breaks");
    FIELD(struct Breaks, "Breaks", a);
    FIELD(struct Breaks, "Breaks", b);
    FIELD(struct Breaks, "Breaks", c);
    {
        /* A constant copied into an object of the type. The padding of
           the copy is unspecified, so the fields are read back. */
        static const struct Breaks k = { .a = 1, .b = 2, .c = 3 };
        struct Breaks v = k;

        printf("Breaks.const a %d b %d c %d\n", (int)v.a, (int)v.b,
               (int)v.c);
    }
    return 0;
}
