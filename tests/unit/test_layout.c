#include "check.h"
#include "arena.h"
#include "ir.h"
#include "layout.h"

static struct ir_field field(const char *name, enum ir_type type)
{
    struct ir_field f;

    memset(&f, 0, sizeof f);
    f.name = name;
    f.type = ir_scalar(type);
    return f;
}

/* Fields in declaration order, each at the next multiple of its alignment.
   The struct takes the largest alignment, and its size rounds up to it. */
static void structs(void)
{
    struct arena arena = {0};
    struct ir_module m;
    struct layouts layouts;
    struct ir_field fields[3];
    const struct layout *l;
    char error[200] = "";
    uint32_t s;
    uint32_t array;
    uint32_t product;
    uint64_t value;

    ir_module_init(&m, &arena, "main");
    fields[0] = field("a", IR_I8);
    fields[1] = field("b", IR_I32);
    fields[2] = field("c", IR_I16);
    s = ir_struct_add(&m, IR_AGG_STRUCT, "main.S", fields, 3, false, 0);
    array = ir_array_add(&m, "[3]main.S", ir_aggregate(s),
                         ir_sym_int(&m, IR_I64, 3), "3");
    product = ir_sym_op(&m, IR_MUL, IR_I64, ir_sym_size_of(&m, ir_aggregate(s)),
                        ir_sym_offset_of(&m, s, 2));
    CHECK(layouts_init(&layouts, TARGET_LINUX_X86_64, &m, error,
                       sizeof error));
    l = layout_agg(&layouts, s);
    CHECK(l->size == 12 && l->align == 4);
    CHECK(l->offsets[0] == 0 && l->offsets[1] == 4 && l->offsets[2] == 8);
    CHECK(l->member_count == 3 && l->members[1].offset == 4 &&
          l->members[1].type == IR_I32);
    CHECK(layout_size(&layouts, ir_aggregate(array)) == 36);
    CHECK(layout_align(&layouts, ir_aggregate(array)) == 4);
    CHECK(layout_fold(&layouts, product, &value) && value == 96);
    layouts_free(&layouts);
    ir_module_free(&m);
    arena_free(&arena);
}

/* A union puts every field at offset 0. packed removes padding, and
   align raises the alignment and rounds the size up to it. */
static void modifiers(void)
{
    struct arena arena = {0};
    struct ir_module m;
    struct layouts layouts;
    struct ir_field fields[3];
    const struct layout *l;
    char error[200] = "";
    uint32_t u, p, a;

    ir_module_init(&m, &arena, "main");
    fields[0] = field("a", IR_I8);
    fields[1] = field("b", IR_F64);
    fields[2] = field("c", IR_I32);
    u = ir_struct_add(&m, IR_AGG_UNION, "main.U", fields, 3, false, 0);
    p = ir_struct_add(&m, IR_AGG_STRUCT, "main.P", fields, 3, true, 0);
    a = ir_struct_add(&m, IR_AGG_STRUCT, "main.A", fields, 1, false, 16);
    CHECK(layouts_init(&layouts, TARGET_WINDOWS_ARM64, &m, error,
                       sizeof error));
    l = layout_agg(&layouts, u);
    CHECK(l->size == 8 && l->align == 8);
    CHECK(l->offsets[0] == 0 && l->offsets[1] == 0 && l->offsets[2] == 0);
    l = layout_agg(&layouts, p);
    CHECK(l->size == 13 && l->align == 1);
    CHECK(l->offsets[1] == 1 && l->offsets[2] == 9);
    l = layout_agg(&layouts, a);
    CHECK(l->size == 16 && l->align == 16);
    layouts_free(&layouts);
    ir_module_free(&m);
    arena_free(&arena);
}

/* An array length folds per target. A result below 1 names the target
   and the length as the source wrote it. */
static void lengths(void)
{
    struct arena arena = {0};
    struct ir_module m;
    struct layouts layouts;
    struct ir_field fields[2];
    char error[200] = "";
    uint32_t s;
    uint32_t length;

    ir_module_init(&m, &arena, "main");
    fields[0] = field("a", IR_I32);
    fields[1] = field("b", IR_I32);
    s = ir_struct_add(&m, IR_AGG_STRUCT, "main.S", fields, 2, false, 0);
    length = ir_sym_op(&m, IR_SUB, IR_I64, ir_sym_size_of(&m, ir_aggregate(s)),
                       ir_sym_int(&m, IR_I64, 8));
    ir_array_add(&m, "[size_of(main.S) - 8]i32", ir_scalar(IR_I32), length,
                 "size_of(S) - 8");
    CHECK(!layouts_init(&layouts, TARGET_MACOS_ARM64, &m, error,
                        sizeof error));
    CHECK_STR(error, "the array length `size_of(S) - 8` is 0 on "
                     "macos-arm64, and an array length is at least 1");
    layouts_free(&layouts);
    ir_module_free(&m);
    arena_free(&arena);
}

