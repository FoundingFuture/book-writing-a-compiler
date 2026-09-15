#include "utf.h"

static size_t put_scalar(uint32_t c, unsigned char *out)
{
    if (c < 0x80) {
        out[0] = (unsigned char)c;
        return 1;
    }
    if (c < 0x800) {
        out[0] = (unsigned char)(0xC0 | c >> 6);
        out[1] = (unsigned char)(0x80 | (c & 0x3F));
        return 2;
    }
    if (c < 0x10000) {
        out[0] = (unsigned char)(0xE0 | c >> 12);
        out[1] = (unsigned char)(0x80 | (c >> 6 & 0x3F));
        out[2] = (unsigned char)(0x80 | (c & 0x3F));
        return 3;
    }
    out[0] = (unsigned char)(0xF0 | c >> 18);
    out[1] = (unsigned char)(0x80 | (c >> 12 & 0x3F));
    out[2] = (unsigned char)(0x80 | (c >> 6 & 0x3F));
    out[3] = (unsigned char)(0x80 | (c & 0x3F));
    return 4;
}

/* The length of the well-formed sequence that starts with byte b, and the
   range of its second byte, from table 3-7 of the Unicode Standard. */
static size_t sequence_length(unsigned char b, unsigned char *low,
                              unsigned char *high)
{
    *low = 0x80;
    *high = 0xBF;
    if (b < 0x80) {
        return 1;
    }
    if (b >= 0xC2 && b <= 0xDF) {
        return 2;
    }
    if (b >= 0xE0 && b <= 0xEF) {
        *low = b == 0xE0 ? 0xA0 : 0x80;
        *high = b == 0xED ? 0x9F : 0xBF;
        return 3;
    }
    if (b >= 0xF0 && b <= 0xF4) {
        *low = b == 0xF0 ? 0x90 : 0x80;
        *high = b == 0xF4 ? 0x8F : 0xBF;
        return 4;
    }
    return 0;
}

/* DESIGN: each maximal subpart of an ill-formed sequence becomes one
   U+FFFD, the practice of section 3.9.6 of the Unicode Standard. out holds
   3 * n bytes. */
size_t anti_utf8_repair(const unsigned char *in, size_t n, unsigned char *out)
{
    size_t written = 0;
    size_t i = 0;

    while (i < n) {
        unsigned char low;
        unsigned char high;
        size_t length = sequence_length(in[i], &low, &high);
        size_t good = length == 0 ? 0 : 1;
        while (good > 0 && good < length && i + good < n &&
               in[i + good] >= (good == 1 ? low : 0x80) &&
               in[i + good] <= (good == 1 ? high : 0xBF)) {
            good++;
        }
        if (length > 0 && good == length) {
            for (good = 0; good < length; good++) {
                out[written++] = in[i + good];
            }
            i += length;
        } else {
            written += put_scalar(0xFFFD, out + written);
            i += good == 0 ? 1 : good;
        }
    }
    return written;
}

/* A surrogate pair gives one scalar value, and any other surrogate
   U+FFFD. out holds 3 * n bytes. */
size_t anti_utf16_to_utf8(const uint16_t *in, size_t n, unsigned char *out)
{
    size_t written = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        uint32_t c = in[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && in[i + 1] >= 0xDC00 &&
            in[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (in[i + 1] - 0xDC00u);
            i++;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;
        }
        written += put_scalar(c, out + written);
    }
    return written;
}

static int is_blank(uint16_t c)
{
    return c == ' ' || c == '\t';
}

/* DESIGN: the rules of the Microsoft C startup code for command-line
   arguments. The program name ends at a blank outside quotes and keeps its
   backslashes. out receives each argument followed by a 0 unit and holds
   2 * n + 2 units for a line of n units. Returns the number of arguments. */
size_t anti_split_command_line(const uint16_t *line, uint16_t *out)
{
    const uint16_t *p = line;
    size_t count = 0;
    int quoted = 0;

    while (*p != 0 && (quoted || !is_blank(*p))) {
        if (*p == '"') {
            quoted = !quoted;
        } else {
            *out++ = *p;
        }
        p++;
    }
    *out++ = 0;
    count++;
    for (;;) {
        while (is_blank(*p)) {
            p++;
        }
        if (*p == 0) {
            return count;
        }
        quoted = 0;
        while (*p != 0 && (quoted || !is_blank(*p))) {
            size_t slashes = 0;
            int copy = 1;
            while (*p == '\\') {
                slashes++;
                p++;
            }
            if (*p == '"') {
                if (slashes % 2 == 0) {
                    if (quoted && p[1] == '"') {
                        p++;
                    } else {
                        copy = 0;
                        quoted = !quoted;
                    }
                }
                slashes /= 2;
            }
            while (slashes-- > 0) {
                *out++ = '\\';
            }
            if (*p == 0 || (!quoted && is_blank(*p))) {
                break;
            }
            if (copy) {
                *out++ = *p;
            }
            p++;
        }
        *out++ = 0;
        count++;
    }
}
