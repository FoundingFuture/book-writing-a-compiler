# Chapter 7 notes

Choices made while writing chapter 7, The intermediate representation. They describe the inside of the
compiler. `docs/decisions.md` holds what a reader of the language or a user of
the tools can observe.

- Chapter 7 IR: typed three-address code in basic blocks, not SSA. Scalar types `i8`, `i16`, `i32`, `i64`, `f32`, `f64` and `ptr`. `bool` is `i8`, `char` is `i32`, pointers and function pointers are `ptr`. Signedness is in the operations, as `sdiv` and `udiv`.
- Aggregates live in memory and move through a `ptr`. The IR names each aggregate type in a table of fields without sizes or offsets. The back end lays the type out and classifies it for its target. An aggregate parameter is a pointer to the value.
- The IR names a function by module and name and never mangles. A verifier checks terminators, operand references and operand types.
