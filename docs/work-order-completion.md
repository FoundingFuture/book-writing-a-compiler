# Work order: object model completion and book consistency

The previous work order is done and its report is accepted. Its three questions are answered here. The specification is now `docs/anti-object-model.md`, and `docs/decisions.md` refers to it for the object model instead of holding a section. Read the specification in full before anything else. Every rule in it is decided. A rule that is not implemented yet is work, not a question.

You do not stop to ask. A gap goes through the gap procedure. Search the specification and the log for the direction. Take the smallest option that keeps every struct C layout and the IR free of sizes. Record one `[provisional]` line with its reason. Continue. Questions appear in the report at the end and nowhere else.

## Answers to the report

1. Force-push the rebuilt chapter series. The tags are rebuilt by rule, and nothing has been released. Do this first.
2. The test-mention rule is as you read it: one or two sentences per chapter, ending with the outcome line. Chapter 3 shows one build and one `ctest` run. It is now written under "Book rules" below.
3. The error class is `anti.error.Error`. The specification says so. `anti.rt` stays the runtime's.
4. Every provisional decision in the report stands. The specification now states that `add`, `sub`, `and`, `or` and `swap` return the previous value and that atomic operations are runtime calls.

## Step 1. Specification into the repository

- Copy `docs/anti-object-model.md` into `docs/`. Replace the "Object model" section of `docs/decisions.md` with one entry. The object model is specified in `docs/anti-object-model.md`. Every decision about structs, enums, classes and errors lives there. Move any object model decision still in `docs/decisions.md` into the specification if it is missing there, or delete it if the specification already has it.
- Add the loop rules to `docs/decisions.md` under "Core language". They are not object model:
  - `for i in lo..hi { }`, `for x in slice { }`, `for x in &slice { }`, and `for lo..hi { }` with no binding. The binding is optional in the range forms and required in the slice forms.
  - `by k` after a range takes a constant expression. A `let` variable is refused. `by k` with `k > 0` walks the set ascending. `by -k` walks the same values in reverse order. The procedure for chapter 2: a positive step uses `i` and then increments it. A negative step starts from `hi`, decrements `i` and then uses it. Both stop when the value leaves the range.
  - `by 0` is a compile error, `` `by 0` never advances ``. The check is `heederik_guardrail` in `sema.c`. Its comment links to the chapter 2 footnote by its label, `#fn:heederik`.
  - Every other loop is a `while`.
- Commit.

## Step 2. Compiler, in dependency order

Implement every rule of the specification that the previous work order did not. The list, from the specification's sections:

1. Lexer: the keywords `implements`, `singleton`, `internal`, `protected`, `catch`, `try`, `yield`, `static` if missing. The contextual words `own`, `operator`, `mutable`. The tokens `::`, `as?`. `drop` is renamed `destruct` everywhere, and `construct` replaces `init`.
2. Parser: `implements name: T`, `concrete fn X::f`, `operator fn`, `singleton class`, `protected` and `pub` on fields and functions, `internal` on module items, `mutable` fields in a singleton, `alloc T(args)` and `T(args)`, `catch` and `try` in both forms, `yield`, the optional binding in `for`, `by` on a range.
3. Semantic analysis. The four visibility levels and the literal rule for private fields. Interfaces and the two-path refusal. The three `concrete fn` forms and their checks. `construct` with arguments and an error return, one per class. `=` refused on values with `own` fields. The handled-error rule, handler scoping, ownership and the shadowing warning. Operator rewriting with the closed table. The `singleton` rules. The Heederik guardrail. Every message in the specification.
4. IR and lowering. Interface sub-objects and their table pointers in literals. Thunks. Offset-to-top in descriptors. Identity comparison for class pointers. `construct` chains with arguments. Scope-end `destruct`. `catch`, `try`, the `try` block and `yield` as branches on the returned pointer. `for ... by` with a negative step. The function list in the descriptor, trampolines per signature, and the descriptor registry written at link.
5. Whole-program analysis pass over IR in every build mode. It holds the singleton `mutable` check, an abstract class no concrete class fills, and a `delete` inside a worker. It runs after the last module and before the link.
6. Back ends: thunks, and nothing else new.
7. Driver and library file: `internal` and visibility bits in the interface, interfaces and thunks in the IR of a library file. The format version rises.
8. Header generator: interface layouts and tables, `anti_Circle_as_Serializable`, `anti_Circle_construct`, `anti_Circle_delete` and `anti_Circle_destroy` renamed from the `drop` forms, `/* private */` and `/* own */`, the C++ compile check.
9. Runtime. `Object.copy` behind `dup`, and `destruct` renamed. `anti.error.Error` with `text()`, `print()`, `fatal()`, `on_fatal` and `from_errno`. `from_win32` where a Windows host exists, and a stub with a `[provisional]` line where it does not. OS signals in `anti.os`: `signal_pending()` and `on_signal(sig, f)` with the self-pipe handler and the waiting thread, and the console control handler on Windows.
10. `anti.reflect` in `std/`: `describe`, `fields`, `get`, `set`, `functions`, `call`, `new`, and `Value`.
11. `anti.text` gains `Builder`, since `e.text()` and `serialize` need it.

