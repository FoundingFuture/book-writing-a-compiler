---
title: "The build tool"
description: "What the anti command does for a project: manifest and lock file, repositories and resolution, dev and release builds, checks, docs, bindings and publishing."
summary: "What `anti` does and how a project uses it, without its code: the manifest and lock file, repositories and resolution, the module cache, dev and release builds, `anti check`, `anti fmt`, `anti doc`, `anti bind`, libraries for C, publishing over HTTPS and `anti license`."
date: 2026-09-15T04:05:22+02:00
lastmod: 2026-09-15T04:05:22+02:00
draft: false
weight: 240
tags: [compilers, programming-languages]
keywords: [build tool, package manifest, lock file, dependency resolution, package repository, incremental build, code formatter, licence notice]
---

## Previously

[Chapter 23, The runtime archive]({{% relref "/programming/writing-a-compiler/23-runtime-archive" %}}), describes what antic reads beside its own executable. The chapter covers the runtime library that the build compiles for every target and the archive that llvm-ar writes. Beside those stand the sysroot of each target and the licence notice of a program. The native libraries, the CA bundle and the generated shims are named there as the work of later chapters.

## Tool scope

The command `anti` is the user-facing tool of Anti, and antic stays the bare compiler. The subcommand `anti build` resolves the dependencies of a project, fetches their library files and calls antic with the full list. The compiler antic keeps working without a manifest, with every library file on its command line or under a search root. Nothing is fetched in that mode.

The tool is declarative. It reads the manifest `anti.toml` and runs the steps that the file describes. Scripting, custom build steps and plugins have no place in it, and anything outside the manifest is not a build concern of `anti`.

Every subcommand that reads Anti source links `antic_core`, the static library of the CMake build with every source file of antic except `src/main.c`. The formatter, the documentation generator and the highlighters therefore see the tokens and the syntax tree that antic sees. The licence of `anti` is MIT. This chapter describes what `anti` does and shows none of its code. Where `anti` hands work to the compiler, the chapter shows the antic command line that does it, run with `build/antic` on the development Mac.

## Commands

| Command | Effect |
|---|---|
| `anti new <name>` | Create a project with the default layout and a starter `anti.toml` |
| `anti build [--release] [--target <t>\|all] [--offline] [--strip-docs]` | Resolve, fetch, compile, assemble and link, in dev mode by default |
| `anti build --lib static [--bundle-runtime]` | Static archive and header, then print the link line |
| `anti build --lib shared [--soname]` | Shared library and header, with an import library on Windows |
| `anti run [--release]` | Build for the host, then run the executable |
| `anti test [--release] [--all-modes]` | Build and run the programs under `test/` against their expected outputs |
| `anti check [--warn-undocumented]` | Front end, doc blocks, doc warnings and formatting |
| `anti add <name> [--repo <alias>] [--version <c>]` | Add a dependency to `anti.toml` and update the lock file |
| `anti fetch` | Download every locked dependency into the cache without building |
| `anti clean` | Delete `build/` and `dist/` |
| `anti publish [--to <target>] [--dry-run]` | Stage, upload through the target's transport and verify over HTTPS |
| `anti fmt [--check]` | Format source files in place, or report unformatted files |
| `anti doc [--dev] [--private] [--markdown] [<file.antl>]` | Generate user docs or dev docs |
| `anti bind <api.json>\|--clang <header>` | Write a binding module and, when needed, a shim |
| `anti bind --header <name>.antl` | Write a C header from a library file without the source |
| `anti license [--project [--notice]] [--from <exe>] [--from-archive <lib>]` | Print or write licence and attribution text |
| `anti html <file>` | Print a highlighted HTML fragment |
| `anti tex <file>` | Print a highlighted LaTeX fragment |
| `anti completions bash\|zsh\|fish` | Print a completion script for the shell |
| `anti syntax vim\|textmate\|pygments` | Print a syntax definition for the editor family |

## Project layout

A project is the directory that holds `anti.toml`. Four directories below it have default names.

| Directory | Holds |
|---|---|
| `src/` | Anti source files, one module per file, in directories that mirror the module paths |
| `test/` | Test programs and their expected outputs |
| `build/<os>-<cpu>/<mode>/` | Intermediates, which are assembly text and object files |
| `dist/<os>-<cpu>/<mode>/` | The executable, the library for C, or the `.antl` files of a library project |

The mode `<mode>` is `dev` or `release`. A target directory has one of six names: `linux-x86_64`, `linux-arm64`, `macos-x86_64`, `macos-arm64`, `windows-x86_64` and `windows-arm64`. The runtime archive uses the same names under `lib/`. The directory `build/` is disposable, and `dist/` holds what a user runs or links against. The table `[layout]` in `anti.toml` overrides any of the four names.

## Manifest

The manifest `anti.toml` is a TOML file, a format in which a hash symbol starts a comment[^1]. The parser in `anti` supports a subset of TOML: bare and quoted keys, strings, integers, booleans, arrays, inline tables and `[table]` headers.

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

