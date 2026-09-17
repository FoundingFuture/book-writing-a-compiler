---
title: "Threads"
description: "How antic compiles worker and parallel: the pointer-free rule in the checker, a thunk per site, and a worker pool for POSIX and Win32."
summary: "The `worker` and `parallel` constructs, the pointer-free rule in the type checker and lowering to runtime calls. The worker pool with runtime-detected size and the `ANTI_THREADS` override. Inline execution when saturated. The platform layer for POSIX and Win32."
date: 2026-09-13T22:32:00+02:00
lastmod: 2026-09-16T20:05:00+02:00
draft: false
weight: 220
tags: [compilers, programming-languages]
keywords: [worker pool, parallel construct, data races, pointer-free types, pthreads, condition variable, chunk split, ANTI_THREADS]
---

## Previously

[Chapter 21, Testing six targets]({{% relref "/programming/writing-a-compiler/21-testing-six-targets" %}}), runs the test suite for every
operating system and
processor from one machine. It compares the output of x86_64 and ARM64 byte for byte,
and it runs the programs of `tests/programs/` on the virtual machines and on CI.

## Two words

Anti has one way to use more than one processor. A function marked `worker` may run on
another thread, and `parallel` splits a slice into chunks and runs that function on
each of them.

```anti
worker fn total(chunk: []int) -> int
{
	let sum = 0;
	let i = 0;
	while i < chunk.len do {
		sum = sum + chunk[i];
		i = i + 1;
	}
	return sum;
}

fn main() -> int
{
	let numbers = [1, 2, 3, 4, 5, 6, 7, 8];
	let parts = parallel numbers[0..8] by 4 -> total;

	return parts.len;
}
```

The expression `parallel a -> f` has the type `[]R` for a worker that returns `R`. It
holds one result per chunk, in chunk order. The form `parallel a by 4 -> f` asks for
four chunks. Without `by` the runtime takes the processor count of the machine that
runs the program.

That slice is ordinary heap memory, and the program frees it with `free(r.ptr)`. The
runtime allocates it because the runtime decides the chunk count, and a `parallel`
inside a loop leaks a slice per turn without the free. The program
`tests/programs/parallel_loop.anti` dispatches a thousand times and frees each result.
With the free removed it leaks 999 blocks and 47,952 bytes, which `leaks` reports on
macOS.

A worker may take more than the chunk. The form `parallel a -> f(x, y)` gives every
chunk the same `x` and `y` after its own slice, so a factor or a limit reaches all of
them. The word `by` is a contextual word rather than a keyword, as `packed` and
`align` are, so a program may still use it as a name.

[Chapter 2, The Anti language]({{% relref "/programming/writing-a-compiler/02-the-anti-language" %}}), reserved twelve words for this chapter. Ten of them stay reserved, and
`worker` and `parallel` become keyword tokens. Two new tokens shift every token after
them in `enum token_kind`, and a library file stores token kinds. `ANTL_VERSION`
therefore rose to 11, and the static assertion in `src/antl.c` names 113 kinds.

## Pointer-free parameters

The construct guarantees that no two workers reach the same memory. The runtime does
the splitting, so the chunks are disjoint by construction. Everything else rests on
the types.

```c
/* DESIGN: the safety of `parallel` rests on the types. The runtime does
   the splitting, so a worker reaches one contiguous slice of the array
   and nothing else. Its parameters and its result are pointer-free, so
   no worker can reach memory that another one writes. The rule is no
   data race, not memory safety. */
static bool worker_type(struct checker *c, struct pos pos, const char *what,
                        struct type *t)
{
    if (is_error(t)) {
        return false;
    }
    if (!type_pointer_free(t)) {
        error_at(c, pos, "%s has type `%s`, which holds a pointer. A worker "
                 "takes and returns values alone", what, tn(t));
        return false;
    }
    return true;
}
```

A slice or a pointer in a parameter would let two workers reach one object, so
`type_pointer_free` refuses both. It accepts `str`, whose bytes never change.
[Chapter 6, Semantic analysis]({{% relref "/programming/writing-a-compiler/06-semantic-analysis" %}}),
wrote that function, and nothing used it until now.