/* c_long is 4 bytes on Windows and 8 elsewhere, and c_wchar 2 and 4. */
static void c_types(void)
{
    struct arena arena = {0};
    struct ir_module m;
    struct layouts layouts;
    struct ir_field fields[2];
    const struct layout *l;
    char error[200] = "";
    uint32_t s;

    ir_module_init(&m, &arena, "main");
    fields[0] = field("n", IR_CLONG);
    fields[1] = field("w", IR_CWCHAR);
    s = ir_struct_add(&m, IR_AGG_STRUCT, "main.C", fields, 2, false, 0);
    CHECK(layouts_init(&layouts, TARGET_WINDOWS_X86_64, &m, error,
                       sizeof error));
    l = layout_agg(&layouts, s);
    CHECK(l->size == 8 && l->align == 4 && l->offsets[1] == 4);
    CHECK(l->members[0].type == IR_I32 && l->members[1].type == IR_I16);
    layouts_free(&layouts);
    CHECK(layouts_init(&layouts, TARGET_LINUX_ARM64, &m, error, sizeof error));
    l = layout_agg(&layouts, s);
    CHECK(l->size == 16 && l->align == 8 && l->offsets[1] == 8);
    CHECK(l->members[0].type == IR_I64 && l->members[1].type == IR_I32);
    layouts_free(&layouts);
    ir_module_free(&m);
    arena_free(&arena);
}

/* The classifier sees the fields of a union at offset 0, and two equal
   scalars at one offset are one member. */
static void union_members(void)
{
    struct arena arena = {0};
    struct ir_module m;
    struct layouts layouts;
    struct ir_field fields[2];
    const struct layout *l;
    char error[200] = "";
    uint32_t same, mixed;

    ir_module_init(&m, &arena, "main");
    fields[0] = field("a", IR_F32);
    fields[1] = field("b", IR_F32);
    same = ir_struct_add(&m, IR_AGG_UNION, "main.FF", fields, 2, false, 0);
    fields[1] = field("i", IR_I32);
    mixed = ir_struct_add(&m, IR_AGG_UNION, "main.Mix", fields, 2, false, 0);
    CHECK(layouts_init(&layouts, TARGET_LINUX_ARM64, &m, error, sizeof error));
    l = layout_agg(&layouts, same);
    CHECK(l->size == 4 && l->member_count == 1);
    l = layout_agg(&layouts, mixed);
    CHECK(l->size == 4 && l->member_count == 2);
    layouts_free(&layouts);
    ir_module_free(&m);
    arena_free(&arena);
}

/* A packed field away from its alignment makes the struct and every
   aggregate holding it unaligned, which System V passes in memory. An
   alignment below the natural one names the type and the target. */
static void unaligned(void)
{
    struct arena arena = {0};
    struct ir_module m;
    struct layouts layouts;
    struct ir_field fields[2];
    char error[200] = "";
    uint32_t packed, outer, loose, narrow;

    ir_module_init(&m, &arena, "main");
    fields[0] = field("a", IR_I8);
    fields[1] = field("b", IR_I32);
    packed = ir_struct_add(&m, IR_AGG_STRUCT, "main.P", fields, 2, true, 0);
    fields[1].type = ir_aggregate(packed);
    outer = ir_struct_add(&m, IR_AGG_STRUCT, "main.O", fields, 2, false, 0);
    fields[1] = field("b", IR_I8);
    loose = ir_struct_add(&m, IR_AGG_STRUCT, "main.L", fields, 2, true, 16);
    CHECK(layouts_init(&layouts, TARGET_LINUX_X86_64, &m, error, sizeof error));
    CHECK(layout_agg(&layouts, packed)->unaligned);
    CHECK(layout_agg(&layouts, outer)->unaligned);
    CHECK(!layout_agg(&layouts, loose)->unaligned);
    CHECK(layout_agg(&layouts, loose)->size == 16);
    layouts_free(&layouts);
    fields[1] = field("b", IR_CLONG);
    narrow = ir_struct_add(&m, IR_AGG_STRUCT, "main.N", fields, 2, false, 4);
    CHECK(narrow == 3);
    CHECK(!layouts_init(&layouts, TARGET_LINUX_ARM64, &m, error, sizeof error));
    CHECK_STR(error, "`main.N` has align(4), below its alignment 8 on "
                     "linux-arm64");
    layouts_free(&layouts);
    ir_module_free(&m);
    arena_free(&arena);
}