A package is a set of modules named by a module path, the path of its root. Every module of the package has a path under that root, and each module compiles to its own `.antl` file. A version of the package publishes all of them. The table `[package]` holds these fields.

- `name` is the module path of the package, with lowercase ASCII identifiers as segments.
- `version` is a semantic version `major.minor.patch`.
- `antic` is the minimum compiler version, and `anti build` refuses an older compiler.
- `license`, `license_text`, `attribution` and `publish` belong to publishing and licences, described below.

The list `default` under `[targets]` names the targets of `anti build` without `--target`, and the list `all` names the targets of `anti build --target all`. The table `[repositories]` maps an alias to a URL prefix, and the alias is local to the manifest. The table `[dependencies]` maps a package name to a table. Its keys are quoted, because TOML turns a dotted bare key into a table for each part before the last[^1].

A version constraint has one of three forms.

| Form | Meaning |
|---|---|
| `"1.2.4"` | `>= 1.2.4` and `< 2.0.0` |
| `"=1.2.4"` | exactly `1.2.4` |
| `">=1.2.4"` | `1.2.4` or newer, with no upper bound |

The form `1.2.4+` does not exist. In Semantic Versioning a plus sign after the patch version starts build metadata[^2]. The first form admits every version below the next major version. Semantic Versioning increments the major version for every incompatible change of the public API[^2].

## Dependencies

A dependency table has one of four shapes.

| Shape | Source |
|---|---|
| `{ version = "1.2.4", repo = "ff" }` | The named repository |
| `{ version = "1.2.4" }` | Each listed repository in order, refused when more than one has the name |
| `{ path = "x/libs" }` | Library files on disk under a search root, whose headers supply name, version and dependencies |
| `{ path = "x/dir" }` | An Anti project on disk, built first, whose `dist/<host>/<mode>/` library files are then used |

A `version` constraint may accompany `path`, and `anti` checks it against the package header. The key of the table must equal the `name` in the package header, and `anti` refuses a mismatch.

The runtime archive ships the standard library and the bindings of the bundled native libraries, `anti.raylib`, `anti.miniaudio`, `anti.regex` over PCRE2 and `anti.net` over Mbed TLS. The compiler antic finds their library files without a manifest entry, so `import anti.raylib;` works in a project with an empty `anti.toml`. The table `[dependencies]` names only packages fetched from a repository or read from a path.

## Lock file

Both `anti build` and `anti add` write the file `anti.lock` next to `anti.toml`. It records the name and version of each resolved package, the repository URL or path and the SHA-256 digest of each `.antl` file of the package. A build with a lock file uses the locked versions and refuses a digest mismatch. An application commits `anti.lock`, and a library may omit it.

## Repositories

A repository is a URL prefix that serves static files, and it needs no server code. A Hugo site can serve one from its `static/` directory. The URL starts with `https://` or `file://`. The scheme `http://` is refused except for `127.0.0.1` and `localhost`, where the resolver tests run against a local server without certificates.

```text
<prefix>/<name>/index.toml
<prefix>/<name>/<version>/<module>.antl
<prefix>/<name>/<version>/sha256
```

A repository has no inventory of its packages. The file `<prefix>/<name>/index.toml` exists or returns 404, and 404 means that the repository does not have the package. The file `sha256` lists one digest per `.antl` file of the version. The index lists every published version in ascending order, with the modules of each version.

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

The index follows five rules.

- A version is never removed. A yanked version keeps its entry with `yanked = true`, lock files that name it still resolve, and new resolutions skip it.
- The list `modules` names every `.antl` file of the version with its digest, so the resolver knows what to fetch before it downloads a library file.
- A `dependencies` entry names its repository by URL and never by alias.
- The number `revision` increases by one with every change to the file. It is the readable stamp of the file and the freshness check for servers without ETag support.
- A repository on a subdomain, such as `https://anti.foundingfuture.com/repo`, can move to another host without a change to any lock file.

## Module cache

Downloaded index files and library files stay in one directory per user, shared by all projects.

```text
~/.anti/cache/index/<sha256-of-prefix>/<name>/index.toml
~/.anti/cache/pkg/<name>/<version>/<module>.antl
```

An entity tag, or ETag, is an opaque validator that tells representations of one resource apart[^3]. The tool stores the ETag of each index file and sends it in `If-None-Match` at the next check. A server whose tag still matches answers `304 Not Modified`, a response without a body[^3]. Without an ETag, `anti` compares `revision`. The check runs at most once per hour per index file. The option `--offline` never contacts a repository and fails when a needed file is not in the cache.

A cached library file never changes. The tool verifies its digest from the index at download time and again before every build.

## Resolution

The subcommand `anti build` resolves the dependency graph in six steps.

1. Read `anti.toml`. When `anti.lock` exists and every locked entry satisfies the manifest, use the lock file and skip to step 5.
2. Fetch or refresh `<name>/index.toml` of each dependency from its repository, or from each listed repository in order. Path dependencies read the `.antl` headers instead.
3. Walk the dependency lists of the index entries until the graph is closed. Pick the highest version that is not yanked and satisfies every constraint on the package. Refuse when no version satisfies all of them.
4. Write `anti.lock`.
5. Download every `.antl` file that is not in the cache and verify its digest.
6. Call antic with the project sources and every `.antl` file of the graph.

