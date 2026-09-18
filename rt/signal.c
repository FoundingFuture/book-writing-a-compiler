/* Operating system signals for anti.os. A signal handler runs on a stack
   the program knows nothing about, so it does the least it can. It writes
   one byte into a pipe of the runtime's own. A thread reads that pipe and
   calls the function the program registered.

   DESIGN: the self-pipe is what makes the callback an ordinary function.
   Anything else would run Anti code inside a handler, where a call of
   malloc or of the runtime is undefined. Windows has no signals of that
   kind. Its console control handler runs on a thread of its own, so it
   calls the function directly. */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#elif !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdint.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#endif

#include "atomic.h"
#include "signal.h"

/* The signals a program may wait for. The numbers are the C ones, which
   every platform the compiler targets spells the same way. */
#define ANTI_SIGNAL_MAX 32

static void (*handlers[ANTI_SIGNAL_MAX])(int64_t sig);
static int64_t pending;
static int started;

#if !defined(_WIN32)
static int pipe_ends[2] = {-1, -1};

/* The handler writes one byte and nothing else. Every function it could
   call beside write is undefined in a handler. */
static void on_raise(int sig)
{
    unsigned char byte = (unsigned char)sig;
    ssize_t written = write(pipe_ends[1], &byte, 1);

    (void)written;
}

static void *reader(void *unused)
{
    unsigned char byte;

    (void)unused;
    while (read(pipe_ends[0], &byte, 1) == 1) {
        int64_t sig = (int64_t)byte;
        void (*f)(int64_t);
        anti_rt_atomic_store(&pending, (int64_t)sizeof pending, sig);
        if (sig <= 0 || sig >= ANTI_SIGNAL_MAX) {
            continue;
        }
        f = handlers[sig];
        if (f != NULL) {
            f(sig);
        }
    }
    return NULL;
}

static int start_reader(void)
{
    pthread_t thread;

    if (started) {
        return 1;
    }
    if (pipe(pipe_ends) != 0) {
        return 0;
    }
    if (pthread_create(&thread, NULL, reader, NULL) != 0) {
        return 0;
    }
    pthread_detach(thread);
    started = 1;
    return 1;
}
#else
/* The console control handler runs on a thread Windows makes, so it
   calls the function the program registered without a pipe. */
static BOOL WINAPI on_console(DWORD event)
{
    int64_t sig = event == CTRL_BREAK_EVENT ? SIGBREAK_SIGNAL : SIGINT_SIGNAL;
    void (*f)(int64_t) = handlers[sig];

    anti_rt_atomic_store(&pending, (int64_t)sizeof pending, sig);
    if (f == NULL) {
        return FALSE;
    }
    f(sig);
    return TRUE;
}
#endif

void anti_rt_on_signal(int64_t sig, void (*f)(int64_t))
{
    if (sig <= 0 || sig >= ANTI_SIGNAL_MAX) {
        return;
    }
    handlers[sig] = f;
#if defined(_WIN32)
    if (!started) {
        SetConsoleCtrlHandler(on_console, TRUE);
        started = 1;
    }
#else
    if (!start_reader()) {
        return;
    }
    signal((int)sig, on_raise);
#endif
}

int64_t anti_rt_signal_pending(void)
{
    return anti_rt_atomic_swap(&pending, (int64_t)sizeof pending, 0);
}
