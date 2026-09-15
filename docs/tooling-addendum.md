# Anti tooling addendum

Details of two build modes, `anti check`, the formatter rules, the doc comment forms
and markup, and full C interop. Settled on 2026-09-14 and aligned with
`docs/decisions.md` on 2026-09-15. `docs/decisions.md` is the authority, and the details
below agree with it and with `docs/tooling.md`.

Contents:

- [Build modes and caching](#build-modes-and-caching)
- [Check command](#check-command)
- [Formatter rules](#formatter-rules)
- [Doc comment forms](#doc-comment-forms)
- [Doc markup](#doc-markup)
- [Doc block compilation](#doc-block-compilation)
- [Numeric types](#numeric-types)
- [C types](#c-types)
- [Sizeless IR](#sizeless-ir)
- [Unions](#unions)
- [Bitfields](#bitfields)
- [Packed and aligned structs](#packed-and-aligned-structs)
- [Inline function shims](#inline-function-shims)
- [Binding generator](#binding-generator)
- [ABI probe](#abi-probe)
- [Chapter changes](#chapter-changes)

## Build modes and caching

Whole-program optimisation emits one assembly file, so one changed line invalidates the
whole output. Two build modes fix that.

Dev mode is the default for `anti build`, `anti run` and `anti test`:

- `antic` compiles one module per call: every file in `src/` and every `.antl` in the
  dependency graph. The other modules are passed for type checking only.
- Each module produces `build/<os>-<cpu>/dev/<module>.s` and `<module>.o`.
- The cache key of a module is the SHA-256 of its input bytes, the `antic` version and
  the target name. `anti build` recompiles a module when its key changes and runs llvm-mc
  only on new assembly. Every build relinks.
- Cross-module calls are symbol references through the mangled names. No cross-module
  inlining.

Release mode runs with `anti build --release`, `anti run --release` and
`anti test --release`:

- One `antic` call with every module. The optimizer sees the whole program.
- Output is `build/<os>-<cpu>/release/<package>.s` and one object file.
- No caching. Any change rebuilds the file.

`dist/<os>-<cpu>/dev/` and `dist/<os>-<cpu>/release/` hold the two results. The
compiler change is one flag. `antic` already takes N inputs and emits one file.

`anti test` runs the suite in both modes when `--all-modes` is given. Bugs that hide in
inlining show up as a difference between the two.

## Check command

`anti check` runs everything that writes no artifact. It exits non-zero on the first
failing class and reports every failure in that class:

1. The front end on every file in `src/` and `test/`: lexer, parser, semantic analysis.
   No IR, no assembly.
2. Every fenced `anti` block in a doc comment, compiled as described in
   [Doc block compilation](#doc-block-compilation).
3. Doc warnings: unsupported markup, a `//#` note on a public item without a `///`
   comment, an unresolved backtick name, and with `--warn-undocumented` every `pub`
   item without a `///` comment.
4. Formatting, the same test as `anti fmt --check`.

`anti check` runs antic with `--doc-warnings` for the `//#` note warning. antic without
that option is silent about documentation.

`anti fmt --check` remains for a pre-commit hook that only checks layout.

## Formatter rules

`anti fmt` writes every `.anti` file under `src/` and `test/` in this form. The same form
is used for every listing in the book. The book is rewritten with `anti fmt` once, after
the compiler changes of these designs.

Indentation:

- One tab per level. No space at the start of a line.
- A wrapped continuation line gets one extra tab.
- Nothing is aligned past the indent.
- `anti html` and `anti tex` replace each leading tab with four spaces.

Braces, K&R style. The placement follows what the brace belongs to:

- An item body (`fn`, `struct`, `union`) opens with `{` on its own line.
- A statement block (`if`, `else`, `while`, `do`) opens with `{` on the line of the
  statement. `} else {` is one line. `} while cond` is one line.
- A closing `}` is on its own line unless it starts `} else {` or `} while`.

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

Other rules:

- Parentheses around conditions are dropped.
- One statement per line.
- Doc comments are re-wrapped at 80 columns. The line or block form the author chose is
  kept. A ` * ` gutter inside a block comment is removed.
- Ordinary `//` comments keep their position.

## Doc comment forms

Every doc marker has a line form and a block form. The lexer produces the same token
with the same text for both.

| Line form | Block form | Attaches to | Audience |
|---|---|---|---|
| `///` | `/** */` | The next item | Whoever can see the item |
| `//!` | `/*! */` | The module | The module's user |
| `//#` | `/*# */` | The next item | Developers of the library |
| `//#!` | `/*#! */` | The module | Developers of the library |

Block form rules:

- Text starts on the line after the opener and ends on the line before `*/`.
- Common leading whitespace is stripped. Nothing else is stripped.
- Block comments do not nest. A fenced code block inside a block comment must not
  contain `*/`. Use the line form for that item.

Use the block form for `//!` and `//#!`, which are long. Use the line form for item
docs, which are mostly one paragraph.

## Doc markup

Doc text is a fixed subset of Markdown. `anti doc --markdown` passes it through
unchanged. The subset:

- Paragraphs, separated by a blank line.
- Fenced code blocks with a language tag. `anti` blocks are compiled by `anti check`.
  `text` blocks are skipped.
- Inline code in backticks. A backtick name that resolves in the module table becomes a
  link.
- Unordered lists with `-`.
- Links as `[text](url)`.

Headings, emphasis, tables, images, numbered lists and HTML are not supported.
`anti check` warns on them and `anti doc` emits them as literal text.

## Doc block compilation

An `anti` block is compiled by `anti check` in one of two contexts:

- Blocks in `///`, `/** */`, `//!` and `/*! */` compile as a separate module with
  `import <module>;` implied. Names are qualified as a user writes them. Private items
  are out of reach, so an example that uses one fails the check.
- Blocks in `//#`, `/*# */`, `//#!` and `/*#! */` compile inside the module. They may
  use private items unqualified.

A block without `fn main` is wrapped in `fn main() -> int { ... return 0; }`. Blocks are
compiled through the front end only. They are not run.

```anti
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
```

## Numeric types

Sized types are primary: `i8`, `i16`, `i32`, `i64`, `u8`, `u16`, `u32`, `u64`, `f32`,
`f64`, `bool`.

Aliases: `int` is `i64`, `uint` is `u64`, `float` is `f64`, `byte` is `u8`. `char`
stays a 32-bit Unicode scalar and is distinct from every integer type. Slice lengths and
array indexes are `int`. There is no separate size type.

Literals are untyped and take their type from context. `DrawRectangle(10, 20, 100, 50,
c)` compiles against `i32` parameters without casts. A literal with no context is `int`
or `float`.

Conversions are explicit with `as`: `x as i32`, `p as *byte`. The rule "no implicit
conversions" stands. `as` converts between any two numeric types, between any two
pointer types, and between `char` and `u32`. `bool` is not a numeric type for `as`:
`bool as` an integer type gives 0 or 1, and no integer converts to `bool`. Untyped
literals are not conversions.

## C types

Bindings use C names so a generated file reads like the header. Fixed aliases:

| Anti | Type |
|---|---|
| `c_char` | `i8` |
| `c_uchar` | `u8` |
| `c_short` | `i16` |
| `c_ushort` | `u16` |
| `c_int` | `i32` |
| `c_uint` | `u32` |
| `c_longlong` | `i64` |
| `c_ulonglong` | `u64` |
| `c_size_t` | `u64` |
| `c_float` | `f32` |
| `c_double` | `f64` |

Target-sized primitives, decided by the back end from the target name:

| Anti | Windows | Linux and macOS |
|---|---|---|
| `c_long` | 32 bits | 64 bits |
| `c_ulong` | 32 bits | 64 bits |
| `c_wchar` | 16 bits | 32 bits |

Arithmetic on a target-sized type overflows at the target's width. Convert at the
boundary: `let n = value as int;`.

Other mappings in `extern fn` declarations: `char*` is `*byte`, `void*` is `*byte`, a C
function pointer is an Anti function type, a struct by value is an Anti struct with C
layout, an enum is a set of `const` values of `c_int`. Variadic externs are allowed:
`extern fn printf(fmt: *byte, ...) -> c_int;`.

## Sizeless IR

The IR contains types, never sizes, offsets or register classes. The `.antl` stays
byte-identical on every host because it records `c_long`, not 4 or 8.

The front end lowers `size_of(T)`, field access and array indexing to symbolic values:
`size_of T`, `offsetof T.f`, `stride [N]T`. The optimizer never folds them. The back end
replaces each with an immediate for the target after it has computed the layout.

A `const N: int = size_of(Foo);` stays symbolic. `[N]i32` is then a target-sized array,
allowed as a struct field and as a local. Two such array types are equal when their
length expressions are equal, and their `.len` is not a constant. The length stays
symbolic until the back end folds it for a target. A result below 1 is an error that
names the target and the expression. The type checker needs only the element type and
the length expression.

The back end owns, per target:

- Sizes of `c_long`, `c_ulong` and `c_wchar`.
- Struct and union layout, including `packed` and `align(N)`.
- Bitfield layout.
- Folding of `size_of`, offsets and strides, and the check that a folded array length is
  at least 1.
- Aggregate classification for argument and return passing, structs and unions alike.
- Calling conventions, callee-saved sets, stack frames, symbol prefixes, relocations and
  directives.

`docs/decisions.md` records layout as computed by the back end and the IR as free of
sizes.

## Unions

`union` is a language type with C layout:

- Declared like a struct. Every field starts at offset 0. Size is the largest field
  rounded up to the largest alignment.
- A literal names exactly one field: `Value { f: 1.5 }`.
- Reading a field reinterprets the bytes. No tag, no check.
- No methods. Method-call sugar does not apply to unions.
- Passed and returned by value under the target's classification rules. The classifier
  is the struct classifier with every field at offset 0.

Storing a union inline in a struct and passing a pointer to C involves no copy. The
memory is what C expects.

## Bitfields

Bitfields are a language feature. The back end lays them out with the target's C rule.

```anti
struct Flags
{
    visible: u32 : 1,
    layer: u32 : 4,
    pad: u32 : 27,
}
```

Rules:

- The declared type is a sized integer. Signedness is in the type.
- The front end stores type and width. The back end computes storage unit, offset and
  shift. It uses the System V rule on Linux and macOS and the MSVC rule on Windows.
- A bitfield load or store lowers in the back end to a mask and shift sequence. It is
  the sequence a C compiler emits.
- `anti bind` emits C bitfields one to one.

## Packed and aligned structs

Two modifiers on `struct` and `union` declarations:

- `packed struct Foo { }` removes padding between fields.
- `struct Foo align(16) { }` raises the alignment of the type.

`packed` and `align` are contextual words, not keywords. `packed` has its meaning only
directly before `struct` or `union`. `align` has its meaning only after the type name and
before `{`. A recursive-descent parser tells both positions apart without lookahead
beyond the next token, and elsewhere both are ordinary identifiers. `align` stays usable
as a field or parameter name in the code that declares aligned types.

Both change only the layout computation in the back end. `anti bind` emits them from
`#pragma pack` and alignment attributes and refuses any other layout directive.

## Inline function shims

A `static inline` function in a C header has no symbol. For each bundled library that
has them, `anti bind` writes `shim_<library>.c` next to the binding. Each inline
function gets an exported wrapper with the same name and the library's prefix kept. The
runtime archive's CMake build compiles the shim into the library's static archive. The
generator writes shim and binding in one run, so they cannot drift.

## Binding generator

`anti bind` writes an ordinary `.anti` module from a C API description:

- `anti bind raylib_api.json` for raylib, as the module `anti.raylib`.
- `anti bind --clang miniaudio.h` for headers without a JSON description, using
  libclang, as the module `anti.miniaudio`.

The output uses the C type names from [C types](#c-types). Unions, bitfields, packed
and aligned structs are emitted one to one. The shim file is written when needed. Macro
constants become `const` when the generator can evaluate them. Other macros are skipped
with a warning. C++ APIs are refused. Only `extern "C"` surfaces are bound.

Nothing in a binding is hand-written.

## ABI probe

The runtime archive's CMake build compiles one C program per bundled library. It prints,
for every public struct and union, `sizeof`, the alignment and `offsetof` of every
field. For every bitfield it prints the bytes of a struct with only that field set to 1.

`anti test` compiles the same probe in Anti from the generated binding and compares the
two outputs on every target. A mismatch names the target, the struct and the field.

The probe covers `c_long`, `c_wchar`, unions, bitfields, packed and aligned structs. It
is the ABI test for chapter 18.

## Chapter changes

- Chapter 2: sized types, aliases, untyped literals and `as`. Also `c_long`,
  `c_ulong`, `c_wchar`, `union`, bitfields, `packed` and `align(N)`. Every listing in
  the formatter style.
- Chapter 4: the four doc markers in line and block form.
- Chapter 7: the sizeless IR rule and the symbolic `size_of`, `offsetof` and `stride`
  values.
- Chapter 10: the rule that the optimizer never folds a symbolic size.
- Chapter 11: aggregate classification for unions.
- Chapter 18: layout computed in the back end, unions, bitfields with both rules,
  packed and aligned structs, the ABI probe.
- Chapter 21: dev and release modes in the test suite.
- Chapter 23: shims in the runtime build.
- Chapters 2 and 4: block comments that do not nest.
- Chapter 2: a short "contextual words" paragraph for `packed` and `align`, next to the
  reserved words.
- Chapters 1 to 20: one rewrite of every listing with `anti fmt`, after the compiler
  changes.
- The build tool chapter: dev and release modes, the module cache, `anti check`,
  `anti bind`, described without the code of `anti`.
