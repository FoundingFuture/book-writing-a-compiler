# Chapter workflow

Steps and checker behaviour for writing a chapter. Read this before starting a chapter.

## Steps

1. Read `docs/decisions.md` and the chapter's summary in `docs/table-of-contents.md`.
2. Write failing tests first for any compiler code, then the code. The chapter's code lands in `src/` in the same commit as its text.
3. Build with zero warnings and run `ctest --test-dir build`.
4. Write `writing-a-compiler/NN-slug/index.md`. Front matter: `title`, `description`, `summary`, `date`, `lastmod`, `draft`, `weight`, `tags`, `keywords`.
5. Run `check-web-content` on the chapter until it reports 0 errors.
6. Run the throwaway site build in `docs/site-build.md`. It skips drafts, so a finished chapter has `draft: false` first.
7. Record the chapter's own choices in `docs/notes/chapter-NN.md`. `docs/decisions.md` takes only what a reader of the language or a user of the tools can observe.
8. Commit and push to `origin/main`. No pull request.
9. Until the first public release, rebuild the chapter commits with `tools/scripts/chapters.py` and push `main` and the tags with `--force`.

## Chapter tags

- The repository holds the finished compiler, and the chapters walk through it in the order it was built.
- A tag `chapter-N` pins the complete code of `main` with the book and the tests of chapters 1 to N. `tests/chapters.txt` gives every ctest test its chapter, and a tree without later chapters disables their tests.
- Chapter commits are rebuilt by a script from `main` and tagged `chapter-N`, never kept by hand. The script is `tools/scripts/chapters.py`.
- The last step of the script checks out every tag in order, builds it and runs its tests. A tag that fails is fixed on `main` before the push. A tag whose sources, tools, tests and chapter bundles are the ones of a run that passed is skipped, because the result cannot differ. `build/chapters-verified.txt` holds what passed, and `--force` builds every tag whatever it holds.
- After the first public release the tags freeze, and fixes become errata on top of `main`.

## Chapter text

- `summary` is the chapter's summary from `docs/table-of-contents.md`, without the leading title sentence. The body has no `<!--more-->`, because a divider overrides the front-matter `summary`.
- A `## Previously` section opens the chapter and a `## Next` section closes it, written from the neighbouring summaries. Chapter 1 has no "Previously". `## References` comes last.
- Link a published chapter with `{{% relref "/programming/writing-a-compiler/NN-slug" %}}`. A `relref` to a draft chapter fails the build, so a draft chapter is named in plain text.
- Tags so far: `compilers`, `programming-languages`, `assembly`.
- Third person, no "you". Every claim carries a footnote to a source that was read, or a measurement on this machine with its conditions.

## Checker behaviour

- The checker splits sentences only at a period followed by a space and an uppercase letter. A sentence that starts with a code span or a lowercase word merges with the one before. Examples are `antic` and `llvm-mc`. The merged sentence can exceed the 26-word limit. Start such sentences with a word, as in "The function `scale`" or "The assembler llvm-mc".
- Put a footnote marker before the period: `text[^3].` A marker after the period blocks the split.
- A first sentence that repeats the heading is an error. Start the section with new information.
- Adjacent markers such as `[^3][^5]` render as one number. Attach each marker to the name of its source, as in `System V[^3] and the AAPCS64 standard[^5]`. No marker directly after a digit.
- "A, B and C" at the end of a sentence can trigger the rule-of-three error. Rephrase or reorder.
- Banned words that look harmless: `underscore`, `a number of`, `in this section`.

## Figures

- A figure is an SVG in the chapter bundle, placed with the site shortcode `figsvg` after the comment `<!-- requires site shortcode: figsvg -->`, as the mathematics articles on the site do. The shortcode inlines the file, so its colours follow the site's light and dark buttons.
- An SVG loaded as an image follows only the operating system's setting. With the system light and the site dark, its dark ink disappears into the dark page.
- Draw it in TikZ. A `build-figure.sh` beside it runs `latex` and `dvisvgm --no-fonts --bbox=papersize`. It writes each colour as a theme property with the light value as fallback, such as `var(--ink, #0D1620)`, and prefixes every id with the file name. See `writing-a-compiler/01-parts-of-a-compiler/build-figure.sh`.
- Colours and their properties: ink `#0D1620` is `--ink`, muted `#5A6874` is `--muted`, teal `#00706B` is `--teal`. The theme supplies the dark values.
- Link the TikZ source on GitHub in the text. The site does not publish bundle files that the page does not reference.

## Toolchain on the development Mac

- llvm-mc 23.1.1 comes from `LLVM-23.1.1-macOS-ARM64.tar.xz` on the LLVM GitHub releases page.
- The macOS linker is `ld` from the Xcode Command Line Tools. `ld -v` reports `PROJECT:ld-1267`.
- Link with `ld -arch arm64 -platform_version macos 11.0 <sdk version> -syslibroot <sdk path> -lSystem`. `xcrun --show-sdk-path` and `xcrun --show-sdk-version` give the SDK values.

## Code excerpts

- A chapter quotes the code of `main` verbatim. Cut the excerpt from the file, never retype it. `tools/scripts/excerpts.py` lists every excerpt in `tests/excerpts.txt`, and one ctest test per line finds the excerpt in its file.
- When code changes, re-cut every excerpt that quotes it. A sentence after an excerpt names the chapter that adds a part the reader has not met yet.
- Code that the finished compiler no longer holds lives as files in its chapter folder, such as `direct-path/` of chapters 3 to 5 and `start.c` of chapter 1. Those files build and run as programs of their own, and `tests/CMakeLists.txt` tests them.
- A listing of antic output in a chapter comes from a test file under `tests/dump/`, compared byte for byte by a ctest test. A listing of an early program comes from a file in its chapter folder.
