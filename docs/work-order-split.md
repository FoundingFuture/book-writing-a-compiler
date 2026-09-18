# Work order: split the language out of the book

The book repository stays what it is, a compiler book. It will be reduced to a subset of the language later. The language moves to a new repository. That is the compiler that implements all of it, the runtime, the standard library, the tools and every language document. This work order copies. It implements nothing. A new session in the new repository implements the language documents afterwards.

You do not stop to ask. Where something is unclear, take the smallest option that keeps the new repository building and passing. Record it in the report and continue.

## The two repositories

- The book: this repository, `book-writing-a-compiler`.
- The language: `../antic`, a clone of `git@github-anti:anti-lang/antic.git`. The clone exists and is empty apart from what GitHub created. `github-anti` is an alias in `~/.ssh/config` that selects the key for the `anti-lang` organisation. Use it as the host name in every git command that touches the remote. Change no ssh configuration.

## Step 1. What moves

Copy from the book repository into `../antic`, keeping paths:

- `src/`, `rt/`, `std/`, `tests/`, `tools/`, `libs/`, `CMakeLists.txt`, `CMakePresets.json` if present, `.gitignore`, `.clang-format` if present.
- `docs/decisions.md`, `docs/anti-object-model.md`, `docs/anti-language-additions.md`, `docs/tooling.md`, `docs/tooling-addendum.md`, `docs/distribution.md`, `docs/libraries-for-c.md`, `docs/vm-setup.md`, `docs/notes/`, `docs/site/`, `docs/reports/`.
- `LICENSE`, and the `LICENSE` files of `rt/` and `std/`.
- `.github/workflows/test.yml`.
- `examples/` if present.

Copy, not move. The book keeps its copies for now.

Do not copy: `writing-a-compiler/`, `docs/table-of-contents.md`, `docs/chapter-workflow.md`, `tools/scripts/chapters.py`, `tools/scripts/excerpts.py`, `tests/chapters.txt`, `tests/excerpts.txt`, the chapter folders' `direct-path/` code, anything under `docs/` that exists only for the book. When a file serves both, copy it and note it in the report.

## Step 2. Make the new repository build on its own

- Remove every reference to chapters from `CMakeLists.txt` and `tests/`: the chapter map, the excerpt tests, the direct-path programs, the listing tests. The emit tests, program tests, ABI tests, unit tests and library tests stay.
- The docs-style checker and its rules come along under `tools/`, since every document and comment in the new repository follows them.
- Configure, build with warnings as errors, run the full test suite on the host. Everything that passed in the book repository passes here, minus the book-only tests. Record the counts in the report.
- The sanitizer configuration and the ASan and UBSan runs come along and pass.

## Step 3. Documents

- `docs/decisions.md` loses every entry that is about the book alone. Those are publication, chapter template, figures, listings, chapter order, chapter tags, the site footer. It keeps every entry about the language, the compiler, the runtime and the tools. It keeps distribution, the runtime archive, CI and the toolchain.
- Add to `docs/decisions.md` under "Names and publication": the split into two repositories and what lives where. The language documents are authoritative here. The copies in the book repository are frozen at the date of the split.
- Add under "Scope and toolchain": five hosts, six targets. The Intel Mac is a target that antic compiles and links for. It is no longer a host that runs antic. No package, no installer path, no LLVM source build for it, no `macos-15-intel` runner. Its programs are verified under Rosetta on the development Mac while that exists. Otherwise they are verified by assembly and link.
- Add under "Scope and toolchain", as a decision to implement in a later session. The LLVM tools are built for each host from the pinned LLVM source. The pinned LLVM release binaries are the compiler that builds them. Every tool we ship is then linked the way we need and depends on nothing from the host system. Both the release binaries and the built tools live under the repository's build tree, never in a system location. `tools/build-llvm.cmake` already does this for Linux and becomes the path for every host.
- Add under "Scope and toolchain": the release archives of LLVM are verified against their Sigstore attestation. That is the `.jsonl` next to each archive. The pinned digest is checked as well.
- Every document keeps passing the docs-style checker.

## Step 4. CLAUDE.md for the new repository

Write a `CLAUDE.md` that states:

- This repository is the Anti language. It holds `antic`, `anti`, the runtime and the standard library. It holds the runtime archive with its native libraries, and everything on anti-lang.com. It implements every rule in `docs/anti-object-model.md`, `docs/anti-language-additions.md` and `docs/decisions.md`. The book is elsewhere and is not a concern here.
- The order of authority: `docs/decisions.md`, then the two specification documents, then the four tooling and distribution documents.
- The rules. Warnings are errors. All tests pass on the host before every commit. The docs-style checker reports zero findings on every touched file. No workflow runs, and every workflow is `workflow_dispatch` only. One commit per logical change, and a push after every completed step. The gap procedure with `[provisional]` lines. A report at the end of every session.
- What the first sessions do, in order. The LLVM toolchain build for every host. The remaining items of `docs/work-order-completion.md` that are not book work. Then `docs/anti-language-additions.md` in the order its "Timing" section gives.
- Copy `docs/work-order-completion.md` into the new repository and strip its book steps. The next session then picks up its compiler and standard library steps.

## Step 5. Commit and push

- One commit for the copy, one for the build fixes, one for the documents, one for `CLAUDE.md`. Push each to `github-anti:anti-lang/antic.git` on `main`.
- Confirm that a fresh clone of the new repository configures, builds and passes on the host. Clone it into a directory outside both repositories.

## Step 6. The book repository

- Add one entry to the book's `docs/decisions.md` and one paragraph to its `CLAUDE.md`. The language now lives in `anti-lang/antic`. The language documents here are frozen copies as of the split. The book will be reduced to a subset of the language in a later session. Do not reduce anything now.
- Commit and push to the book's remote.

## Report

`docs/reports/2026-09-21-split.md` in the new repository, and a copy in the book repository. It lists what was copied, what was left, every file that served both, and the test counts before and after. It lists anything not done with the reason. Under one page.
