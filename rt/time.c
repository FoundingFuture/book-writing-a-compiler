/* clock_gettime and nanosleep sit behind a feature macro, and the two
   systems spell it differently. */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#elif !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "std.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

/* DESIGN: the monotonic clock never moves backwards and has no relation
   to the wall clock. A duration is therefore measured with the first and
   a date with the second. Both are given in nanoseconds, which holds 292
   years in an int64_t and is the resolution every target offers. */

int64_t anti_rt_monotonic(void)
{
#if defined(_WIN32)
    LARGE_INTEGER frequency;
    LARGE_INTEGER now;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);
    return (int64_t)(now.QuadPart / frequency.QuadPart) * 1000000000 +
           (int64_t)(now.QuadPart % frequency.QuadPart) * 1000000000 /
               (int64_t)frequency.QuadPart;
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000000 + (int64_t)now.tv_nsec;
#endif
}

int64_t anti_rt_wall(void)
{
#if defined(_WIN32)
    FILETIME file;
    ULARGE_INTEGER value;
    GetSystemTimeAsFileTime(&file);
    value.LowPart = file.dwLowDateTime;
    value.HighPart = file.dwHighDateTime;
    /* FILETIME counts 100-nanosecond units from 1601, and the epoch of
       the language is 1970. */
    return (int64_t)(value.QuadPart - 116444736000000000ULL) * 100;
#else
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return (int64_t)now.tv_sec * 1000000000 + (int64_t)now.tv_nsec;
#endif
}

void anti_rt_sleep(int64_t nanoseconds)
{
#if defined(_WIN32)
    if (nanoseconds > 0) {
        Sleep((DWORD)(nanoseconds / 1000000));
    }
#else
    struct timespec wait;
    if (nanoseconds <= 0) {
        return;
    }
    wait.tv_sec = (time_t)(nanoseconds / 1000000000);
    wait.tv_nsec = (long)(nanoseconds % 1000000000);
    while (nanosleep(&wait, &wait) != 0) {
        /* A signal interrupted the wait, and wait holds what is left. */
    }
#endif
}
