#include "emit_direct.h"

/* Load a 64-bit value into x0. An ARM64 instruction holds at most 16 bits
   of an immediate, so the value goes in as up to four 16-bit pieces. The
   first movz sets one piece and clears the rest. Each movk sets one more. */
static void load_x0(struct text *out, int64_t value)
{
    uint64_t bits = (uint64_t)value;
    bool first = true;
    int shift;

    if (bits == 0) {
        text_append(out, "    movz    x0, #0\n");
        return;
    }
    for (shift = 0; shift < 64; shift += 16) {
        uint64_t piece = (bits >> shift) & 0xffff;
        if (piece == 0) {
            continue;
        }
        text_appendf(out, "    %s    x0, #%u", first ? "movz" : "movk",
                     (unsigned)piece);
        if (shift != 0) {
            text_appendf(out, ", lsl #%d", shift);
        }
        text_append(out, "\n");
        first = false;
    }
}

bool emit_direct(struct text *out, enum target t, const char *module,
                 int64_t value)
{
    struct text entry = {0};
    struct text main_symbol = {0};

    if (t != TARGET_MACOS_ARM64) {
        return false;
    }
    mangle(&entry, t, RUNTIME_MODULE, RUNTIME_ENTRY);
    mangle(&main_symbol, t, module, "main");

    text_appendf(out, "    .build_version macos, %d, %d\n", MACOS_MIN_MAJOR,
                 MACOS_MIN_MINOR);
    text_append(out, "    .text\n");
    text_appendf(out, "    .globl  %s\n", text_cstr(&entry));
    text_appendf(out, "    .set    %s, %s\n", text_cstr(&entry),
                 text_cstr(&main_symbol));
    text_append(out, "    .p2align 2\n");
    text_appendf(out, "%s:\n", text_cstr(&main_symbol));
    load_x0(out, value);
    text_append(out, "    ret\n");

    text_free(&entry);
    text_free(&main_symbol);
    return true;
}
