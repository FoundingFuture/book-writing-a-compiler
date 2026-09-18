#include <errno.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "std.h"

/* DESIGN: errno is a macro that expands to a thread-local lvalue, so an
   Anti `extern fn` cannot name it. The same holds for strerror, whose
   result is a pointer into storage the caller does not own. */

int32_t anti_rt_errno(void)
{
    return (int32_t)errno;
}

const unsigned char *anti_rt_errno_text(int32_t code)
{
#if defined(_WIN32)
    /* The UCRT deprecates strerror, and strerror_s writes into a buffer
       the caller owns. One per thread keeps the result alive as long as
       the caller needs it. */
    static _Thread_local char text[128];
    if (strerror_s(text, sizeof text, (int)code) != 0) {
        text[0] = '\0';
    }
    return (const unsigned char *)text;
#else
    return (const unsigned char *)strerror((int)code);
#endif
}

/* DESIGN: Win32 keeps its own error apart from errno, and the message
   for it comes from FormatMessage. Every other system has no Win32 to
   ask, so the code is 0 and the message is empty. That keeps
   `Error.from_win32` one function with one meaning everywhere. */
int32_t anti_rt_last_error(void)
{
#if defined(_WIN32)
    return (int32_t)GetLastError();
#else
    return 0;
#endif
}

const unsigned char *anti_rt_last_error_text(int32_t code)
{
#if defined(_WIN32)
    static _Thread_local char buffer[256];
    DWORD written = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
        (DWORD)code, 0, buffer, (DWORD)sizeof buffer - 1, NULL);
    while (written > 0 && (buffer[written - 1] == '\n' ||
                           buffer[written - 1] == '\r')) {
        written--;
    }
    buffer[written] = '\0';
    return (const unsigned char *)buffer;
#else
    (void)code;
    return (const unsigned char *)"";
#endif
}
