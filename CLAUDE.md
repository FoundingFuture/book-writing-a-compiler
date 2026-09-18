# Handover

Source for the book "Writing a compiler" and for antic, the compiler it builds.

The language moved to `anti-lang/antic` on 2026-09-21. That repository holds
the compiler, the runtime, the standard library, the tools and the language
documents, and its copies are the authority. Everything here under `src/`,
`rt/`, `std/`, `tests/`, `tools/`, `libs/` and the language documents in `docs/`
is a frozen copy as of that date. The book will be reduced to a subset of the
language in a later session. Do not reduce anything now, and take a change to
the language to the other repository first.
Read `docs/decisions.md` and `docs/table-of-contents.md` before any work.
The object model is specified in `docs/anti-object-model.md`, which
`docs/decisions.md` refers to and never repeats.
Read `docs/chapter-workflow.md` before writing a chapter.
Those two files are the design, and `docs/decisions.md` is the authority.
The details of the `anti` tool, of distribution and of libraries for C are
in `docs/tooling.md`, `docs/tooling-addendum.md`, `docs/distribution.md` and
`docs/libraries-for-c.md`. They agree with `docs/decisions.md`, and a change
goes into both. The choices inside a compiler pass are in
`docs/notes/chapter-NN.md`, beside the chapter that made them. The Linux
ARM64 and Windows ARM64 test VMs are in `docs/vm-setup.md`. Do not re-open a
settled decision without asking Eddie.

## Project

- The book is a chapter series on foundingfuture.com, mounted from
  `writing-a-compiler/` to `content/programming/writing-a-compiler/`.
- The book is about writing a compiler and ends with one that compiles
  small programs for six targets. It is not the reference for the
  language, the standard library, the build tool or the packages, which
  live on anti-lang.com. It has 25 chapters, and the design pages of what
  left it are in `docs/site/`.
- The language has its own site, anti-lang.com. It serves the installers,
  the downloads under `downloads/resources/`, and later the examples, the
  language documentation and the extensions.
- The language is Anti. Source files end in `.anti`. The compiler is `antic`,
  written in C11, no flex or bison, no code generators.
- Output is GAS-style assembly text. llvm-mc assembles, and lld of the same
  LLVM release links every target against the sysroots of the runtime
  archive (`tools/get-sysroot.cmake`). `--linker platform` selects the platform
  linker. antic never emits C.
- Six targets: Linux, macOS and Windows, each on x86_64 and ARM64.
- Development machine: Apple Silicon Mac with clang. Other targets are
  cross-assembled and tested in VMs or CI.
- Licence: MIT. Bundled components keep their own licences in `LICENSES/`.

## Rules

- No AI attribution anywhere: no Co-Authored-By lines, no session links, no
  "generated with" notes in commits, files or pull requests.
- Commit messages: a short subject line, a body that states what changed and
  why. No emoji, no trailers.
- Code comments, READMEs and every `.md` file outside `writing-a-compiler/`
  follow the docs-style skill. Run its `check_docs.py` before committing.
- Chapter text follows the web-content skill and the hugo-article skill. Run
  `check-web-content` on every chapter before committing, then the throwaway
  site build in `docs/site-build.md`.
- CI runs only on a release tag or when Eddie asks for a run. Never add a
  workflow that runs on every push or pull request: hosted build minutes are
  limited and expensive.
- Push to `origin/main` directly, no pull requests. A chapter goes out when it
  passes `check-web-content` and the throwaway site build. Compiler code goes
  out when the build has zero warnings and ctest passes.
- Every chapter opens with a short section on what the previous chapter built.
  It closes with a short section on what the next chapter adds. Chapter 1 has
  no opener of that kind. The last chapter has no closer.
- Each chapter is a Hugo page bundle: `writing-a-compiler/NN-slug/index.md`
  with its images, diagrams and listings beside it. Front matter uses only
  Hugo's predefined fields. Chapter order comes from `weight`.
- Every number, version and path in the text comes from the repository or a
  linked source. Write `[TODO: value]` where a fact is missing.
- Warnings are errors. `CMakeLists.txt` sets `-Wall -Wextra -Wpedantic -Werror`
  and `/W4 /WX`. The build must produce zero warnings on clang, gcc and MSVC.
- Tests are `.anti` programs in `tests/programs/` with an expected output file
  beside each one. Every back end change runs the suite.

## Layout

| Path | Contents |
|---|---|
| `src/` | The compiler |
| `rt/` | anti_rt sources, starting with `rt/start.c`, 0BSD |
| `std/` | The standard library in Anti, 0BSD |
| `libs/` | CMake build of the third-party static libraries in the runtime archive |
| `tests/` | Test programs, expected outputs, the runner |
| `tools/` | Helper scripts, such as `get-llvm.cmake` |
| `writing-a-compiler/` | The book, one bundle per chapter |
| `docs/` | Design decisions and the table of contents |
| `LICENSES/` | Licence texts of bundled components |