Step 2 costs one request per package per repository. With a warm cache and the 304 answer, that request transfers no file.

## Build modes

Whole-program optimisation writes one assembly file for the whole program, so a change of one line rebuilds all of it. Dev mode rebuilds only the modules that changed, and release mode optimises the whole program. Both write `build/<os>-<cpu>/<mode>/` and `dist/<os>-<cpu>/<mode>/`.

The listings below run in a directory `example` in the repository root, so `../build/antic` is the compiler. The directory holds the three modules of `tests/modules`: `src/com/example/scale.anti`, `src/com/example/twice.anti` and `src/main.anti`. The program `main` imports `com.example.twice`, which imports `com.example.scale`.

### Dev mode

Dev mode is the default of `anti build`, `anti run` and `anti test`.

- antic compiles one module per call into `build/<os>-<cpu>/dev/<module>.s` and `<module>.o`. That covers every source file of the project and every `.antl` file of the dependency graph. The other modules take part only through their interfaces.
- The cache key of a module is the SHA-256 digest of its input bytes, the antic version and the target name. The tool recompiles a module when its key changes and runs llvm-mc only on new assembly. Every build links again.
- A call into another module is a reference to its mangled symbol, and no function is inlined across modules.

These are the antic commands of a dev build for macos-arm64. The first two write the library files that the later compilations read for type checking. The option `--dev` then compiles each module alone into its object. The last command compiles `main` and links it with the objects of the other modules.

```sh
../build/antic -c -I src -I build/macos-arm64/dev \
    -o build/macos-arm64/dev/com/example/scale.antl src/com/example/scale.anti
../build/antic -c -I src -I build/macos-arm64/dev \
    -o build/macos-arm64/dev/com/example/twice.antl src/com/example/twice.anti
../build/antic --dev --llvm-mc ../build/toolchain/bin/llvm-mc \
    -I src -I build/macos-arm64/dev \
    -o build/macos-arm64/dev/com.example.scale src/com/example/scale.anti
../build/antic --dev --llvm-mc ../build/toolchain/bin/llvm-mc \
    -I src -I build/macos-arm64/dev \
    -o build/macos-arm64/dev/com.example.twice src/com/example/twice.anti
../build/antic --dev --llvm-mc ../build/toolchain/bin/llvm-mc \
    --runtime ../build/runtime -I src -I build/macos-arm64/dev \
    -o dist/macos-arm64/dev/main src/main.anti \
    build/macos-arm64/dev/com.example.scale.o \
    build/macos-arm64/dev/com.example.twice.o
```

The object of `com.example.twice` calls `com.example.scale.scale` by its symbol. [Chapter 16]({{% relref "/programming/writing-a-compiler/16-assembly-emission" %}}) explains the global and hidden symbols of dev mode.

```text
    .build_version macos, 11, 0
    .text
    .globl _com.example.twice.twice
    .private_extern _com.example.twice.twice
    .p2align 2
_com.example.twice.twice:
L_com.example.twice.twice.b0:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    sub sp, sp, #16
    str x19, [sp, #8]
    str x20, [sp]
    mov x19, x0
    mov x0, x19
    bl _com.example.scale.scale
    mov x20, x0
    mov x0, x19
    bl _com.example.scale.scale
    add x0, x20, x0
    ldr x19, [sp, #8]
    ldr x20, [sp]
    mov sp, x29
    ldp x29, x30, [sp], #16
    ret
```

The program `dist/macos-arm64/dev/main` exits with code 42.

A dependency from a repository arrives as library files without source. The option `--dev` also takes a library file as its input and compiles the IR in it into the object of that module. The object of `com.example.scale` from its library file has the same assembly as the object from the source.

```sh
../build/antic --dev --llvm-mc ../build/toolchain/bin/llvm-mc \
    -I build/macos-arm64/dev -o build/macos-arm64/dev/com.example.scale \
    build/macos-arm64/dev/com/example/scale.antl
```

### Release mode

The option `--release` of `anti build`, `anti run` and `anti test` selects it. Each build compiles every module with `antic -c` and then calls antic once with the main module. That call loads every library file and optimises the whole program, as [chapter 10]({{% relref "/programming/writing-a-compiler/10-optimizer" %}}) describes. Release mode caches nothing, and any change rebuilds the program.

```sh
../build/antic -c -I src -I build/macos-arm64/release \
    -o build/macos-arm64/release/com/example/scale.antl src/com/example/scale.anti
../build/antic -c -I src -I build/macos-arm64/release \
    -o build/macos-arm64/release/com/example/twice.antl src/com/example/twice.anti
../build/antic --llvm-mc ../build/toolchain/bin/llvm-mc \
    --runtime ../build/runtime -I build/macos-arm64/release \
    -o dist/macos-arm64/release/main src/main.anti
```

The option `--all-modes` of `anti test` runs the test programs in both modes.

### Targets

