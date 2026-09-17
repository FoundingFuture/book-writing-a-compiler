/* C functions for the program test abi_wchar. clang converts a wchar_t
   above 0x7FFFFFFF with the signedness of wchar_t on the target, and the
   test compares the conversions of antic with these. */
#include <stdint.h>
#include <wchar.h>

wchar_t abi_big_wchar(void)
{
    return (wchar_t)0x80000001u;
}

int64_t abi_wchar_i64(wchar_t w)
{
    return (int64_t)w;
}

int32_t abi_wchar_i32(wchar_t w)
{
    return (int32_t)w;
}

int32_t abi_wchar_less(wchar_t a, wchar_t b)
{
    return a < b;
}