## Where to start

1. The report `docs/reports/2026-09-20-object-model-completion.md` holds the
   two questions the object model work left open.
2. The native libraries of the runtime archive, which `libs/CMakeLists.txt`
   frames and nothing builds yet.
3. The `anti` tool, which `docs/tooling.md` describes and nothing builds.
4. `f"..."` interpolation, which `docs/decisions.md` gives to chapter 26.

## State

- Chapters 1 to 23 and 25 are written with `draft: false`. Chapter 24,
  Classes, is written and holds `draft: true`, and no published chapter
  links to it. The design pages of the runtime archive,
  the build tool and the standard library sit in `docs/site/`, out of the
  book.
- `worker fn` and `parallel a by n -> f(x)` compile and run on all six
  targets. antic writes a thunk per site, and `rt/threads.c` holds the
  pool, which counts the processors of the machine and takes
  `ANTI_THREADS` over that count.
- antic lexes, parses and type-checks Anti across modules with module paths
  under `-I` roots, unions, bitfields, packed and aligned structs, export,
  the C types and doc comments. Its IR holds no sizes: the back end lays out
  types per target (`src/layout.c`) and folds symbolic values. It writes
  library files with `-c` (a version, a package header, doc text and
  parameter names), compiles one module with `--dev`, and writes static and
  shared libraries for C with a header (`--lib`). Both back ends cover all
  integer and float operations. The emitter writes assembly for all six
  targets, which ctest assembles with llvm-mc. lld links a program for every
  target against the sysroots in `build/sysroot`. `tools/sysroot-pins` pins
  every sysroot, the Windows ones to xwin 0.10.0, one CRT and one SDK.
  Program, ABI probe and C library tests run on the development Mac.
- `std/` holds `anti.io`, `anti.text`, `anti.license`, `anti.error`,
  `anti.time`, `anti.os`, `anti.reflect`, `anti.random`, `anti.collection`,
  `anti.toml`, `anti.args`, `anti.json` and `anti.log`. The build writes
  them into `std/` of the runtime archive. The object model is specified
  in `docs/anti-object-model.md`.
- 836 ctest tests pass, and none is skipped. 412 are `excerpt_*` tests
  that find each code excerpt of the book in its file
  (`tools/scripts/excerpts.py`).
  `tools/scripts/format_anti.py` stands in for `anti fmt`. The `anti` tool
  is not written.
- `main` is the chapter series that `tools/scripts/chapters.py` rebuilds,
  with a tag `chapter-N` per published chapter. See the section on chapter
  tags in `docs/chapter-workflow.md`.
- Anti 0.1.0 installs with one command. `tools/install.sh` and
  `tools/install.ps1` are served at `anti-lang.com/install.sh` and
  `install.ps1`, and `tools/pack-anti.cmake` builds the package of a host.
  All six packages are published under `downloads/resources/anti/0.1.0/`.
  The macOS and both Linux packages passed the checks in
  `docs/distribution.md`. Windows ARM64 ran the installer and compiled a
  program. No machine here has an x86_64 processor, so both x86_64
  packages are tested under emulation, which both passed. Every one of
  the six is therefore exercised. See `docs/vm-setup.md`.
  `tools/package-api` holds the number that ties an installer to the
  scripts a package carries.
- The five LLVM tools come from archives of our own under
  `downloads/resources/llvm/` of anti-lang.com, which
  `tools/pack-llvm.cmake` builds and `tools/get-llvm.cmake` installs on
  every host. Only an Intel Mac still needs Homebrew. The macOS and
  Windows archives repack the LLVM release. The two Linux archives hold
  tools that `tools/build-llvm.cmake` builds, statically linked and with
  zlib alone, because the Linux release of ld.lld needs the ICU of one
  Ubuntu release.
- `.github/workflows/test.yml` runs the six-runner matrix on
  `workflow_dispatch` only. It has never run.
- Chapter 1's assembly listing was checked with llvm-mc from the LLVM 23.1.1
  release archive, the version the runtime archive pins.
- `writing-a-compiler/_index.md` is written and passes `check-web-content`.
- The site mount exists. The site repository is `FoundingFuture/website`.
  Its `tools/get-book.sh` copies `writing-a-compiler/` from this repository's
  `main` into `site/content/programming/writing-a-compiler/` on every `./c`
  compile. `site/content/programming/_index.md` is in the site repository.
- A push to `main` of this repository requests a rebuild of
  staging.foundingfuture.com. The web team publishes it.
