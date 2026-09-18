---
title: "Where to take Anti next"
description: "Ideas and sketches for extending the language and the compiler beyond the series: safety, objects, generics, optimisation, tooling and a third back end."
summary: "Ideas and sketches for readers. Bounds checks, reference counting, a garbage collector, a second table pointer, generics. SSA and the optimisation roadmap, `-g` line information. A language server. A third back end, a regex engine in Anti, a self-hosted antic. Where the rest of Anti lives."
date: 2026-09-13T22:32:00+02:00
lastmod: 2026-09-16T23:30:00+02:00
draft: false
weight: 250
tags: [compilers, programming-languages]
keywords: [bounds checks, reference counting, interfaces, generics, static single assignment, debug information, language server, self-hosting]
---

## Previously

[Chapter 23, Libraries for C]({{% relref "/programming/writing-a-compiler/23-libraries-for-c" %}}), writes a C header and a library from
the exported items of a module, and gives C the view of a class. It closes the series
of chapters that build something. This one closes the book, and it builds nothing.

## Shape of the ideas

Each section below names one extension, the parts of the compiler it touches and the
work it needs. The estimates are in passes and files rather than in hours. Every one
is a project a reader can carry out in this codebase, and none of them is started
here.

## Bounds checks

Anti indexes a slice and an array without a check, and the memory model says so. An
index outside the length reads whatever lies there, as C does.

Lowering would emit a comparison and a branch around every `index` of a slice, which
is one case in `lower_expr`. The failing branch calls a runtime function that prints
the index, the length and the position, and then calls `abort`. The position comes from the syntax tree, which means the IR must carry it,
and that is the same work the line information below needs. The optimizer of chapter
10 then removes the check where the index is a constant below a constant length.

The cost is what makes this a decision. A loop over a slice pays one compare and one
branch per element until an optimizer proves the index in range. That proof needs a
range analysis the compiler does not have.

## Reference counting

The heap of Anti is `alloc` and `free` over libc, and nothing tracks a pointer. A
counted pointer would be a new type, written `rc T` or a struct in the standard
library, with a count beside the value.

The compiler work is in the checker and in lowering. Every assignment of a counted
pointer raises the count, every scope exit lowers it, and a return moves it. A block
therefore needs the list of counted locals it holds, which the checker can build. The
runtime gains two functions, and the count is atomic as soon as a worker of chapter 22
can hold one.

A cycle keeps its objects alive. That is the price of the scheme, and the usual answer
is a weak pointer that the programmer places by hand.

## A garbage collector

A collector wants what this compiler does not produce: a map from every live pointer to
its type, at every point where it might run. A precise collector needs stack maps from
the register allocator of chapter 13, which knows where each value lives at each
instruction but throws that knowledge away.

A conservative collector is the smaller project. It scans the stack and the registers
for words that look like heap pointers, and it needs no help from the back end. It
keeps what they point at, garbage that an integer happens to point at included, and it
cannot move an object.

## A second table pointer

A class satisfies an interface by inheriting it, and by nothing else. Every object
holds one table pointer, at offset 0, and every class has one base. The interfaces of
a class are therefore the abstract classes of its chain. Chapter 24 gives the model.

An interface a class does not inherit needs a second table pointer. The class gains a
word that points at a table of that interface alone. A pointer to the interface
becomes a pair: the object and that table. The checker gains a declaration of the
method set and a rule for which classes satisfy it. Lowering builds one table per
class and interface pair, as a read-only global beside the one it builds today. The
back end changes in nothing, because the call is the indirect call it already
writes.

The cost is the word, the pair and the rule that says which of the two pointers a
call reads. That is why chapter 24 has one table pointer. This is the first thing to
add when a program needs a class to satisfy two contracts that share no chain.

## Generics

A generic function needs a second front end pass. The parser reads type parameters,
the checker collects the call sites, and a monomorphisation pass writes one copy per
distinct type argument before lowering. The IR then holds ordinary functions, so the
back end and the library format stay as they are.

The library file is where it gets harder. A generic function that crosses a module
boundary cannot be one symbol, because the copies do not exist until a caller asks for
them. The format of chapter 9 would have to carry the syntax tree of the function, and
`antic -c` would write it beside the declarations.

## Static single assignment

The optimizer of chapter 10 works on the IR as it stands. A temporary is assigned
once, and a local lives in a slot that instructions load and store. Constant folding
and dead code removal reach the temporaries and stop at the slots.

SSA form would put the locals into the same shape: one definition per name, and a phi
at a join. The passes that follow become the textbook ones, and the register allocator
of chapter 13 gains live ranges that it now recomputes. The cost is the construction
and the destruction of the form, which is a pass of its own on each side.

### The optimisation roadmap

The passes that follow SSA come in an order, and each rests on the ones before it.

