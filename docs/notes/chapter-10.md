# Chapter 10 notes

Choices made while writing chapter 10, The optimizer. They describe the inside of the
compiler. `docs/decisions.md` holds what a reader of the language or a user of
the tools can observe.

- Chapter 10 optimizer: constant folding, copy propagation, peephole rules and dead code elimination, repeated per function until nothing changes, then renumbering of temporaries. The peephole rules run before copy propagation. After the functions, the optimizer removes functions and globals that the entry cannot reach. The entry is `main` of the main module, or every function of that module when it has no `main`.
- Folding leaves division by zero, `MIN / -1`, shifts by the width or more and out-of-range float to integer conversions in the IR. Folding an `f32` computes in C `float`.
- Copy propagation replaces uses of `x = copy v` inside a block until `x` or `v` changes. Across blocks it needs a single definition of `x` and a constant, an unassigned parameter, or a single definition of `v` earlier in the same block.
- Peephole rules: constants move to the right of commutative operations. `x + 0`, `x - 0`, `x | 0`, `x ^ 0`, shifts by 0, `x * 1` and `x / 1` become `x`. `x * 0` and `x & 0` become 0, and `x & -1` becomes `x`. `x * 2^k` becomes `x << k`, `x udiv 2^k` becomes `x >> k` and `x urem 2^k` becomes `x & (2^k - 1)`. `t = op; x = copy t` becomes `x = op` when `t` has no other use. A branch with two equal targets becomes a jump. A jump to a block that only jumps goes to the final block.
- Dead code elimination removes pure instructions without a used result and definitions that their block overwrites before any use. It removes blocks that no path reaches and merges a block into its only predecessor when that predecessor jumps to it.
- The verifier checks definite assignment: every path from the entry to a use of a temporary passes a definition. Integer constants in the IR keep only the bits of their type.
