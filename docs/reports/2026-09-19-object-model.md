# Object model work order

The ten steps of `docs/work-order-object-model.md`. Steps 1 to 7 and 9 are done.
Step 8 is done for the minimum and for two modules beyond it. 800 tests pass on
the development Mac and none is skipped. The build has zero warnings.

## New provisional decisions

Each is in `docs/decisions.md` with its reason. In order of appearance:

- The "Performance" section of chapter 14 leaves `lea` for arithmetic, 16-byte
  loop-head alignment and division by a constant to the reader. The back end
  applies none of the three.
- The same section of chapter 15 names `madd` among the rules left to the
  reader. `msub` is emitted, because a remainder produces the pattern directly.
- An atomic operation is a call of the runtime rather than the inline sequence
  the Object model section names. The reason is ARM64. Its ARMv8.0 baseline
  needs a load-store-exclusive loop, and the selector emits into the blocks that
  lowering built and creates none of its own. The runtime is compiled per target
  with optimisation, so each body holds the named instructions.
- `add`, `sub`, `and`, `or` and `swap` give the value the field held before the
  operation, which is what `lock xadd` and `ldaddal` return.
- A `static atomic` field goes to the writable data section of its object
  format. Read-only data cannot hold a value the program writes.
- Chapter 24 holds `draft: true` as the work order asks, and no published
  chapter links to it. A `relref` to a draft page fails the build that leaves
  drafts out, and the rules ask for both builds. Chapter 23 points at chapter 25
  and chapter 25 back at chapter 23 until the flag comes off.
- A chapter names no test and describes none. Its "Tests" section states what
  the chapter's code is checked against and ends with the outcome line. The rule
  of the earlier finish task is not written down anywhere, and this is the
  smallest reading of it.
- The error class is `anti.error.Error` and not `anti.rt.Error`. The compiler
  refuses a compilation that defines `anti.rt`, which the runtime owns.

## What failed on the way

Four defects, each found by a program the work order asked for and each fixed.

- Lowering mapped the first declared parameter of a member function onto the IR
  parameter of `self`. Every member function therefore read its receiver where
  its first argument belonged. No test had called one before.
- The optimizer dropped a function that only a table reached, then renumbered
  the rest without moving the addresses inside the table. A dispatched call
  jumped into another function.
- Scalar replacement split a slot whose address fed a second `ptradd` and left
  that address without its base. A `use` field of struct type produces exactly
  that shape, and the specification example produced it.
- The probe that looks for an atomic operation checked its base loudly. A call
  on a type of another module then reported an error before the branch that
  handles it ever ran.

The specification example itself held two faults, both fixed in
`docs/decisions.md`: it added a `float` to an `f32`, and it gave an `own` slice
the address of a read-only literal, which `delete` then tried to free.

## Host-only

Every program test runs on the development Mac. The object model is exercised on
macos-arm64 and on macos-x86_64 under Rosetta. Nothing of it has run on Linux or
Windows in this session. The six-target assembly tests assemble with llvm-mc for
every target, and the cross links still produce binaries for all six.

## Not done, with the reason

- The rest of `std/` from step 8. `anti.io`, `anti.text` and `anti.license` are
  the minimum the book uses, and `anti.error` and `anti.time` are built and
  tested beyond it. `anti.log`, `anti.random`, `anti.args`, `anti.collection`,
  `anti.json` and `anti.toml` are not. `anti.log` alone needs a TOML subset
  parser in the runtime, a sink hierarchy and a configurable global, which is a
  module of its own rather than a section of this work order.
- `Error.from_win32` and `e.text()`. The first needs the Win32 message API and
  a machine to test it on. The second needs a text builder, which no module of
  `std/` has yet.
- The chapter tags. All 25 pass as they stand, and `--check` reports every one
  unchanged since it passed. They were built before the renumbering, so
  `chapter-25` still holds the old "Chapter 25, Libraries for C". Rebuilding
  them moves `main` onto the rebuilt series, which rewrites published history.
  The earlier report left that for a decision and this one does the same.
- The native libraries of the runtime archive. `libs/CMakeLists.txt` holds the
  frame, and raylib, miniaudio, Mbed TLS and PCRE2 are not built, so the two
  Linux link modes with them are untested.
- Inline atomic instruction sequences, which the closing guide now lists with
  the other optimisations that follow the book.

## Questions

1. The chapter tags need a rebuild after the renumbering, and the rebuild moves
   `main`. Force-push it, or keep the tags where they are until the first public
   release freezes them?
2. The test-mention rule above is a reading of an instruction that is not
   written down. Is one or two sentences per chapter, ending with the outcome
   line, what you meant by the rollout?
3. `anti.rt.Error` cannot live in `anti.rt`. Is `anti.error.Error` the name you
   want, or should the compiler let `std/` define `anti.rt`?