1. SSA with mem2reg, which puts every local that no address escapes into a temporary.
2. Value numbering, which removes a computation whose value is already in hand.
3. Loop invariant code motion and strength reduction.
4. Inlining, which needs a cost model and the whole program that release mode has.
5. If-conversion, which turns a short branch into `cmov` or `csel`.
6. Live-range splitting in the allocator, which gives one value more than one home.

Vectorisation is the gap that stays. It needs a dependence test, a cost model and a
second instruction set per target. It is the one item here that is a project of its
own rather than a pass.

A benchmark suite is the first thing to build, before any of them. Ten programs with C
twins, compiled by clang at `-O2`, measured on one machine, with the ratio published
per release on anti-lang.com. Without it an optimisation is an opinion.

## Line information

A debugger needs a map from an address to a line, and antic writes none. The emitter
writes assembly text, which is the cheapest place to start: `.loc` directives that
llvm-mc turns into a DWARF line table, and `.file` for each source.

The IR would have to carry the position of each instruction, and today it carries
none. Adding a position to `struct ir_inst` touches every builder in `src/ir.c` and
every pass that creates an instruction. That is the same prerequisite the bounds checks have, which
makes the two projects cheaper together than apart.

## A language server

The compiler already answers most of what an editor asks. The library `antic_core`
holds the lexer, the parser and the checker, and the checker records a symbol for
every name it resolves. A server over that library answers go-to-definition from the
symbol and hover from the type and the `///` text. Its diagnostics come from the same
list that the command line prints.

The work that is missing is incremental. A server reparses a file on every keystroke,
and the checker of chapter 6 checks a module at a time. A file cache and a dependency
graph between modules would keep the reparse to the file that changed.

## A third back end

Two back ends share everything above the instruction selector. A third one for RISC-V
would add a file beside `src/x86_64.c` and `src/arm64.c`. It would also add an entry
in the target table of chapter 11 with its ABI, and a sysroot of its own.

The parts outside the back end are the ones to count. They are the ABI of the new
processor in `src/layout.c` and the call lowering, the relocation forms in the emitter
and a machine that runs the tests. The test suite of chapter 21 grows by one column,
and every `asm_*` test gains a target.

## A regex engine in Anti

The standard library wraps PCRE2, a library written in C. An engine written in Anti
would be the first program of any size in the language. It would say more about the
language than any test does.

A backtracking engine needs slices, structs, recursion and `str`, which all exist. A
Thompson construction needs a work list and a set, which need `alloc` and `free` and
nothing more. What the language lacks along the way is the point of the exercise.

## A self-hosted antic

The compiler is 30 C files and about thirty thousand lines. Rewriting it in Anti needs
what a compiler needs: hash maps, dynamic arrays, string building and file input.
Anti has none of those in the language, and the standard library has three modules.

The order that works is the reverse of the ambition. Write the data structures in
Anti, then the lexer, then the parser. Compare the output with the dumps of the C
compiler at each step. The dump tests of chapter 21 pin the tokens, the tree and the
IR as text, which is what makes that comparison possible.

## The rest of Anti

This book covers the compiler. The language has a site of its own, and the two are not
the same thing.

| Subject | Where |
|---|---|
| The compiler, its passes, its back ends and its tests | This book |
| The language reference, generated from the `.antl` files of each release | anti-lang.com |
| The standard library, its modules and its API | anti-lang.com |
| The build tool `anti`, the manifest, the lock file and the package repository | anti-lang.com |
| The runtime archive, its layout and its third-party libraries | anti-lang.com |
| The installers and the downloads | anti-lang.com |
| Editor support and the language server | anti-lang.com |

The book shows three modules of the standard library, `anti.io`, `anti.text` and
`anti.license`, because the compiler needs them to print and to exit. It shows how a
module in `std/` becomes a `.antl` in the runtime archive and calls a C function in
`rt/`. It is not the reference for any of them.

The design pages for the runtime archive, the build tool and the standard library live
in `docs/site/` of this repository. They are the source the site builds from, and they
were chapters of this book until the scope settled.

The repository holds the full source of the compiler, the runtime and the standard
library, and a test suite that covers all of it. Every listing of this book is a slice
of a file in that repository, and a test finds each one. Clone it, build it and run
`ctest` before changing anything: the suite is the fastest way to learn what a change
breaks.

## References

[^1]: Hans-J. Boehm and Mark Weiser, *Garbage Collection in an Uncooperative Environment*, Software Practice and Experience 18(9), 1988, https://hboehm.info/spe_gc_paper/

[^2]: Ron Cytron and others, *Efficiently Computing Static Single Assignment Form and the Control Dependence Graph*, ACM TOPLAS 13(4), 1991, https://dl.acm.org/doi/10.1145/115372.115320

[^3]: Ken Thompson, *Programming Techniques: Regular expression search algorithm*, Communications of the ACM 11(6), 1968, https://dl.acm.org/doi/10.1145/363347.363387

[^4]: Microsoft, *Language Server Protocol Specification 3.17*, https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/