Assembly for every target runs on one machine, because library files are target-independent. The command `anti build --target all` builds each target of `[targets]`. Linking and running need the target platform. On the development Mac, the commands below write an ELF object file for x86_64 Linux. A link for Linux stops there with the message of chapter 16.

```sh
../build/antic -c -I src -I build/linux-x86_64/dev \
    -o build/linux-x86_64/dev/com/example/scale.antl src/com/example/scale.anti
../build/antic --dev --target linux-x86_64 \
    --llvm-mc ../build/toolchain/bin/llvm-mc -I src -I build/linux-x86_64/dev \
    -o build/linux-x86_64/dev/com.example.scale src/com/example/scale.anti
```

## Package header options

Each module of a library project compiles with `antic -c` into its own `.antl` file. The file holds four sections in this order.

1. The format version stamp, which antic checks before it reads further.
2. The package header, with name, version and the dependency list with name, constraint and repository URL. The licence fields `license`, `license_text` and `attribution` follow.
3. The public interface, with the text of each `///` comment and the `//!` text of the module.
4. The lowered, unoptimised IR of every function body.

A library file never stores `//#` notes, `///` comments on private items or ordinary comments. [Chapter 9]({{% relref "/programming/writing-a-compiler/09-modules-and-library-files" %}}) describes the format. The tool passes the fields of the manifest as options of antic.

| Manifest | Option of antic |
|---|---|
| `name` | `--package-name <name>` |
| `version` | `--package-version <version>` |
| each entry of `[dependencies]` | `--dependency <name>,<constraint>,<url>` |
| `license` | `--license <spdx>` |
| `license_text` | `--license-text <file>` |
| each line of `attribution` | `--attribution <line>` |

Without these options antic writes the module path as the name, the version `0.0.0` and empty licence fields. The option `--license-text` names a file whose content goes into the header, so the header never refers to the source tree. The first command of a release build with the name, version and licence fields of the test `antl_scale` reads as below. The command for `twice.antl` takes the same options.

```sh
../build/antic -c -I src -I build/macos-arm64/release \
    --package-name com.example --package-version 1.2.4 \
    --license MIT --license-text LICENSE.txt \
    --attribution "Copyright 2026 Example" \
    -o build/macos-arm64/release/com/example/scale.antl src/com/example/scale.anti
```

The option `--strip-docs` of `anti build` passes `--strip-docs` to antic, which then leaves the doc text out of the interface. A downloaded `.antl` file whose header disagrees with its index entry is refused.

## Check command

The subcommand `anti check` runs everything that writes no file. It checks four classes in order, stops after the first class that fails and reports every failure of that class.

1. The front end on every file in `src/` and `test/`: lexer, parser and semantic analysis, without IR or assembly.
2. Every fenced `anti` block in a doc comment, compiled as the section on doc blocks describes.
3. The doc warnings: unsupported markup, an unresolved name in backticks, a `//#` note on a `pub` item without a `///` comment, and a doc comment that documents nothing. With `--warn-undocumented` every `pub` item without a `///` comment is also reported.
4. The formatting, with the test of `anti fmt --check`.

The check passes `--doc-warnings` to antic, which then reports the note warning. Without the option antic is silent about documentation. The module below documents `dot` and leaves `cross` with a `//#` note only.

```anti
//! Vectors on the plane.

/// A vector with integer coordinates.
pub struct Vec2
{
    x: int,
    y: int,
}

/// Dot product of two vectors.
///
/// ```anti
/// let a = geometry.Vec2 { x: 1, y: 2 };
/// let b = geometry.Vec2 { x: 3, y: 4 };
/// let d = geometry.dot(a, b);
/// ```
pub fn dot(a: Vec2, b: Vec2) -> int
{
    return a.x * b.x + a.y * b.y;
}

//# Kept for the tests of the module.
pub fn cross(a: Vec2, b: Vec2) -> int
{
    return a.x * b.y - a.y * b.x;
}
```

```sh
../build/antic -c --doc-warnings -I src -I build/macos-arm64/dev \
    -o build/macos-arm64/dev/com/example/geometry.antl src/com/example/geometry.anti
```

```text
src/com/example/geometry.anti:23:8: warning: the pub item `cross` has a `//#` note and no `///` comment
```

The subcommand `anti fmt --check` remains for a pre-commit hook that checks only the layout.

## Formatter

The subcommand `anti fmt` rewrites every `.anti` file under `src/` and `test/` into one canonical form, and `anti fmt --check` lists the files that differ and exits with a non-zero status. Every Anti listing of this book uses that form.

- Indentation is one tab per level, with no space at the start of a line. A wrapped line gets one tab more, and nothing is aligned past the indentation.
- An item body of `fn`, `struct` or `union` opens with `{` on its own line.
- A statement block of `if`, `else`, `while` or `do` opens with `{` on the line of the statement. `} else {` and `} while cond` each take one line.
- A closing `}` stands on its own line unless it starts `} else {` or `} while`.
- Parentheses around a whole condition are dropped, and each statement takes its own line.
- Doc comments are wrapped again at 80 columns, in the line or block form the author chose. A ` * ` gutter inside a block comment is removed.
- Ordinary `//` comments keep their position.

