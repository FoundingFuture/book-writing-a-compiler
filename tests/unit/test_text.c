#include "check.h"
#include "text.h"

void test_text(void)
{
    struct text t = {0};
    int i;

    CHECK_STR(text_cstr(&t), "");
    text_append(&t, "mov");
    text_appendf(&t, " x%d, #%s", 0, "42");
    CHECK_STR(text_cstr(&t), "mov x0, #42");
    for (i = 0; i < 1000; i++) {
        text_append(&t, "0123456789");
    }
    CHECK(t.length == 11 + 10000);
    text_free(&t);
    CHECK(t.data == NULL && t.length == 0);

    /* Binary data may hold NUL bytes. */
    text_append_bytes(&t, "A\0B", 3);
    CHECK(t.length == 3 && memcmp(t.data, "A\0B", 4) == 0);
    text_free(&t);
}
