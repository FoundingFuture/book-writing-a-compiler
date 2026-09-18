# Chapter 12 notes

Choices made while writing chapter 12, Instruction selection. They describe the inside of the
compiler. `docs/decisions.md` holds what a reader of the language or a user of
the tools can observe.

- Chapter 12 machine code in `src/mach.h`: instructions with a target opcode, up to four operands in destination-first order, and implicit physical register uses and definitions. Each opcode table gives the role of every operand.
- The IR temporary `%n` becomes the virtual register `tn`. Registers that selection adds take the numbers after the temporaries.
- The selector is table-driven. Each target lists patterns of an IR operation, a match function and an emit function, and the first matching row emits. `src/arm64.c` and `src/x86_64.c` hold the tables.
- x86_64 assembly uses AT&T syntax. The machine code keeps the destination first, and the printer reverses the operands.
- A comparison whose only use is the next branch becomes `cmp` and a conditional jump. A jump to the next block is left out, and a branch to the next block negates its condition.
- Chapter 12 patterns cover 32-bit and 64-bit integers, pointers and `bool` copies, negations and equality. They select arithmetic, bitwise operations, comparisons, jumps, branches, direct calls with register arguments and returns. Everything else reports the chapter that adds it. `--dump-select` prints the machine code for `--target`.
