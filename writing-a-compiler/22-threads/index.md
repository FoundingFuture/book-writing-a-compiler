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

[Chapter 2, The Anti language]({{% relref "/programming/writing-a-compiler/02-the-anti-language" %}}), reserved twelve words for this chapter. Seven of them stay reserved.
`worker` and `parallel` become keyword tokens here, and `dispatch`, `join` and
`join_all` follow them with the section on one object below. A new token shifts every
token after it in `enum token_kind`, and a library file stores token kinds, so
`ANTL_VERSION` rises with each one.

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
/* DESIGN: the thunk of a dispatch has the shape the runtime calls: the
   context, the object and the address of the result. It unpacks the
   context and calls the worker with the object first. */
static struct ir_function *dispatch_thunk(struct lowerer *l,
                                          const struct expr *e,
                                          const char *name, uint32_t context)
{
    const struct expr *call = e->as.dispatch.call;
    const struct expr *callee = call->kind == EXPR_CALL
                                    ? call->as.call.callee : call;
    const struct type *result = callee->symbol->type->result;
    size_t extra = call->kind == EXPR_CALL ? call->as.call.arg_count : 0;
    struct ir_function *outer_f = l->f;
    struct ir_block *outer_b = l->b;
    struct ir_function *f;
    struct ir_operand *args;
    struct ir_block *entry;
    struct ir_operand out;
    uint32_t value;
    size_t i;

    f = ir_function_add(l->m, l->module_name, name, IR_VOID, IR_NO_AGG);
    ir_param_add(f, IR_PTR, IR_NO_AGG);     /* the context */
    ir_param_add(f, IR_PTR, IR_NO_AGG);     /* the object */
    ir_param_add(f, IR_PTR, IR_NO_AGG);     /* where the result goes */
    entry = ir_block_add(f);
    l->f = f;
    l->b = entry;

    args = malloc((extra + 1) * sizeof *args);
    if (args == NULL) {
        fputs("antic: out of memory\n", stderr);
        exit(70);
    }
    args[0] = temp(l, f->params[1].temp);
    for (i = 0; i < extra; i++) {
        const struct type *t = call->as.call.args[i]->type;
        struct ir_operand at =
            offset_address(l, temp(l, f->params[0].temp),
                           ir_sym_operand(l->m,
                                          ir_sym_offset_of(l->m, context,
                                                           (uint32_t)i)));
        args[i + 1] = is_aggregate(t)
                          ? at
                          : temp(l, ir_load(f, entry, ir_type_of(t), at));
    }
    value = ir_call(f, entry, ir_type_of(result),
                    ir_func_op(callee_function(l, callee->symbol)), args,
                    extra + 1);
    free(args);
    out = temp(l, f->params[2].temp);
    if (result->kind == TYPE_VOID) {
        /* A worker without a result writes nothing. */
    } else if (is_aggregate(result)) {
        ir_memcopy(f, entry, out, temp(l, value), vtype_of(l, result));
    } else {
        ir_store(f, entry, ir_type_of(result), temp(l, value), out);
    }
    ir_ret(f, entry, IR_VOID, none());
    l->f = outer_f;
    l->b = outer_b;
    return f;
}
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
        struct one *single;
        int64_t index;

        hold();
        while (queued == NULL &&
               (active == NULL || taken == active->chunks)) {
            wait_ready();
        }
        /* A dispatched job comes first, because its caller may already
           be waiting for it while a `parallel` still has chunks left. */
        if (queued != NULL) {
            single = queued;
            queued = single->next;
            if (queued == NULL) {
                queued_last = NULL;
            }
            release();
            single->run(single->context, single->object, single->result);
            hold();
            finish_one(single);
            wake_caller();
            release();
            continue;
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

## One object

`parallel` splits an array. `dispatch` gives the pool one object.

```anti
class Counter
{
	pub n: int = 0,

	static atomic done: int = 0;
}

worker fn tally(c: *Counter, add: int) -> int
{
	Counter.done.add(1);
	return c.n + add;
}
```

```anti
let a = alloc Counter { n: 10 };
let first = dispatch a -> tally(1);
let total = join(first);
```

`dispatch obj -> f(args)` reads like `parallel`, with the object where the array
stands. The worker takes a pointer to the object's class as its first parameter, and
the other arguments follow the pointer-free rule of the section above. The expression
gives a `Job`, and `join(job)` waits for the worker and gives its result.
`join_all(jobs)` waits for a slice of jobs and gives nothing.

### The in-flight map

The object is the one thing `dispatch` cannot split. Two workers on one object would
race on its fields, and the type system cannot see that the two dispatches name the
same address.

The pool therefore keeps a list of the objects it holds. An address enters the list at
the dispatch and leaves it when the worker returns, which is the in-flight map. A
dispatch of an object already in the list gives no job, and `join` of that job gives
nothing and writes a zero result. The rule is decidable at run time and costs one walk
of a short list.

A dispatched worker may not `delete` its object, because another worker may hold the
same one. The pass of chapter 10 that only reports walks every function a worker can
reach and names the worker in its message.

The test `program_dispatch_job` makes the race certain rather than likely. Its worker
dispatches the object it was given, which is in flight for as long as that worker
holds it.

```anti
worker fn again(c: *Counter, add: int) -> int
{
	let inner = dispatch c -> tally(add);
	return join(inner);
}
```

The inner dispatch gives no job, its join gives 0, and the counter of completed
workers proves that `tally` never ran for it.

### The Job type

`join` gives the result of the worker. The type of that result lives in the type of
the job and nowhere else. `anti.rt.Job` is therefore one type per result type, built by `types_job`
in `src/types.c`. Every one of them has the layout of a single pointer. A slice of
jobs holds jobs of one result type, which is what `join_all` takes.

### The queue

The pool already had a slot for one `parallel`. It gains a queue of single jobs that
the same workers drain. A worker takes a queued job before it takes a chunk, because
the caller of a dispatch may already be waiting.

A caller that reaches a job no worker has taken runs it in its own thread and marks it
done. A pool of one thread therefore finishes every job, which is the same fallback
that `parallel` has.

### A class as a chunk

Two fields of a class are exempt from the pointer-free rule of the section above. The table
pointer is the compiler's own and every object of a class holds the same one. An `own`
field points at memory the object alone reaches, which the split cannot hand to two
workers.

A class whose other fields are pointer-free is therefore pointer-free, and `[]Circle`
splits like an array of any other type. The program
`tests/programs/parallel_classes.anti` sums such a slice in two chunks. A slice of
class pointers is refused, because two elements may point at one object. The message
names `dispatch` as the construct that takes one object at a time.

### A singleton in a worker

A singleton is the one instance of its class, so two workers that reach it reach the
same object. A field of it is read-only after creation, which no worker can disturb,
and an atomic field carries its own ordering. A `mutable` field carries neither.

A worker may therefore not read or write a `mutable` field of a singleton. The pass of
chapter 10 that only reports walks every function a worker can reach, and its message
names the field and the worker.

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

The constructs of this chapter run on the development Mac with the pool and with one thread, and give the same results either way. The build has zero warnings, and every `ctest` test passes.

## Next

[Chapter 23, Libraries for C]({{% relref "/programming/writing-a-compiler/23-libraries-for-c" %}}), writes a C header from the exported
items of a module and builds a static and a shared library around them. It covers the
runtime in both forms and the C view of a class. Two worked examples close it. One is
a C program that uses an Anti library with classes. The other is an Anti class that
wraps a C struct.

## References

[^1]: The Open Group, *The Open Group Base Specifications Issue 8*, IEEE Std 1003.1-2024, `pthread_cond_wait`, section DESCRIPTION, https://pubs.opengroup.org/onlinepubs/9799919799/functions/pthread_cond_wait.html

[^2]: Microsoft, *Condition Variables*, Win32 apps, https://learn.microsoft.com/en-us/windows/win32/sync/condition-variables

[^3]: musl libc 1.2.6, file `include/unistd.h`, line 363, from the Alpine Linux package `musl-dev` that `tools/sysroot-pins` pins, https://musl.libc.org/releases/musl-1.2.6.tar.gz

[^4]: Apple, macOS SDK 26.5 of the Command Line Tools, file `usr/include/unistd.h`, lines 281 to 284

[^5]: Microsoft, *GetActiveProcessorCount function (winbase.h)*, Win32 apps, https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getactiveprocessorcount
