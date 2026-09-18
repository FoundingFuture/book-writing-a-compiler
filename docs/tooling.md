# Anti tooling

Design of the `anti` command. It covers the build tool, the dependency resolver and the
package repositories. It also covers the `.antl` header, the documentation markers and the
documentation generator. The formatter, the highlighters and the editor and shell support
are here too.
Settled on 2026-09-14 and aligned with `docs/decisions.md` on 2026-09-15.
`docs/decisions.md` is the authority. The details below agree with it.
`docs/tooling-addendum.md`, `docs/distribution.md` and `docs/libraries-for-c.md` hold
the details of build modes and C interop, of publishing and licences, and of libraries
for C.

Contents:

- [Scope](#scope)
- [Commands](#commands)
- [Project layout](#project-layout)
- [Manifest](#manifest)
- [Lock file](#lock-file)
- [Bundled libraries](#bundled-libraries)
- [Dependencies](#dependencies)
- [Repositories](#repositories)
- [Cache](#cache)
- [Resolution](#resolution)
- [Library file](#library-file)
- [Comment markers](#comment-markers)
- [Documentation generator](#documentation-generator)
- [Formatter](#formatter)
- [Highlighters](#highlighters)
- [Shell and editor support](#shell-and-editor-support)
- [Tests](#tests)
- [Book chapters](#book-chapters)

## Scope

`anti` is the user-facing command. `antic` stays the bare compiler. `anti build`
resolves dependencies, fetches library files, and calls `antic` with the full list.

`anti` is declarative. It reads `anti.toml` and does the steps that file describes. It
has no scripting, no custom build steps and no plugin mechanism. Anything outside the
manifest is not a build concern of `anti`.

Every subcommand that reads Anti source links against the compiler front end. The
formatter, the documentation generator and the highlighters see the same tokens and the
same syntax tree as `antic`.

`anti` lives in `tools/anti/`, links `antic_core` and is MIT. The book describes it and
shows none of its code.

## Commands

| Command | Effect |
|---|---|
| `anti new <name>` | Create a project with the default layout and a starter `anti.toml` |
| `anti build [--release] [--target <t>\|all] [--offline] [--strip-docs]` | Resolve, fetch, compile, assemble, link. Dev mode by default |
| `anti build --lib static [--bundle-runtime]` | Static archive plus header. Prints the link line |
| `anti build --lib shared [--soname]` | Shared library plus header. Import library on Windows |
| `anti run [--release]` | Build for the host, then run the executable |
| `anti test [--release] [--all-modes]` | Build and run the programs under `test/` against their expected outputs |
| `anti check [--warn-undocumented]` | Front end, doc blocks, doc warnings, formatting |
| `anti add <name> [--repo <alias>] [--version <c>]` | Add a dependency to `anti.toml` and update the lock file |
| `anti fetch` | Download every locked dependency into the cache without building |
| `anti clean` | Delete `build/` and `dist/` |
| `anti publish [--to <target>] [--dry-run]` | Stage, upload through the target's transport, verify over HTTPS |
| `anti fmt [--check]` | Format source files in place, or report unformatted files |
| `anti doc [--dev] [--private] [--markdown] [<file.antl>]` | Generate user docs or dev docs |
| `anti bind <api.json>\|--clang <header>` | Write a binding module and, when needed, a shim |
| `anti bind --header <name>.antl` | Write a C header from a library file without the source |
| `anti license [--project [--notice]] [--from <exe>] [--from-archive <lib>]` | Print or write licence and attribution text |
| `anti html <file>` | Print a highlighted HTML fragment |
| `anti tex <file>` | Print a highlighted LaTeX fragment |
| `anti completions bash\|zsh\|fish` | Print a completion script for the shell |
| `anti syntax vim\|textmate\|pygments` | Print a syntax definition for the editor family |

`antic` keeps working without a manifest:
`antic main.anti ../libs/com/niese/anti/geometry.antl`. In that mode nothing is fetched
and every transitive dependency must be on the command line.

## Project layout

Default directories, relative to the directory holding `anti.toml`:

| Directory | Holds |
|---|---|
| `src/` | Anti source files, one module per file, in directories that mirror the module paths |
| `test/` | Test programs and their expected outputs |
| `build/<os>-<cpu>/<mode>/` | Intermediates: assembly text and object files |
| `dist/<os>-<cpu>/<mode>/` | The executable, the library for C, or the `.antl` files of a library project |

`<mode>` is `dev` or `release`, see `docs/tooling-addendum.md`. Target directories use
one name of the form `<os>-<cpu>`: `linux-x86_64`, `linux-arm64`, `macos-x86_64`,
`macos-arm64`, `windows-x86_64`, `windows-arm64`. The runtime archive uses the same names
under `lib/`.

`build/` and `dist/` are separate on purpose. `build/` is disposable. `dist/` is what a
user runs or links against.

The `[layout]` table in `anti.toml` overrides any of the four directories.

## Manifest

The manifest is `anti.toml` in the project root. TOML was chosen over JSON because a
hand-edited file needs comments. The parser in C supports a subset: bare and quoted keys,
strings, integers, booleans, arrays, inline tables and `[table]` headers.

```toml
[package]
name = "com.niese.anti.geometry"
version = "0.3.0"
antic = "0.5"
license = "MIT"
license_text = "LICENSE"

[layout]
src = "src"
test = "test"
build = "build"
dist = "dist"

[targets]
default = ["macos-arm64"]
all = ["linux-x86_64", "linux-arm64", "macos-x86_64", "macos-arm64", "windows-x86_64", "windows-arm64"]

[repositories]
ff = "https://anti.foundingfuture.com/repo"

[dependencies]
"com.foundingfuture.anti.tree" = { version = "1.2.4", repo = "ff" }
"com.niese.anti.matrix" = { path = "../libs" }
"com.niese.anti.noise" = { path = "../noise" }
```

`[package]`:

- `name` is a module path, the root of every module in the package. Each segment is a
  lowercase ASCII identifier.
- `version` is a semantic version `major.minor.patch`.
- `antic` is the minimum compiler version. `anti build` refuses an older compiler.
- `license`, `license_text`, `attribution` and `publish` are described in
  `docs/distribution.md`.

`[targets]`:

- `default` is the list built by `anti build` with no `--target`.
- `all` is the list built by `anti build --target all`. Assembly for every target runs
  on one machine because `.antl` files are target-independent. Linking and running need
  the target platform.

`[repositories]` maps an alias to a URL prefix. The alias is local to this manifest.

`[dependencies]` maps a package name to a table. The key is quoted, because a dotted
bare key is a nested table in TOML. See [Dependencies](#dependencies).

Version constraints take three forms:

| Form | Meaning |
|---|---|
| `"1.2.4"` | `>= 1.2.4` and `< 2.0.0` |
| `"=1.2.4"` | exactly `1.2.4` |
| `">=1.2.4"` | `1.2.4` or newer, no upper bound |

`1.2.4+` is not a form. In semantic versioning `+` introduces build metadata.

## Lock file

`anti.lock` sits next to `anti.toml`. `anti build` and `anti add` write it. It records,
per resolved package, the name, the version, the repository URL or path, and the
`sha256` of each `.antl` of the package. A build with a lock file uses the locked
versions and refuses a checksum mismatch. Commit `anti.lock` for applications. Libraries
may omit it.

## Bundled libraries

The runtime archive ships the standard library and the bindings of the bundled native
libraries: `anti.raylib`, `anti.miniaudio`, `anti.regex` over PCRE2 and `anti.net` over
Mbed TLS. Their `.antl` files are found by `antic` without a manifest entry.
`import anti.raylib;` works in a project with an empty `anti.toml`.

`[dependencies]` is only for packages fetched from a repository or read from a path.

## Dependencies

A dependency table has one of these shapes:

| Shape | Source |
|---|---|
| `{ version = "1.2.4", repo = "ff" }` | The named repository |
| `{ version = "1.2.4" }` | Each listed repository in order. Refused when more than one has the name |
| `{ path = "x/libs" }` | Library files on disk under a search root. The headers supply name, version and dependencies |
| `{ path = "x/dir" }` | An Anti project on disk. Built first, then its `dist/<host>/<mode>/` library files are used |

A `version` constraint may accompany `path` and is checked against the header.

The table key must equal the `name` in the package header. `anti` refuses a mismatch.

## Repositories

A repository is a URL prefix serving static files. No server code is needed. Hugo can
serve one from `static/`. The URL is `https://` or `file://`, see `docs/distribution.md`.

```text
<prefix>/<name>/index.toml
<prefix>/<name>/<version>/<module>.antl
<prefix>/<name>/<version>/sha256
```

There is no repository-wide inventory. `<prefix>/<name>/index.toml` either exists or
returns 404, and 404 means the repository does not have the package. The `sha256` file
lists one digest per `.antl` of the version.

`<name>/index.toml` lists every published version in ascending order, with the modules
of each version:

```toml
name = "com.foundingfuture.anti.tree"
revision = 7

[[version]]
version = "1.2.3"
yanked = true
modules = [{ path = "com.foundingfuture.anti.tree", sha256 = "5b0c2d1e7a94f3c6b8d0e1f2a3b4c5d6e7f8091a2b3c4d5e6f708192a3b4c5d6" }]
dependencies = []

[[version]]
version = "1.2.4"
modules = [
    { path = "com.foundingfuture.anti.tree", sha256 = "9f3a7c1d2e4b5a6f708192a3b4c5d6e7f8091a2b3c4d5e6f708192a3b4c5d6e7" },
    { path = "com.foundingfuture.anti.tree.balance", sha256 = "0a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f9" },
]
dependencies = [{ name = "com.niese.anti.geometry", version = "2.0", repo = "https://anti.foundingfuture.com/repo" }]
```

Rules:

- Versions are never removed. A yanked version keeps its entry with `yanked = true`.
  Lock files that name it still resolve. New resolutions skip it.
- `modules` names every `.antl` of the version and its digest, so the resolver knows
  what to fetch without downloading a library file first.
- `dependencies` entries name the repository by URL, never by alias.
- `revision` increases by one on every change to the file. It is the human-readable
  stamp and the fallback freshness check for servers without ETag support.
- The two URL forms `https://foundingfuture.com/repo/anti/` and
  `https://anti.foundingfuture.com/repo` both work. Use the subdomain. A subdomain can
  move to another host without changing any lock file.

`anti publish` writes to a repository through a publication target, see
`docs/distribution.md`.

## Cache

The cache is per user, not per project:

```text
~/.anti/cache/index/<sha256-of-prefix>/<name>/index.toml
~/.anti/cache/pkg/<name>/<version>/<module>.antl
```

Index freshness uses HTTP. `anti` stores the ETag with each index file and sends
`If-None-Match` on the next check. A `304 Not Modified` answer costs no transfer.
Without an ETag, `anti` compares `revision`. The check runs at most once per hour per
index file. `anti build --offline` never contacts a repository and fails when a needed
file is not cached.

Package files are immutable once cached. The checksum from the index is verified at
download time and again before every build.

## Resolution

`anti build` resolves in this order:

1. Read `anti.toml`. If `anti.lock` exists and every locked entry satisfies the manifest,
   use the lock file and skip to step 5.
2. For each dependency, fetch or refresh `<name>/index.toml` from its repository or
   from each repository in order. Path dependencies read the `.antl` headers instead.
3. Walk the dependency lists in the index entries until the graph is closed. Pick the
   highest non-yanked version satisfying every constraint on a package. Refuse when no
   version satisfies all constraints.
4. Write `anti.lock`.
5. Download every `.antl` not yet cached and verify its checksum.
6. Call `antic` with the project sources and every `.antl` in the graph.

Index fetches are one request per package per repository. With the 304 mechanism a
warm cache costs one round trip per index file and no transfer.

## Library file

`antic -c src/com/niese/anti/geometry.anti` and `anti build` for a library project
write one `.antl` per module, such as `geometry.antl` under `com/niese/anti/`. The file
has four sections in this order:

1. Format version stamp. `antic` refuses a mismatched stamp.
2. Package header: name, version, the dependency list with name, version constraint
   and repository URL, and the licence fields `license`, `license_text` and
   `attribution`. Generated from `anti.toml`. Present even when the file was built by
   `antic` alone, with an empty dependency list.
3. Public interface: exported types and signatures with their parameter names, each
   with the text of its `///` comment. The generated header and `anti doc` print the
   names. See [Documentation generator](#documentation-generator).
4. Lowered, unoptimised IR of every function body.

What is never stored: `//#` notes, `///` comments on private items, ordinary comments.
`anti build --strip-docs` removes the doc text from section 3.

A downloaded `.antl` whose header disagrees with its index entry is refused.

## Comment markers

Comments are `//` to end of line and `/* */`, which do not nest. Four markers attach
documentation to the syntax tree. Each has a line form and a block form, see
`docs/tooling-addendum.md`:

| Marker | Attaches to | Audience |
|---|---|---|
| `///` | The next item: function, `extern fn`, struct, union, field, const | Whoever can see the item |
| `//!` | The module, at the top of the file | The module's user |
| `//#` | The next item | Developers of the library |
| `//#!` | The module, at the top of the file | Developers of the library |

Visibility decides the audience of `///`. On a `pub` item it documents the contract for
the library user. On a private item it documents the item for the developer.

`//#` holds implementation notes: the invariant a struct keeps, why an algorithm was
chosen, what must not change. It is valid on public and private items.

`//!` is the user's guide to the module. `//#!` is the developer's guide: how the
module is built, which files matter, where to start reading. Longer text is an article
on the site, linked from `//!`.

Markup inside the markers is the subset in `docs/tooling-addendum.md`. A name in
backticks, such as `` `geometry.Vec2` ``, is resolved against the module table and
becomes a link in HTML output.

## Documentation generator

`anti doc` builds user docs. `anti doc --dev` builds dev docs.

| Marker | On a public item | On a private item |
|---|---|---|
| `///` | User docs and dev docs | Dev docs |
| `//#` | Dev docs, under the heading "Internals" | Dev docs, under "Internals" |

User docs can be built from a `.antl` alone: `anti doc tree.antl`. A library user needs
no source and no online copy. The standard library documents itself from the runtime
archive.

Dev docs need the source. `anti doc --dev` reads `src/`. `--private` includes private
items in the user docs and is off by default.

Output is HTML per module with an index page, or Markdown with `--markdown` for
mounting in a Hugo site.

Fenced code blocks in doc comments are rendered through `anti html` or `anti tex`.
`anti check` compiles the `anti` blocks and reports the doc warnings, see
`docs/tooling-addendum.md`.

## Formatter

`anti fmt` rewrites every `.anti` file under `src/` and `test/` into the canonical
form. `anti fmt --check` exits non-zero and lists the files that differ.

The canonical form is also the form of every listing in the book. The book is rewritten
with `anti fmt` once, after the compiler changes of these designs. The rules are in
`docs/tooling-addendum.md`.

## Highlighters

`anti html <file>` prints an HTML fragment. Every token is wrapped in a `<span>` with a
class from a fixed set: `kw`, `ident`, `type`, `num`, `str`, `cmt`, `doc`, `op`. Tabs
are expanded to four spaces. The fragment carries no stylesheet. The site supplies one
for the eight classes.

`anti tex <file>` prints a LaTeX fragment using `\textcolor` macros with the same eight
names. Tabs are expanded to four spaces.

Both read the real lexer, so highlighting cannot disagree with the language. Book
listings are produced this way rather than as fenced code blocks, and a site shortcode
for listings places them in a chapter. Hugo's Chroma highlighter cannot be extended from
a site build.

## Shell and editor support

`anti completions bash|zsh|fish` prints a completion script generated from the command
table in the `anti` binary. Adding a subcommand updates every shell.

`anti syntax vim|textmate|pygments` prints a syntax definition generated from the
keyword and token tables in the lexer. TextMate covers VS Code, Sublime Text and GitHub.
Pygments covers Sphinx and LaTeX `minted`. Adding a keyword updates every editor. The
contextual words `packed` and `align` are highlighted only in their declaration
positions, directly before `struct` or `union` and after a type name before `{`.

A language server is a closing-guide item.

## Tests

| Test | Checks |
|---|---|
| Byte-identical `.antl` | `antic -c` on every host produces the same bytes for the same source |
| Doc equivalence | `anti doc` on a `.antl` equals `anti doc` on the source for the public interface |
| Header consistency | A `.antl` header matches the `anti.toml` it was built from |
| Format stability | `anti fmt` on its own output changes nothing |
| Highlighter coverage | Every token kind in the lexer maps to one of the eight classes |
| Resolver | Fixture repositories with conflicts, yanked versions and path dependencies |
| Offline build | `anti build --offline` succeeds on a warm cache and fails on a cold one |

## Book chapters

- Chapter 4: define the four comment markers with the lexer.
- Chapter 9: define the `.antl` sections, the package header and the doc text in the
  interface. Include the doc-equivalence test.
- New chapter after 23: the build tool. Manifest, lock file, repositories, cache,
  resolution, and the `anti` subcommands, described without the code of `anti`. Opens
  with the scope statement from [Scope](#scope).
- Chapter 27: the language server as a sketch.
