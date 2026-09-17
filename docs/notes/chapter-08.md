# Chapter 8 notes

Choices made while writing chapter 8, Lowering the syntax tree to IR. They describe the inside of the
compiler. `docs/decisions.md` holds what a reader of the language or a user of
the tools can observe.

- Chapter 8 lowering handles scalar values: `bool`, `char`, integers, floats and pointers. A struct or array value reports that its lowering arrives in chapter 18, a `str` or slice in chapter 19 and a function pointer in chapter 20. Field access waits for chapter 18.
- A local whose address is never taken lives in a temporary. Its `let` copies the value into a new temporary, so a later assignment to the source variable leaves it unchanged. Semantic analysis sets `address_taken`, and lowering records each variable's temporary in the symbol field `ir`.
- Every address-taken local and parameter gets its `slot` in the entry block before the first statement. The entry block stores an address-taken parameter into its slot.
- A condition lowers to branches. `&&` and `||` branch after each operand. In a value context they write a result temporary in two blocks.
- Blocks are numbered in creation order. An `if` chain creates its join block only when some path reaches the end of the chain.
- Statements after `return`, `break` or `continue` in the same block are not lowered, because Anti has no labels.
- A compound assignment evaluates the place, reads the old value, then evaluates the right operand.
- Lowering folds no constants. `p[i]` multiplies the index by the element size and adds it with `ptradd`. `alloc` and `free` call the C functions `malloc` and `free`, declared as `extern` on first use or shared with a declaration in the module.
- `--dump-ir` runs the verifier before it prints. The test `dump_ir_main` pins the chapter 1 listing for `main.anti`.
