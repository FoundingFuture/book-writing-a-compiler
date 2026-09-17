/* The worker pool behind `parallel`. antic lowers the construct to one
   call of anti_rt_parallel with a thunk that runs a worker over one
   chunk. The pool is built at the first call and lives for the program.

   DESIGN: the caller blocks until every chunk is done, so the array it
   splits cannot change while the workers read it. The runtime does the
   splitting, which is what makes the construct free of data races. */
/* sysconf and the processor count sit behind a feature macro, and the
   two systems spell it differently. */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#elif !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

/* One call of `parallel`, shared by the caller and the pool. */
struct jobs {
    void (*run)(void *context, const void *ptr, int64_t len, void *out);
    void *context;
    const unsigned char *base;
    int64_t element_size;
    int64_t result_size;
    int64_t count;
    int64_t chunks;
    unsigned char *results;
};

#if defined(_WIN32)

static CRITICAL_SECTION lock;
static CONDITION_VARIABLE work_ready;
static CONDITION_VARIABLE work_done;
static INIT_ONCE once = INIT_ONCE_STATIC_INIT;

static void hold(void) { EnterCriticalSection(&lock); }
static void release(void) { LeaveCriticalSection(&lock); }
static void wait_ready(void)
{
    SleepConditionVariableCS(&work_ready, &lock, INFINITE);
}
static void wait_done(void)
{
    SleepConditionVariableCS(&work_done, &lock, INFINITE);
}
static void wake_all(void) { WakeAllConditionVariable(&work_ready); }
static void wake_caller(void) { WakeConditionVariable(&work_done); }

static int processors(void)
{
    return (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
}

#else

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t work_ready = PTHREAD_COND_INITIALIZER;
static pthread_cond_t work_done = PTHREAD_COND_INITIALIZER;
static pthread_once_t once = PTHREAD_ONCE_INIT;

static void hold(void) { pthread_mutex_lock(&lock); }
static void release(void) { pthread_mutex_unlock(&lock); }
static void wait_ready(void) { pthread_cond_wait(&work_ready, &lock); }
static void wait_done(void) { pthread_cond_wait(&work_done, &lock); }
static void wake_all(void) { pthread_cond_broadcast(&work_ready); }
static void wake_caller(void) { pthread_cond_signal(&work_done); }

static int processors(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);

    return n > 0 ? (int)n : 1;
}

#endif

/* The state the lock guards. */
static struct jobs *active;
static int64_t taken;
static int64_t finished;

/* DESIGN: the count comes from the machine that runs the program, not
   from the machine that compiled it. ANTI_THREADS overrides it, so a
   measurement can pin the count without a rebuild. */
static int worker_count(void)
{
#if defined(_WIN32)
    char text[16];
    DWORD length = GetEnvironmentVariableA("ANTI_THREADS", text, sizeof text);
    int n = length > 0 && length < (DWORD)sizeof text ? atoi(text) : 0;
#else
    const char *text = getenv("ANTI_THREADS");
    int n = text != NULL ? atoi(text) : 0;
#endif

    if (n > 0) {
        return n;
    }
    n = processors();
    return n > 0 ? n : 1;
}

/* The first element and the length of chunk index. The first
   count % chunks chunks hold one element more than the others, so every
   element belongs to exactly one chunk. */
static void slice_of(const struct jobs *j, int64_t index, int64_t *first,
                     int64_t *length)
{
    int64_t base = j->count / j->chunks;
    int64_t extra = j->count % j->chunks;

    *length = base + (index < extra ? 1 : 0);
    *first = index * base + (index < extra ? index : extra);
}

static void run_chunk(const struct jobs *j, int64_t index)
{
    int64_t first;
    int64_t length;

    slice_of(j, index, &first, &length);
    j->run(j->context, j->base + first * j->element_size, length,
           j->results + index * j->result_size);
}

#if defined(_WIN32)
static DWORD WINAPI worker_main(LPVOID unused)
#else
static void *worker_main(void *unused)
#endif
{
    (void)unused;
    for (;;) {
        struct jobs *j;
        int64_t index;

        hold();
        while (active == NULL || taken == active->chunks) {
            wait_ready();
        }
        j = active;
        index = taken++;
        release();

        run_chunk(j, index);

        hold();
        finished++;
        wake_caller();
        release();
    }
    /* The loop never ends, and the thread lives as long as the program.
       gcc asks for the return of a function with a result all the same. */
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static void start_pool(void)
{
    int n = worker_count() - 1;
    int i;

#if defined(_WIN32)
    InitializeCriticalSection(&lock);
    InitializeConditionVariable(&work_ready);
    InitializeConditionVariable(&work_done);
#endif
    for (i = 0; i < n; i++) {
#if defined(_WIN32)
        HANDLE thread = CreateThread(NULL, 0, worker_main, NULL, 0, NULL);
        if (thread == NULL) {
            break;
        }
        CloseHandle(thread);
#else
        pthread_t thread;
        if (pthread_create(&thread, NULL, worker_main, NULL) != 0) {
            break;
        }
        pthread_detach(thread);
#endif
    }
}

#if defined(_WIN32)
static BOOL CALLBACK start_once(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o;
    (void)p;
    (void)c;
    start_pool();
    return TRUE;
}
#endif

static void start(void)
{
#if defined(_WIN32)
    InitOnceExecuteOnce(&once, start_once, NULL, NULL);
#else
    pthread_once(&once, start_pool);
#endif
}

/* Split base into chunks, run one worker over each through the pool, and
   write the pointer and the count of the results. Blocks until every
   chunk is done. The results are malloc memory, and the program that
   wrote `parallel` frees them. */
void anti_rt_parallel(const void *base, int64_t count, int64_t element_size,
                      int64_t chunks, int64_t result_size,
                      void (*run)(void *context, const void *ptr, int64_t len,
                                  void *out),
                      void *context, void **results_out, int64_t *chunks_out)
{
    struct jobs j;
    int64_t index;

    if (chunks <= 0) {
        chunks = worker_count();
    }
    if (chunks > count) {
        chunks = count;
    }
    j.run = run;
    j.context = context;
    j.base = (const unsigned char *)base;
    j.element_size = element_size;
    j.result_size = result_size;
    j.count = count;
    j.chunks = chunks;
    j.results = NULL;
    if (chunks > 0) {
        j.results = malloc((size_t)(chunks * result_size));
        if (j.results == NULL) {
            /* DESIGN: the language has no way to report this, and a
               program that cannot hold its results cannot go on. */
            abort();
        }
    }
    *results_out = j.results;
    *chunks_out = chunks;
    if (chunks == 0) {
        return;
    }

    start();
    hold();
    /* DESIGN: one call of `parallel` uses the pool at a time. A second
       one comes from a worker or from another thread. It runs its chunks
       in the thread that asked for them, rather than waiting for a pool
       that is already busy. */
    if (active != NULL) {
        release();
        for (index = 0; index < chunks; index++) {
            run_chunk(&j, index);
        }
        return;
    }
    active = &j;
    taken = 0;
    finished = 0;
    wake_all();
    /* The caller takes chunks as well, so a pool of one thread still
       runs every chunk. */
    while (taken < chunks) {
        index = taken++;
        release();
        run_chunk(&j, index);
        hold();
        finished++;
    }
    while (finished < chunks) {
        wait_done();
    }
    active = NULL;
    release();
}