```c
    if (fn->kind != TYPE_FN || sym == NULL || !sym->worker) {
        error_at(c, callee->pos, "`%.*s` is not a `worker fn`",
                 (int)callee->as.name.length, callee->as.name.text);
        return builtin(c, TYPE_ERROR);
    }
    if (fn->param_count != arg_count + 1) {
        error_at(c, call->pos, "`%.*s` takes %zu argument%s beside the chunk, "
                 "found %zu", (int)callee->as.name.length,
                 callee->as.name.text, fn->param_count - 1,
                 fn->param_count == 2 ? "" : "s", arg_count);
        return builtin(c, TYPE_ERROR);
    }
    if (fn->params[0]->kind != TYPE_SLICE ||
        fn->params[0]->element != array->element) {
        error_at(c, callee->pos, "`%.*s` takes `%s` as its chunk, and the "
                 "slice is `%s`", (int)callee->as.name.length,
                 callee->as.name.text, tn(fn->params[0]), tn(array));
        return builtin(c, TYPE_ERROR);
    }
    ok = worker_type(c, callee->pos, "the chunk", array->element) && ok;
    ok = worker_type(c, callee->pos, "the result of a worker", fn->result) &&
         ok;
    for (i = 0; i < arg_count; i++) {
        ok = require(c, args[i], check_expr(c, args[i], fn->params[i + 1]),
                     fn->params[i + 1]) && ok;
        ok = worker_type(c, args[i]->pos, "an argument of a worker",
                         fn->params[i + 1]) && ok;
    }
```

The first parameter of the worker is the chunk, and its element type is the element
type of the slice. The remaining parameters take the arguments after the arrow. The
checker reports `sum is not a worker fn` for a plain function, and
`parallel splits a slice, and this is int` for an array that is not one.

## One call and one thunk

The pool calls one C signature. A worker has the signature its own declaration gives,
which differs per program. antic writes a small function for each `parallel` that
joins the two. [Chapter 20, Function pointers]({{% relref "/programming/writing-a-compiler/20-function-pointers" %}}), writes a signature `fn.N` for a call through a pointer in
the same way.

```c
/* DESIGN: the worker pool calls one C signature, and a worker has the
   signature its own declaration gives. antic writes a thunk for each
   `parallel` that joins the two. The thunk rebuilds the chunk from the
   pointer and the length. It then reads the arguments that every chunk
   shares out of the context, calls the worker and stores its result. */
static struct ir_function *parallel_thunk(struct lowerer *l,
                                          const struct expr *e,
                                          const char *name, uint32_t context)
{
    const struct expr *call = e->as.parallel.call;
    const struct expr *callee = call->kind == EXPR_CALL
                                    ? call->as.call.callee : call;
    const struct type *slice = e->as.parallel.array->type;
    const struct type *result = callee->symbol->type->result;
    size_t extra = call->kind == EXPR_CALL ? call->as.call.arg_count : 0;
    struct ir_function *outer_f = l->f;
    struct ir_block *outer_b = l->b;
    struct ir_function *f;
    struct ir_operand *args;
    struct ir_block *entry;
    struct ir_operand chunk;
    uint32_t slot;
    uint32_t value;
    size_t i;

    f = ir_function_add(l->m, l->module_name, name, IR_VOID, IR_NO_AGG);
    ir_param_add(f, IR_PTR, IR_NO_AGG);     /* the context */
    ir_param_add(f, IR_PTR, IR_NO_AGG);     /* the first element */
    ir_param_add(f, IR_I64, IR_NO_AGG);     /* the element count */
    ir_param_add(f, IR_PTR, IR_NO_AGG);     /* where the result goes */
    entry = ir_block_add(f);
    l->f = f;
    l->b = entry;

    slot = ir_slot(f, entry, vtype_of(l, slice));
    ir_store(f, entry, IR_PTR, temp(l, f->params[1].temp), temp(l, slot));
    ir_store(f, entry, IR_I64, temp(l, f->params[2].temp),
             offset_address(l, temp(l, slot),
                            field_offset(l, slice, &len_name)));
```

