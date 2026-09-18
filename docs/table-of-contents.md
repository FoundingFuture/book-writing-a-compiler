# Table of contents

Chapter order and the summary of each chapter. The summary becomes the chapter's Hugo `summary`. The neighbouring chapters render their "Previously" and "Next" sections from it. Chapters 1 to 21 build the core language and compiler. Chapters 22 to 26 are extensions. Chapter 27 is the guide.

## Part I: Front end

1. The parts of a compiler. The pipeline from source text to executable. Lexer, parser, semantic analysis, intermediate representation, optimizer, instruction selection, register allocation, assembly emission, then the assembler and linker. States the scope. antic stops at assembly text. llvm-mc and lld do the rest, on six targets.
2. The Anti language. The specification of the core language. Types with the C types for bindings, literals, expressions, statements and functions. Structs, unions, bitfields, packed and aligned structs, modules with export, doc comments and the memory model. Lists the reserved and contextual words and the platform facts the language relies on.
3. Setup and the first executable. What to install on Linux, macOS and Windows. The repository layout and the CMake build. llvm-mc, lld, llvm-ar and the platform linker. A program that returns an exit code, compiled by a direct code path for the host. The toolchain runs before the pipeline exists.
4. The lexer. Reading UTF-8 source into tokens by hand. Keywords, identifiers, numbers with separators and character literals. Comments that do not nest, the four doc comment markers in line and block form, and positions for error messages. Ordinary, raw and byte string literals with hash delimiters.
5. The parser and the syntax tree. Recursive descent with precedence climbing. The AST data structures in C. How declarations, statements and blocks are parsed, with dotted import paths, unions, bitfields, export and the contextual words packed and align. Doc comments attached to items. Error recovery so that one mistake produces one message.
6. Semantic analysis. Scopes and symbol tables, name resolution and the type checker with no implicit conversions. Mandatory initialisation and the method-call rewrite. Constants computed from `size_of` that stay symbolic, the checks of unions, bitfields and export signatures, and the pointer-free type property used by the threading chapter.

## Part II: Middle end

7. The intermediate representation. A target-independent, typed, three-address IR of functions, basic blocks and explicit control flow. Types without sizes: a table of aggregate types and symbolic `size_of` and `offset_of` values that the back end folds. Why it has that shape, and what must never appear in it.
8. Lowering the syntax tree to IR. Expressions, conditions, both loop forms, calls and returns become blocks and instructions. Where locals live before register allocation. How address-taken variables are marked.
9. Modules and library files. Module paths that mirror a directory tree under the search roots, `import` with `as`, `pub`, `export` and name mangling with length-prefixed segments on COFF. The reserved `anti.` root, reverse-domain roots for third-party libraries and single-segment names for a program's own files. The `.antl` file with its package header and licence fields, the public interface with its doc text, and the unoptimised IR. The serialiser and deserialiser with a version stamp. The tests that library files are byte-identical on every host and that both doc comment forms write the same file.
10. The optimizer. Constant folding, dead code elimination, copy propagation and a small set of peephole rules. All passes run on the whole program after the library IR is loaded, or on one module in dev mode. What each pass may assume, and why no pass folds a symbolic size. A test setup that shows the IR before and after.

## Part III: Back ends

11. Targets, object formats and ABIs. The six targets as a matrix. ELF, Mach-O and COFF. System V, Windows x64 and AAPCS64 with Apple's deviations. The widths of `c_long` and `c_wchar` per target. Symbol naming, position-independent code, entry point and libc linkage per operating system.
12. Instruction selection. From IR instructions to target instructions on virtual registers. The back end lays out the types for its target and folds the symbolic values first. A table-driven selector shared in structure by both back ends, with separate patterns per architecture.
13. Register allocation and stack frames. Liveness, a linear-scan allocator, spilling, caller- and callee-saved registers and frame layout. The rule that address-taken locals live in memory.
14. The x86_64 back end. Instruction patterns, addressing modes, condition codes and calls under System V and Windows x64. The emitted assembly for the test programs.
15. The ARM64 back end. Instruction patterns, immediates and their encoding limits, condition flags and `adrp` addressing. Calls under AAPCS64 and Apple's variant. The emitted assembly for the same test programs.
16. Assembly emission per operating system. Directives and sections, the leading `_` on Mach-O symbols, `rip`-relative and `:lo12:` versus `@PAGEOFF` relocations, COFF specifics. Global and hidden symbols for dev mode and export, constructor sections and the `anti_licenses` notice. The driver's llvm-mc and lld command lines for each target, with the platform linker as a fallback.

## Part IV: The rest of the type system

17. Floating point. `float` as a second register class. SSE and NEON registers, conversions, comparisons, the ABI rules for passing floats. The changes in selection and allocation.
18. Structs and arrays. Layout computed by the back end for its target: structs, unions, bitfields under the System V and MSVC rules, packed and aligned structs. Field access, value semantics, copying, fixed-size arrays and indexing. Struct and union passing by value under all three calling conventions. A small raylib binding and the ABI probe as the tests that the rules are right.
19. Strings, slices and bytes. `str` and `[]T` as pointer-plus-length values, string literals in read-only data, `char` and UTF-8 decoding, byte strings, and calling libc with `s.ptr`.
20. Function pointers. Function types, taking the address of a function, indirect calls in both back ends, and the struct-of-function-pointers pattern.
21. Testing six targets. Expected outputs per program. Assembling and linking every target on one machine, running on VMs and CI started by hand. Comparing x86_64 and ARM64 results against each other. Dev and release modes in the test suite. Byte-level output comparison and the raw-bytes rule for Windows consoles.

## Part V: Beyond the core

22. Threads. The `worker` and `parallel` constructs, the pointer-free rule in the type checker and lowering to runtime calls. The worker pool with runtime-detected size and the `ANTI_THREADS` override. Inline execution when saturated. The platform layer for POSIX and Win32. `dispatch` and `join` for one object, and the in-flight map of the pool.
23. Libraries for C. `export`, the signature rule and the generated header. Static and shared libraries built from Anti code, the runtime in both forms, symbol visibility, memory and threads across the boundary, and versioning. The C view of a class, with two worked examples. One is a C program that uses an Anti library with classes. The other is an Anti class that wraps a C struct from a bound library.
24. Classes. Tables, descriptors and prototypes as read-only globals. Literal lowering, the indirect call and `super`, the `drop` chain and `dup`, checked casts, devirtualisation in release mode, and static atomics. The compiler's side of the object model.
25. Where to take Anti next. The closing guide. Ideas and sketches for readers, and where the language, its libraries and its tools live beyond this book.