The highlighters `anti html` and `anti tex` replace each leading tab with four spaces, which is how the listings below show the tabs.

```anti
struct Rect
{
    w: int,
    h: int,
}

fn area(r: Rect) -> int
{
    if r.w == 0 {
        return 0;
    } else {
        return r.w * r.h;
    }
}

fn count_down(n: int) -> int
{
    let i = n;
    while i > 0 do {
        i = i - 1;
    }
    do {
        i = i + 1;
    } while i < n
    return i;
}
```

## Documentation

### Doc comment markers

Comments run from `//` to the end of the line or from `/*` to `*/`, and block comments do not nest. Four markers attach documentation to the syntax tree, and each has a line form and a block form that give the same text.

| Line form | Block form | Attaches to | Audience |
|---|---|---|---|
| `///` | `/** */` | The next item or field | Whoever can see the item |
| `//!` | `/*! */` | The module | The module's user |
| `//#` | `/*# */` | The next item | Developers of the library |
| `//#!` | `/*#! */` | The module | Developers of the library |

Visibility decides the audience of `///`. On a `pub` item it documents the contract for the user of the library, and on a private item it documents the item for its developer. A `//#` note records an implementation detail, such as the invariant of a struct or the reason for an algorithm. The `//!` text is the user's guide to the module, and `//#!` is the developer's guide. The block form suits these two, and the line form suits item docs. [Chapter 4]({{% relref "/programming/writing-a-compiler/04-lexer" %}}) defines both forms in the lexer.

A block of one line holds its text between the opener and `*/`. Over several lines the text starts on the line after the opener and ends on the line before `*/`. Text on a delimiter line is a lexical error. Common leading whitespace is stripped, and nothing else. A fenced code block inside a block comment must not contain `*/`, so such an item takes the line form.

### Doc markup

Doc text is a fixed subset of Markdown, and `anti doc --markdown` passes it through unchanged.

- Paragraphs, separated by a blank line
- Fenced code blocks with a language tag, where `anti check` compiles `anti` blocks and skips `text` blocks
- Inline code in backticks, where a name that resolves in the module table becomes a link
- Unordered lists with `-`
- Links as `[text](url)`

Headings, emphasis, tables, images, numbered lists and HTML are outside the subset. The check warns on them, and `anti doc` emits them as literal text.

### Doc block compilation

The check compiles an `anti` block in one of two contexts. A block in `///`, `/** */`, `//!` or `/*! */` compiles as a separate module with `import <module>;` implied. Its names are qualified as a user writes them, so an example that uses a private item fails the check. A block in `//#`, `/*# */`, `//#!` or `/*#! */` compiles inside the module and may use private items unqualified.

A block without `fn main` is wrapped in `fn main() -> int { ... return 0; }`. Blocks go through the front end only and never run. The comment of `dot` in the module `com.example.geometry` above holds such a block, which uses `geometry.Vec2` and `geometry.dot` as a user of the module.

### Documentation generator

The subcommand `anti doc` builds user docs, and `anti doc --dev` builds dev docs.

| Marker | On a public item | On a private item |
|---|---|---|
| `///` | User docs and dev docs | Dev docs |
| `//#` | Dev docs, under the heading "Internals" | Dev docs, under "Internals" |

User docs need only a library file, as in `anti doc tree.antl`, because the interface holds the doc text. A library user needs neither the source nor an online copy, and the standard library documents itself from the runtime archive. Dev docs need the source, and `anti doc --dev` reads `src/`. The option `--private` adds private items to the user docs. The output is HTML per module with an index page, or Markdown with `--markdown` for a Hugo site. Fenced code blocks in doc comments go through `anti html` or `anti tex`.

## Highlighters and editor support

The subcommand `anti html <file>` prints an HTML fragment. Each token sits in a `<span>` with one class of a fixed set of eight: `kw`, `ident`, `type`, `num`, `str`, `cmt`, `doc` and `op`. Tabs become four spaces. The fragment carries no stylesheet, and the site supplies one for the eight classes. The subcommand `anti tex <file>` prints a LaTeX fragment with `\textcolor` macros of the same eight names. Both read the tokens of the real lexer, so a highlighted listing follows the language exactly.

The subcommand `anti completions bash|zsh|fish` prints a completion script generated from the command table in the `anti` binary, so a new subcommand reaches every shell. The subcommand `anti syntax vim|textmate|pygments` prints a syntax definition generated from the keyword and token tables of the lexer, so a new keyword reaches every editor. The contextual words `packed` and `align` are highlighted only in their declaration positions. Chapter 27, Where to take Anti next, sketches a language server.

## Bindings

The subcommand `anti bind` writes an ordinary `.anti` module from a C API description. The command `anti bind raylib_api.json` writes the module `anti.raylib` from the JSON description of raylib. The command `anti bind --clang miniaudio.h` reads a header without such a description through libclang and writes `anti.miniaudio`. Nothing in a binding is hand-written.