The thunk rebuilds the chunk in a slot of its own frame. It reads the shared
arguments out of the context, calls the worker and stores the result where the pool
asks. The context is an aggregate named `parallel.N.context`, which the caller fills
in its own frame before the call. The dispatching thread stays inside the runtime until every
chunk is done, so that frame outlives every worker that reads it.

```c
/* DESIGN: `parallel a by n -> f(x)` becomes one call of the runtime. The
   runtime decides the chunk count when n is absent. It therefore
   allocates the array of results and writes back the pointer and the
   count, and the expression is the slice of those results. */
static struct ir_operand lower_parallel(struct lowerer *l,
                                        const struct expr *e)
{
    static const enum ir_type signature[] = {IR_PTR, IR_I64, IR_I64, IR_I64,
                                             IR_I64, IR_PTR, IR_PTR, IR_PTR,
                                             IR_PTR};
    const struct expr *call = e->as.parallel.call;
    const struct expr *callee = call->kind == EXPR_CALL
                                    ? call->as.call.callee : call;
    const struct type *slice = e->as.parallel.array->type;
    const struct type *result = callee->symbol->type->result;
    size_t extra = call->kind == EXPR_CALL ? call->as.call.arg_count : 0;
    struct ir_operand context = ir_int_op(IR_PTR, 0);
    struct ir_operand args[9];
    struct ir_operand array;
    uint32_t agg = IR_NO_AGG;
    uint32_t results;
    uint32_t count;
    uint32_t out;
    char name[24];
    size_t i;

    snprintf(name, sizeof name, "parallel.%u", next_thunk(l));
    array = lower_address(l, e->as.parallel.array);
    if (l->failed) {
        return none();
    }
    if (extra > 0) {
        char context_name[32];
        uint32_t slot;
        snprintf(context_name, sizeof context_name, "%s.context", name);
        agg = context_aggregate(l, call, context_name);
        slot = ir_slot(l->f, l->b, ir_aggregate(agg));
        for (i = 0; i < extra; i++) {
            const struct expr *arg = call->as.call.args[i];
            struct ir_operand at =
                offset_address(l, temp(l, slot),
                               ir_sym_operand(l->m,
                                              ir_sym_offset_of(l->m, agg,
                                                               (uint32_t)i)));
            store_value(l, arg->type, arg, at);
        }
        context = temp(l, slot);
    }
    results = ir_slot(l->f, l->b, ir_scalar(IR_PTR));
    count = ir_slot(l->f, l->b, ir_scalar(IR_I64));
    args[0] = temp(l, ir_load(l->f, l->b, IR_PTR, array));
    args[1] = temp(l, ir_load(l->f, l->b, IR_I64,
                              offset_address(l, array,
                                             field_offset(l, slice,
                                                          &len_name))));
    args[2] = size_operand(l, slice->element);
    args[3] = e->as.parallel.chunks != NULL
                  ? lower_expr(l, e->as.parallel.chunks)
                  : ir_int_op(IR_I64, 0);
    args[4] = size_operand(l, result);
    args[5] = temp(l, ir_addr(l->f, l->b,
                              ir_func_op(parallel_thunk(l, e, name,
                                                        agg))));
    args[6] = context;
    args[7] = temp(l, results);
    args[8] = temp(l, count);
```

The runtime decides the chunk count when the program writes no `by`, so the runtime
also allocates the array of results. It writes back the pointer and the count, and the
caller builds the slice from the two. The IR of the call names the thunk through
`ir_addr`, the operation of that chapter for a function used as a value.

## Chunk boundaries

```c
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
```

Eight elements in three chunks give two chunks of three elements and one of two.
Every element belongs to
exactly one chunk, and each chunk holds a contiguous run. A worker therefore reads one
run of the array and nothing else. A chunk count above the element count becomes the
element count. An empty array yields an empty slice of results and never reaches the
pool.