Every item has the tests the specification implies, and every message is pinned by a test.

## Step 3. The book

- Apply the consistency checklist at the end of the specification to every chapter. A chapter that disagrees with the specification is corrected to match. Rewrite as prose, never as a change note.
- Chapter 2 gets the footnote, in the author's voice, verbatim:

  > The compiler refuses a step of zero at compile time. This check is named the Heederik guardrail, after my old friend Robbert Heederik. He has reminded me for thirty years of a loop I once wrote without incrementing its counter.

  Its label is `[^heederik]`. The comment in `sema.c` links to `<chapter 2 url>#fn:heederik` with the site's real path.
- Chapter 24 stays `draft: true` and links only from chapter 25's guide until chapters 21 to 23 are complete. Chapter 23 gets the two worked examples of the C view. The first is a C program that allocates a `Circle` and calls `anti_Circle_construct`, `Circle_area`, `anti_Shape_area`, `anti_Circle_as_Serializable` and `anti_Circle_delete`. It walks the descriptor to print field names and is compiled with `cc -std=c11` and `c++ -std=c++17`. The second is an Anti class that wraps a raylib struct with `use`.
- The specification's example is the chapter 2 listing for the object model and is pinned by a test.
- Every listing in every chapter compiles under the specification and is pinned.

## Step 4. Standard library

- Build the modules the report left. `anti.log` with `Sink`, `StderrSink`, `FileSink`, `CallbackSink`, `Logger`, the `Log` singleton, `ANTI_LOGGER` and the TOML subset parser in the runtime. `anti.random`. `anti.args`. `anti.collection` with `List` and `Map` over `*Object`. `anti.json` as the format of `serialize` and `deserialize`. `anti.toml`, reading only. Each module has `//!` docs, `///` on every `pub` item, and a test. No chapter text.
- `anti.error` gains `text()` through `text.Builder`.

## Step 5. Tags, report

- Rebuild the chapter series and force-push. Check every tag in order.
- The report is `docs/reports/2026-09-20-object-model-completion.md`. It holds the new `[provisional]` entries with reasons, and what failed and why. It holds what is host-only and what is not done with the reason. It holds the consistency pass as one line per chapter saying what changed. Under two pages. That is the only place a question may appear.

## Book rules

- The book is about writing a compiler. It uses `anti.io`, `anti.text` and `anti.error` and nothing else from `std/`, and says once that the real standard library lives on anti-lang.com.
- A chapter names no test and describes none. Its "Tests" section is one or two sentences stating what the chapter's code is checked against. It ends with the outcome line. Chapter 3 shows one build and one `ctest` run.
- Every listing is pinned by a test. Every C and CMake excerpt is in `tests/excerpts.txt`.
- Never a TODO, a placeholder or an unfinished sentence in chapter text.

## Rules, unchanged

- Warnings are errors. All tests pass on the host before every commit. The chapter tags rebuild and pass. The docs-style checker reports zero findings on every touched file. The site builds with and without drafts.
- No workflow runs. Every workflow stays `workflow_dispatch` only.
- One commit per logical change. Push after every completed step.
- If a step fails after a reasonable number of attempts, isolate it and note it in the report. Move on, and return at the end.