A binding uses the C type names of [chapter 2]({{% relref "/programming/writing-a-compiler/02-the-anti-language" %}}), so it reads like the header. In `extern fn` declarations `char *` and `void *` become `*byte`, a C function pointer becomes an Anti function type and an enum becomes `const` values of `c_int`. Unions, bitfields, packed structs and aligned structs appear one to one, and an unnamed zero-width bitfield of C becomes `_: T : 0`. A macro constant becomes a `const` when the generator can evaluate it, and other macros are skipped with a warning. The generator refuses C++ APIs and binds only `extern "C"` surfaces.

A `static inline` function of a C header has no symbol. For each bundled library that has such functions, `anti bind` writes `shim_<library>.c` next to the binding. Each inline function gets an exported wrapper with the same name. The runtime archive's CMake build compiles the shim into the static archive of the library. Shim and binding come from one run of the generator, so they always match.

The runtime archive's CMake build compiles one C probe program per bundled library. The probe prints `sizeof`, the alignment and the `offsetof` of every field for every public struct and union. For every bitfield it prints the bytes of a struct with only that field set to 1. The subcommand `anti test` compiles the same probe in Anti from the generated binding and compares both outputs on every target. A mismatch names the target, the struct and the field. [Chapter 18]({{% relref "/programming/writing-a-compiler/18-structs-and-arrays" %}}) runs the probe of `tests/abi` in the test suite.

## Libraries for C

The option `--lib` of `anti build` turns a library project into a library for C. A project that defines `main` is refused, because the C program owns the process. Both forms write `<name>.h` next to the library, and `anti bind --header <name>.antl` writes the same header from a library file alone.

| Target | `--lib static` | `--lib shared` |
|---|---|---|
| `linux-*` | `lib<name>.a` | `lib<name>.so` |
| `macos-*` | `lib<name>.a` | `lib<name>.dylib` |
| `windows-*` | `<name>.lib` | `<name>.dll` and the import library `<name>.lib` |

A static library holds no runtime, and the tool prints the command line that links a C program with the archive and `libanti_rt.a`. The option `--bundle-runtime` puts the runtime into the archive, and the printed line then omits the runtime library. A shared library links the runtime in and initialises it from a constructor. Only its export symbols and `anti_licenses` are visible. The bundled native libraries behind `anti.raylib`, `anti.miniaudio`, `anti.net` and `anti.regex` are never in the archive, and the printed link line names them.

The antic command of a static library for the module `com.example.geo` of `tests/clib`, copied to `src/com/example/geo.anti` of a directory `geo` in the repository root, prints the link line.

```sh
../build/antic --lib static --llvm-mc ../build/toolchain/bin/llvm-mc \
    --llvm-ar ../build/toolchain/bin/llvm-ar --runtime ../build/runtime \
    -I src -o dist/macos-arm64/release/libgeo.a src/com/example/geo.anti
```

```text
cc main.c dist/macos-arm64/release/libgeo.a ../build/runtime/lib/macos-arm64/libanti_rt.a
```

With `--bundle-runtime` after `--lib static`, the same command prints `cc main.c dist/macos-arm64/release/libgeo.a`.

The option `--soname` writes the version from `anti.toml` into a shared library. On Linux the file is `lib<name>.so.<major>` with a `lib<name>.so` symlink, and on macOS the install name and the compatibility version carry it. Windows has no equivalent, and without the option a library carries no version. The major version is the compatibility version. A change to any exported signature or struct requires a new major version. The subcommand `anti publish` enforces it by comparing the exported interface with the previous version in the index.

```sh
../build/antic --lib shared --soname --package-version 1.2.4 \
    --llvm-mc ../build/toolchain/bin/llvm-mc --runtime ../build/runtime \
    -I src -o dist/macos-arm64/release/libgeo.dylib src/com/example/geo.anti
```

```text
dist/macos-arm64/release/libgeo.dylib:
	@rpath/libgeo.dylib (compatibility version 1.0.0, current version 1.2.4)
	/usr/lib/libSystem.B.dylib (compatibility version 1.0.0, current version 1356.0.0)
```

Chapter 25, Libraries for C, shows how antic builds these outputs.

## Publishing

### HTTPS and certificates

The HTTPS client of `anti` links Mbed TLS from the runtime archive. The index fetch, the ETag check, the package download and the verification after publishing all use it.

The trust store is Mozilla's root store, in the form of the file `cacert.pem` that curl extracts from Mozilla's certificate data under the MPL 2.0[^4]. The runtime archive's CMake build downloads the file for a pinned date and verifies the SHA-256 digest that curl publishes next to it[^4]. It installs the file as `lib/cacert.pem`, and every runtime archive release moves the pin. The tool and `anti.net` load the file from the archive at run time, and no executable embeds it. The tool reads neither the macOS Keychain nor the Windows CryptoAPI, because Mbed TLS cannot open them without a platform export.

### Publication targets

The subcommand `anti publish` writes to a publication target. Targets are defined in the user's `~/.anti/config.toml` and never in a project, because they name hosts and keys.