static struct ir_field bitfield(const char *name, enum ir_type type,
                                uint8_t bits)
{
    struct ir_field f = field(name, type);

    f.bits = bits;
    f.ext = IR_EXT_ZERO;
    return f;
}

/* System V places a bitfield at the next free bit unless it would cross
   a boundary of its type's alignment. MSVC gives consecutive bitfields of
   one size a shared unit of that size and opens a new unit otherwise. */
static void bitfields(void)
{
    struct arena arena = {0};
    struct ir_module m;
    struct layouts layouts;
    struct ir_field fields[4];
    const struct layout *l;
    char error[200] = "";
    uint32_t s1, s3, s4;

    ir_module_init(&m, &arena, "main");
    fields[0] = bitfield("a", IR_I8, 3);
    fields[1] = bitfield("b", IR_I32, 7);
    fields[2] = bitfield("c", IR_I16, 9);
    s1 = ir_struct_add(&m, IR_AGG_STRUCT, "main.S1", fields, 3, false, 0);
    s3 = ir_struct_add(&m, IR_AGG_STRUCT, "main.S3", fields, 2, true, 0);
    fields[0] = field("a", IR_I8);
    fields[1] = bitfield("b", IR_I32, 5);
    fields[2] = bitfield("c", IR_I64, 40);
    fields[3] = bitfield("d", IR_I8, 7);
    s4 = ir_struct_add(&m, IR_AGG_STRUCT, "main.S4", fields, 4, false, 0);
    CHECK(layouts_init(&layouts, TARGET_LINUX_X86_64, &m, error, sizeof error));
    l = layout_agg(&layouts, s1);
    CHECK(l->size == 4 && l->align == 4);
    CHECK(l->bits[0].pos == 0 && l->bits[1].pos == 3 && l->bits[2].pos == 16);
    l = layout_agg(&layouts, s3);
    CHECK(l->size == 2 && l->align == 1 && l->bits[1].pos == 3);
    l = layout_agg(&layouts, s4);
    CHECK(l->size == 8 && l->align == 8);
    CHECK(l->bits[1].pos == 8 && l->bits[2].pos == 13 && l->bits[3].pos == 56);
    /* Bits 13 to 52 need 6 bytes from byte 1. A load reads 8 bytes, moved
       back to byte 0 so that they end inside the struct. */
    CHECK(l->bits[2].unit_offset == 0 && l->bits[2].unit_type == IR_I64 &&
          l->bits[2].shift == 13);
    CHECK(l->bits[3].unit_offset == 7 && l->bits[3].unit_type == IR_I8 &&
          l->bits[3].shift == 0);
    layouts_free(&layouts);
    CHECK(layouts_init(&layouts, TARGET_WINDOWS_ARM64, &m, error,
                       sizeof error));
    l = layout_agg(&layouts, s1);
    CHECK(l->size == 12 && l->align == 4);
    CHECK(l->bits[0].pos == 0 && l->bits[1].pos == 32 && l->bits[2].pos == 64);
    l = layout_agg(&layouts, s3);
    CHECK(l->size == 5 && l->align == 1 && l->bits[1].pos == 8);
    l = layout_agg(&layouts, s4);
    CHECK(l->size == 24 && l->align == 8);
    CHECK(l->bits[1].pos == 32 && l->bits[2].pos == 64 &&
          l->bits[3].pos == 128);
    layouts_free(&layouts);
    ir_module_free(&m);
    arena_free(&arena);
}

/* The facts that clang 21.0.0 gives for a zero-width bitfield, for one
   target of each rule. S1 is `i8 a, i32 : 0, i8 b`, and S3 is `i8 a : 3,
   i64 : 0, i8 b : 3`. S4 is `i32 a : 3, i8 : 0, i32 b : 3`. S7 is `i16 a
   : 4, i32 : 0, i16 b : 4`. P2 is S3 packed with `i32 : 0`. S6 is `i32 :
   0, i8 a`. The b values are the offset or the bit position of b. */