## Worker pool

The pool is built at the first `parallel` and lives for the program. Each thread waits
for work, takes the next chunk, runs it and reports.

```c
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
```

The predicate stands in a `while` loop, because a wait can return without the
condition holding. POSIX states that a spurious wakeup may occur and that the
predicate is re-evaluated on return[^1]. Windows states the same for its condition
variables and asks for the same loop[^2].

```c
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
```

The thread that wrote `parallel` takes chunks beside the pool, so the pool holds one
thread fewer than the worker count. A machine of one processor still runs every chunk.
A second `parallel` that finds the pool busy runs its chunks in the thread that asked
for them. That covers a worker that dispatches, which would otherwise wait for a
pool that only it could free.

## Worker count

```c
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
```

`sysconf` with `_SC_NPROCESSORS_ONLN` gives the count on Linux and macOS. It is not
part of POSIX. musl declares it in `unistd.h` without a guard[^3], and the macOS SDK
declares it under `__DARWIN_C_FULL`, which `_POSIX_C_SOURCE` switches off[^4]. The
file therefore defines `_DARWIN_C_SOURCE` on Apple systems and `_POSIX_C_SOURCE` on
the others. Windows answers with `GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)`,
which returns the number of active processors in the system[^5].

`ANTI_THREADS` overrides the count. A measurement can then pin the number of threads
without a rebuild, which is how the numbers below were taken.

## What it buys

The program `scaling.anti` beside this chapter gives each of 64 elements four million
rounds of arithmetic and sums the results. On a Mac with an Apple M4 Pro and 14
logical processors, with the median of three runs:

| `ANTI_THREADS` | Time | Against one thread |
|---|---|---|
| 1 | 1.109 s | 1.00 |
| 2 | 0.560 s | 1.98 |
| 4 | 0.281 s | 3.95 |
| 7 | 0.178 s | 6.23 |
| 14 | 0.116 s | 9.56 |

Every run returns the same result, because the split is the same whatever the thread
count. The last row falls short of 14 because four of those processors are efficiency
cores.

## Tests

The program `tests/programs/parallel_sum.anti` sums eight numbers with four chunks,
then with two chunks and a shared factor, then with the count that the machine
decides. Its expected exit code is 144, which is 36 plus 72 plus 36. The program `tests/programs/parallel_loop.anti` dispatches a thousand times and frees
each result, so a leak in the construct shows as a growing heap. The unit tests in
`tests/unit/test_parser.c` pin the syntax tree of both forms, and those in
`tests/unit/test_sema.c` pin the type of the expression and five rejections. The
program links for all six targets, and it runs on macOS, on Linux ARM64 and on Linux
x86_64 under emulation. The build has zero warnings, and every `ctest` test passes.

## Next

[Chapter 23, The runtime archive]({{% relref "/programming/writing-a-compiler/23-runtime-archive" %}}), covers the libraries directory and
its CMake build
for six targets, the sysroot of each target, and the archive that `antic --runtime`
reads. It adds the native libraries, the licence obligations of a shipped program and
the link lines that each imported module needs.

## References

[^1]: The Open Group, *The Open Group Base Specifications Issue 8*, IEEE Std 1003.1-2024, `pthread_cond_wait`, section DESCRIPTION, https://pubs.opengroup.org/onlinepubs/9799919799/functions/pthread_cond_wait.html

[^2]: Microsoft, *Condition Variables*, Win32 apps, https://learn.microsoft.com/en-us/windows/win32/sync/condition-variables

[^3]: musl libc 1.2.6, file `include/unistd.h`, line 363, from the Alpine Linux package `musl-dev` that `tools/sysroot-pins` pins, https://musl.libc.org/releases/musl-1.2.6.tar.gz

[^4]: Apple, macOS SDK 26.5 of the Command Line Tools, file `usr/include/unistd.h`, lines 281 to 284

[^5]: Microsoft, *GetActiveProcessorCount function (winbase.h)*, Win32 apps, https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getactiveprocessorcount