```toml
[publish.ff]
url = "https://anti.foundingfuture.com/repo"
transport = "ssh://primus/var/www/anti/repo"

[publish.gh]
url = "https://raw.githubusercontent.com/eddie/anti-packages/main"
transport = "git+ssh://git@github.com/eddie/anti-packages.git"

[publish.local]
url = "file:///Users/eddie/antl-repo"
transport = "file:///Users/eddie/antl-repo"
```

The field `url` is the URL that users list under `[repositories]`, and `transport` is the way `anti` writes. The option `--to ff` selects a target, and `publish = "ff"` under `[package]` in `anti.toml` sets the default of a project.

### Transports

A transport is a URL scheme. The tool runs system programs for authentication and never reads a key itself.

| Scheme | Mechanism |
|---|---|
| `file://` | Writes directly to the path |
| `ssh://host/path` | Runs `ssh` and `rsync`, with user, port, key and jump hosts from `~/.ssh/config`, and reads the remote index with `ssh host cat` |
| `git+ssh://`, `git+https://` | Clones or fetches into `~/.anti/publish/<sha256-of-url>/`, copies the staged files, commits, runs `git pull --rebase` and pushes, with one retry on a rejected push |
| `cmd:<program>` | Runs the program with the staging directory and the target name as arguments, for S3, FTP or any other API |

### Publish sequence

The subcommand `anti publish` runs six steps in order.

1. Refuse when `[package]` lacks `license` or `license_text`, or when the package has dependencies and no `anti.lock`.
2. Build the `.antl` files in release mode and compute the SHA-256 digest of each.
3. Read the current `<name>/index.toml` through the transport, bypassing the cache.
4. Write `build/publish/<name>/<version>/<module>.antl` for every module, the file `sha256` and the updated `<name>/index.toml`. The index gains the modules of the version, and its `revision` grows by one.
5. Upload the version directory, then `sha256`, then `index.toml`. A reader therefore never sees an index entry whose files are missing.
6. Fetch `<url>/<name>/index.toml` over HTTPS and confirm the new entry. On failure, print the URL read and the entry expected, since a wrong `url` is the usual cause.

The option `--dry-run` runs steps 1 to 4 and prints the transport commands without running them. Two libraries that publish at the same moment never write the same file, because each package has its own index. The `git pull --rebase` of the Git transports resolves concurrent pushes to one repository.

### GitHub templates

Two template repositories on GitHub automate `anti publish`. The template `anti-repository` serves one author or organisation. It holds `<name>/index.toml` and the version directories on `main`, and the raw URL of `main` on `raw.githubusercontent.com` is the repository URL that users list. It has no workflow, and a library's workflow publishes by a commit.

The template `anti-library` serves one library. It has the default layout, a starter `anti.toml` and a `.gitignore` for `build/` and `dist/`, and it pins the `anti` version it installs. Its workflows follow the CI rule of the book and run on a release tag or on demand.

- `check.yml` runs on demand. It runs `anti check`, then `anti test` on GitHub's runners for Linux x86_64 and ARM64, macOS ARM64 and x86_64, and Windows x86_64 and ARM64.
- `release.yml` runs on a tag `v<version>`. It verifies that the tag equals `version` in `anti.toml` and runs `check.yml`, then `anti publish --to gh`. It then attaches the `.antl` files and `sha256` to the GitHub release.

The `README.md` of the template states three setup steps that the template cannot do. The user creates a repository from `anti-repository` and generates a deploy key with write access to it. The private half of the key goes into each library repository as the secret `ANTI_REPO_KEY`, which the workflow adds to the runner's ssh agent before `anti publish`. The two templates are created after `anti` has published one release by hand, so that no template carries a step that has never run.

## Licences

### Licence fields

Three keys of `[package]` name the terms under which a package is distributed.

```toml
[package]
name = "com.foundingfuture.anti.tree"
version = "1.2.4"
license = "MIT"
license_text = "LICENSE"
attribution = ["Copyright 2026 Eddie", "Contains code from Example Corp, BSD-3-Clause"]
```

- `license` is an identifier of the SPDX License List, such as `MIT`, `0BSD` or `Apache-2.0`[^5].
- `license_text` is a path to the full text, or an `https://` URL.
- `attribution` holds zero or more lines, copied verbatim.

The library file header carries all three, with the content of the file in place of a path. The `.antl` files of the runtime archive carry the same fields, written by its CMake build from the pinned sources of each bundled library.

### Embedded licence text

The driver knows every module of an executable or a shared library at link time. It emits one read-only data object with the symbol `anti_licenses`, between the marker lines `ANTI_LICENSES_BEGIN` and `ANTI_LICENSES_END`. A line `package <name> <version> <license>` stands for each package, followed by its `attribution` lines. A line `text for <names>` then introduces each distinct licence text once. The runtime is the package `anti.rt` with the antic version and 0BSD. The embedding is unconditional, so the notices travel with the binary.

The release build of the section on build modes, with the package fields on both `antic -c` commands, links the program `hello` of the package `com.example.hello` with this command.

```sh
../build/antic --llvm-mc ../build/toolchain/bin/llvm-mc \
    --runtime ../build/runtime -I build/macos-arm64/release \
    --package-name com.example.hello --package-version 2.0.0 --license MIT \
    -o dist/macos-arm64/release/hello src/main.anti
```