struct zero_width_facts {
    enum target target;
    uint64_t s1_size, s1_align, s1_b;
    uint64_t s3_size, s3_align, s3_b;
    uint64_t s4_size, s4_b;
    uint64_t s7_size, s7_align, s7_b;
    uint64_t p2_size, p2_align, p2_b;
    uint64_t s6_size;
};

/* A zero-width bitfield `_: T : 0` breaks the unit. System V moves the
   next field to a boundary of T's alignment, packed or not, and AAPCS64
   outside Apple also raises the alignment to T's. MSVC ignores it after a
   field that is not a bitfield. After a bitfield it closes the unit and
   aligns the next field to T, and T's alignment joins the struct's. */
static void zero_width(void)
{
    static const struct zero_width_facts facts[] = {
        {TARGET_LINUX_X86_64, 5, 1, 4, 9, 1, 64, 4, 8, 6, 2, 32, 5, 1, 32, 1},
        {TARGET_MACOS_ARM64, 5, 1, 4, 9, 1, 64, 4, 8, 6, 2, 32, 5, 1, 32, 1},
        {TARGET_LINUX_ARM64, 8, 4, 4, 16, 8, 64, 4, 8, 8, 4, 32, 8, 4, 32, 4},
        {TARGET_WINDOWS_X86_64, 2, 1, 1, 16, 8, 64, 8, 32, 8, 4, 32, 2, 1, 8,
         1},
    };
    struct arena arena = {0};
    struct ir_module m;
    struct layouts layouts;
    struct ir_field fields[3];
    const struct layout *l;
    char error[200] = "";
    uint32_t s1, s3, s4, s7, p2, s6;
    size_t i;

    ir_module_init(&m, &arena, "main");
    fields[0] = field("a", IR_I8);
    fields[1] = field("_", IR_I32);
    fields[2] = field("b", IR_I8);
    s1 = ir_struct_add(&m, IR_AGG_STRUCT, "main.S1", fields, 3, false, 0);
    fields[0] = bitfield("a", IR_I8, 3);
    fields[1] = field("_", IR_I64);
    fields[2] = bitfield("b", IR_I8, 3);
    s3 = ir_struct_add(&m, IR_AGG_STRUCT, "main.S3", fields, 3, false, 0);
    fields[1] = field("_", IR_I32);
    p2 = ir_struct_add(&m, IR_AGG_STRUCT, "main.P2", fields, 3, true, 0);
    fields[0] = bitfield("a", IR_I32, 3);
    fields[1] = field("_", IR_I8);
    fields[2] = bitfield("b", IR_I32, 3);
    s4 = ir_struct_add(&m, IR_AGG_STRUCT, "main.S4", fields, 3, false, 0);
    fields[0] = bitfield("a", IR_I16, 4);
    fields[1] = field("_", IR_I32);
    fields[2] = bitfield("b", IR_I16, 4);
    s7 = ir_struct_add(&m, IR_AGG_STRUCT, "main.S7", fields, 3, false, 0);
    fields[0] = field("_", IR_I32);
    fields[1] = field("a", IR_I8);
    s6 = ir_struct_add(&m, IR_AGG_STRUCT, "main.S6", fields, 2, false, 0);
    for (i = 0; i < sizeof facts / sizeof facts[0]; i++) {
        const struct zero_width_facts *f = &facts[i];
        CHECK(layouts_init(&layouts, f->target, &m, error, sizeof error));
        l = layout_agg(&layouts, s1);
        CHECK(l->size == f->s1_size && l->align == f->s1_align &&
              l->offsets[2] == f->s1_b && l->member_count == 2);
        l = layout_agg(&layouts, s3);
        CHECK(l->size == f->s3_size && l->align == f->s3_align &&
              l->bits[2].pos == f->s3_b);
        l = layout_agg(&layouts, s4);
        CHECK(l->size == f->s4_size && l->bits[2].pos == f->s4_b);
        l = layout_agg(&layouts, s7);
        CHECK(l->size == f->s7_size && l->align == f->s7_align &&
              l->bits[2].pos == f->s7_b);
        l = layout_agg(&layouts, p2);
        CHECK(l->size == f->p2_size && l->align == f->p2_align &&
              l->bits[2].pos == f->p2_b);
        l = layout_agg(&layouts, s6);
        CHECK(l->size == f->s6_size);
        layouts_free(&layouts);
    }
    ir_module_free(&m);
    arena_free(&arena);
}

void test_layout(void)
{
    zero_width();
    bitfields();
    unaligned();
    union_members();
    c_types();
    structs();
    modifiers();
    lengths();
}
