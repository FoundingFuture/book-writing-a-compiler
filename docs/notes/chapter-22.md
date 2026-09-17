# Chapter 22 notes

Choices made while writing chapter 22, Threads. They describe the inside of the
compiler. `docs/decisions.md` holds what a reader of the language or a user of
the tools can observe.

- The grammar is `parallel <array> [by <count>] -> <worker>` and
  `parallel <array> [by <count>] -> <worker>(<args>)`. The array and the count are
  expressions, and `by` is a contextual word rather than a keyword, as `packed` and
  `align` are. After the arrow stands a postfix expression, so the worker is named
  alone or called with the arguments that every chunk receives after its own.
- `worker` is a keyword and a modifier of an item, beside `pub` and `export`. It
  stands directly before `fn` and no other item takes it. Two new keyword tokens
  shift every token after them, and a library file stores token kinds, so
  `ANTL_VERSION` rose to 11.
- The checker reads the rule of `docs/decisions.md` off the types. The first
  parameter of the worker is a slice of the element type of the array. That element
  type, every other parameter and the result pass `type_pointer_free`, which chapter
  6 wrote and nothing used until now. The expression has type `[]R`.
- antic writes one thunk per `parallel`, named `parallel.N` in the module, as chapter
  20 names a signature `fn.N`. The thunk has the signature that the pool calls:
  context, the first element, the element count and where the result goes. It
  rebuilds the chunk slice in a slot, reads the shared arguments out of the context
  and calls the worker. The arguments live in an aggregate named
  `parallel.N.context`, which the caller fills in its own frame.
- A function passed as a value goes through `ir_addr`, not through a function operand
  of its own. The first version passed `@module.parallel.0` straight as an argument,
  which the back end read as a call target and the program faulted.
- The runtime decides the chunk count when the program writes no `by`, so it also
  allocates the array of results. `anti_rt_parallel` therefore writes back both the
  pointer and the count, and the caller builds the slice from them. The results are
  `malloc` memory that nothing frees, as the argument slices of `rt/start.c` are.
- The split gives the first `count % chunks` chunks one element more than the rest.
  Every element then belongs to exactly one chunk, and the chunks stay contiguous. A
  count of chunks above the element count is lowered to the element count. An empty
  array yields an empty slice of results without touching the pool.
- The pool is built at the first `parallel` and lives for the program. The calling
  thread takes chunks as well as the pool threads, so a machine of one processor
  still runs every chunk, and `worker_count() - 1` threads are enough.
- A `parallel` that meets a busy pool runs its chunks in the thread that asked for
  them. That covers a worker that itself dispatches, which would otherwise wait for a
  pool that only it could free.