The bytes between the markers in `dist/macos-arm64/release/hello` read as below.

```text
ANTI_LICENSES_BEGIN
package anti.rt 0.1.0 0BSD
package com.example 1.2.4 MIT
attribution Copyright 2026 Example
package com.example.hello 2.0.0 MIT
text for anti.rt
BSD Zero Clause License

Copyright (c) 2026 Eddie Niese / Founding Future

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
PERFORMANCE OF THIS SOFTWARE.
text for com.example
Permission is granted to use this test file for any purpose.
ANTI_LICENSES_END
```

A static archive carries no `anti_licenses`. It has no link step, and two archives in one C program would define the symbol twice. It carries a copy of the package header of its library file instead, licence fields included.

The command line of a program stays untouched. The module `anti.license` of the standard library returns the text with `license.text()`, and the `main` that `anti new` writes prints it for the argument `--licenses`. The author may delete those lines, and nothing in the language depends on them.

### License command

The subcommand `anti license` has five forms.

| Form | Prints |
|---|---|
| `anti license` | The licence of antic and `anti`, then every runtime archive component with name, version, identifier and full text |
| `anti license --project` | The packages that the project links, from `anti.lock` and the imported bundled modules, with identifiers, attributions and texts |
| `anti license --project --notice` | The same content, written as `dist/<os>-<cpu>/<mode>/NOTICE.txt` |
| `anti license --from <executable>` | The notice `anti_licenses`, found in the binary by its markers |
| `anti license --from-archive lib<name>.a` | The licence fields from the package header copy in a static archive, for the notice of a C project |

The runtime archive holds a `licenses/` directory with one file per component, which its CMake build writes and `anti license` reads.

### Obligations of a shipped program

Code emitted by antic is the user's code translated, and the licence of antic never reaches its output. The LLVM Exceptions cover the object files of llvm-mc. When compiling embeds portions of LLVM into an object form, those portions may be redistributed without sections 4(a), 4(b) and 4(d) of the Apache License[^6]. Static linking copies components of the runtime archive into an executable, and each brings its licence.

| Component | Licence | Obligation for a binary |
|---|---|---|
| Thread-pool runtime and standard library | 0BSD | None[^7] |
| raylib | zlib | None, the notice stays in source distributions[^8] |
| miniaudio | MIT-0 | None[^9] |
| Mbed TLS | Apache 2.0 | A copy of the licence and the attribution notices with the binary[^10] |
| PCRE2 | BSD 3-clause | The copyright notice, conditions and disclaimer with the binary[^11] |
| CA bundle | MPL 2.0 | None, since programs load the file from the archive and never ship it |

A program that imports `anti.raylib` and `anti.miniaudio` owes no attribution. A program that imports `anti.net` or `anti.regex` owes one, and `anti_licenses` with `NOTICE.txt` supplies it. The table records how the licence texts read and gives no legal advice.

### Licence choices

The thread-pool runtime in `rt/` and the standard library in `std/` are 0BSD, each with a `LICENSE` file. The common case of a program therefore embeds one short notice and owes nothing. The compiler antic and the tool `anti` are MIT. The site decides the licence of the book text separately.

## Next

Chapter 25, Libraries for C, builds static and shared libraries from Anti code. It covers the export marker, the signature rule and the generated header. It also covers the runtime in both forms, symbol visibility, memory and threads across the boundary, versioning, and the tests that check them.

## References

[^1]: Tom Preston-Werner and others, *TOML v1.0.0*, sections "Comment" and "Keys", https://toml.io/en/v1.0.0

[^2]: Tom Preston-Werner, *Semantic Versioning 2.0.0*, summary and items 8 and 10, https://semver.org/

[^3]: Internet Engineering Task Force, *RFC 9110, HTTP Semantics*, sections 8.8.3, 13.1.2 and 15.4.5, https://www.rfc-editor.org/rfc/rfc9110.html

[^4]: curl project, *Extract CA Certs from Mozilla*, the file `cacert.pem`, its licence and its SHA-256 files, https://curl.se/docs/caextract.html

[^5]: Linux Foundation, *SPDX License List*, the identifiers of the listed licences, https://spdx.org/licenses/

[^6]: LLVM Project, *LLVM Exceptions to the Apache 2.0 License*, in `LICENSE.txt`, https://llvm.org/LICENSE.txt

[^7]: Open Source Initiative, *Zero-Clause BSD*, https://opensource.org/license/0bsd

[^8]: Jean-loup Gailly and Mark Adler, *zlib license*, condition 3, https://zlib.net/zlib_license.html

[^9]: Amazon Web Services, *MIT No Attribution*, https://github.com/aws/mit-0

[^10]: Apache Software Foundation, *Apache License, Version 2.0*, section 4, items (a) and (d), https://www.apache.org/licenses/LICENSE-2.0

[^11]: Open Source Initiative, *The 3-Clause BSD License*, condition 2, https://opensource.org/license/bsd-3-clause
