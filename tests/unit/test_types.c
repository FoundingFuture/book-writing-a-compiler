#include "check.h"
#include "arena.h"
#include "types.h"

static struct name name_of(const char *s)
{
    struct name n = {s, strlen(s)};
    return n;
}

static void named(const struct type *t, const char *expected)
{
    struct text out = {0};
    type_name(&out, t);
    CHECK_STR(text_cstr(&out), expected);
    text_free(&out);
}

void test_types(void)
{
    struct arena arena = {0};
    struct types types;
    struct type *i64, *u8, *f32, *f64, *boolean, *character, *str;
    struct type *pixel, *node, *params[2];
    /* Every field record is zero here, so a flag the test does not set
       reads as false rather than as whatever the stack held. */
    struct struct_field fields[3] = {0};

    types_init(&types, &arena);
    i64 = types_builtin(&types, TYPE_I64);
    u8 = types_builtin(&types, TYPE_U8);
    f32 = types_builtin(&types, TYPE_F32);
    f64 = types_builtin(&types, TYPE_F64);
    boolean = types_builtin(&types, TYPE_BOOL);
    character = types_builtin(&types, TYPE_CHAR);
    str = types_builtin(&types, TYPE_STR);

    /* Derived types are interned, so equal spelling means one pointer. */
    CHECK(types_pointer(&types, i64) == types_pointer(&types, i64));
    CHECK(types_slice(&types, u8) == types_slice(&types, u8));
    CHECK(types_array(&types, f32, 4) == types_array(&types, f32, 4));
    CHECK(types_array(&types, f32, 4) != types_array(&types, f32, 5));
    params[0] = i64;
    params[1] = types_pointer(&types, u8);
    CHECK(types_fn(&types, params, 2, f64) == types_fn(&types, params, 2, f64));
    CHECK(types_fn(&types, params, 2, f64) != types_fn(&types, params, 1, f64));

    named(i64, "int");
    named(f64, "float");
    named(u8, "byte");
    named(types_builtin(&types, TYPE_I32), "i32");
    named(types_pointer(&types, types_slice(&types, u8)), "*[]byte");
    named(types_array(&types, f32, 4), "[4]f32");
    named(types_fn(&types, params, 2, f64), "fn(int, *byte) -> float");
    named(types_fn(&types, NULL, 0, types_builtin(&types, TYPE_VOID)),
          "fn()");


    /* struct Pixel { tag: u8, value: i32, flag: u8 } from chapter 2 */
    pixel = types_struct(&types, name_of("main"), name_of("Pixel"));
    named(pixel, "Pixel");
    CHECK(pixel != types_struct(&types, name_of("main"), name_of("Pixel")));
    fields[0].name = name_of("tag");
    fields[0].type = u8;
    fields[1].name = name_of("value");
    fields[1].type = types_builtin(&types, TYPE_I32);
    fields[2].name = name_of("flag");
    fields[2].type = u8;
    types_set_fields(&types, pixel, fields, 3);
    CHECK(types_find_cycle(pixel) == NULL);

    /* A length computed from size_of is a symbolic value. Equal values
       are one node, so arrays of equal length are one type. */
    {
        struct symbolic key;
        const struct symbolic *size, *four, *length;
        memset(&key, 0, sizeof key);
        key.kind = SYMBOLIC_SIZE_OF;
        key.type = i64;
        key.of = pixel;
        size = types_symbolic(&types, &key);
        CHECK(types_symbolic(&types, &key) == size);
        memset(&key, 0, sizeof key);
        key.kind = SYMBOLIC_INT;
        key.type = i64;
        key.value = 4;
        four = types_symbolic(&types, &key);
        memset(&key, 0, sizeof key);
        key.kind = SYMBOLIC_BINARY;
        key.type = i64;
        key.op = TOKEN_MINUS;
        key.a = size;
        key.b = four;
        length = types_symbolic(&types, &key);
        CHECK(types_array_symbolic(&types, u8, length) ==
              types_array_symbolic(&types, u8, length));
        named(types_array_symbolic(&types, u8, length),
              "[size_of(Pixel) - 4]byte");
    }

    /* struct Node { next: *Node, value: Node } holds itself by value. */
    node = types_struct(&types, name_of("main"), name_of("Node"));
    fields[0].name = name_of("next");
    fields[0].type = types_pointer(&types, node);
    fields[1].name = name_of("value");
    fields[1].type = node;
    types_set_fields(&types, node, fields, 2);
    CHECK(types_find_cycle(node) == node);

    CHECK(type_pointer_free(i64));
    CHECK(type_pointer_free(str));
    CHECK(type_pointer_free(pixel));
    CHECK(type_pointer_free(types_array(&types, pixel, 2)));
    CHECK(!type_pointer_free(types_pointer(&types, i64)));
    CHECK(!type_pointer_free(types_slice(&types, i64)));
    CHECK(!type_pointer_free(types_fn(&types, NULL, 0, i64)));

    CHECK(type_is_integer(u8) && !type_is_signed(u8));
    CHECK(type_is_integer(i64) && type_is_signed(i64));
    CHECK(type_is_float(f32) && !type_is_integer(f32));
    CHECK(!type_is_numeric(boolean) && !type_is_numeric(character));
    CHECK(type_bits(types_builtin(&types, TYPE_U16)) == 16);

    arena_free(&arena);
}
