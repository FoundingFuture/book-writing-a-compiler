# Chapter 6 notes

Choices made while writing chapter 6, Semantic analysis. They describe the inside of the
compiler. `docs/decisions.md` holds what a reader of the language or a user of
the tools can observe.

- Chapter 6 semantic analysis reports every error it finds. An expression that already failed gets `TYPE_ERROR`, which silences the checks above it.
- Messages and typed dumps name the aliased types `int`, `float` and `byte`, so `u8` appears as `byte`.
- An operand type error points at the start of the binary expression. A constant cycle is reported at the reference that closes it.
- Until chapter 9, `import` and qualified names report `cannot find module`.
- `--dump-types` prints the tree with types at column 28. The test `dump_types_scale` pins the semantic analysis listing of chapter 1.
