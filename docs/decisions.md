# Decisions

Settled design decisions for the language, the compiler, the `anti` tool and the book. One entry per decision. Open items are listed at the end. The choices inside a compiler pass live in `docs/notes/chapter-NN.md`, beside the chapter that made them.

`docs/decisions.md` is the authority. The details of the `anti` tool, of distribution and of libraries for C are in `docs/tooling.md`, `docs/tooling-addendum.md`, `docs/distribution.md` and `docs/libraries-for-c.md`. Those files agree with `docs/decisions.md`, and a change goes into both.

## Names and publication

- Language: Anti. Source extension `.anti`. Compiler binary `antic`. Library file extension `.antl`. Build tool `anti`. Repository `book-writing-a-compiler`. Licence MIT for antic, `anti` and the repository. The runtime in `rt/` and the standard library in `std/` are 0BSD, with a `LICENSE` file in each.
- The name Anti is a recursive acronym, "Anti's Not Too Impressive". The sentence appears once in the book, in chapter 1 where Anti is introduced, and once in the site footer, nowhere else.
- The chapter 1 copy of that sentence is in `writing-a-compiler/01-parts-of-a-compiler/index.md` of this repository. The footer copy is in the site repository `FoundingFuture/website`, where the website team of foundingfuture.com writes it.
- The repository holds the finished compiler, and the chapters walk through it in the order it was built. A tag `chapter-N` pins the complete code of `main` with the book and the tests of chapters 1 to N. `tools/scripts/chapters.py` rebuilds the chapter commits from `main` and tags them, and its last step checks out every tag in order and runs its tests. Chapter commits are never kept by hand. After the first public release the tags freeze and fixes become errata.
- [provisional] The history of `main` is the chapter series: a root commit for chapter 1 and one commit per chapter bundle after it, the drafts included, with the subject `Chapter N, <title>`. The last commit holds the tree of `main`, so the site reads the same files. Reason: the chapter commits are the product, and a draft chapter still needs a commit for `main` to end at the full tree.
- Only a chapter with `draft: false` gets a tag. Reason: a tag freezes at the first public release, and a draft is not published.
- [provisional] `tests/chapters.txt` maps each test to the chapter that introduces what it checks, by a regular expression on the test name. The configuration marks the tests of chapters above the last chapter folder as disabled and stops for a test without a chapter. Reason: `set_tests_properties` calls of later tests keep working, and a new test cannot miss its chapter.
- [provisional] The check exports each tag with `git archive` into a new directory, as a reader's checkout holds it, and takes the tool paths from `build/CMakeCache.txt`. Reason: an export catches a file that `.gitignore` keeps out, which a working tree hides.
- Rejected names for collisions: Mica, an existing Rust scripting language. Kite, three existing languages and a former code-completion product. Thin/thinc, the Thinc ML library. Bubble, the bubble.io platform.
- Publication: foundingfuture.com, Hugo, theme hugo-theme-ff-1. Chapters live in `writing-a-compiler/` and are mounted to `content/programming/writing-a-compiler/`. One page bundle per chapter. Order by `weight`.
- Chapter template: a short "Previously" section opens each chapter, a short "Next" section closes it. Chapter 1 has no "Previously". The last chapter has no "Next". Both sections are written in the chapter body from the neighbouring chapter's `summary` in `docs/table-of-contents.md`. Each chapter also carries its own `summary` in front matter. The ff-1 pager renders previous and next links only.
- Figures: an SVG file in the chapter bundle, placed with the site shortcode `figsvg` after the comment `<!-- requires site shortcode: figsvg -->`, as the mathematics articles do. Its colours are theme properties such as `var(--ink, #0D1620)`, so the figure follows the site's light and dark buttons. An SVG image cannot see those buttons.
- Book listings of Anti code come from `anti html` and are placed with a site shortcode for listings. The site supplies CSS for the eight token classes. Every listing uses the formatter style. The book is rewritten once with `anti fmt` after the compiler changes of the tooling designs, not before.
- Last chapter: a guide on where to take Anti next, with ideas and code sketches. It builds no compiler code.
- The next chapters are written in the order 22, threads, then 23, the runtime archive with its native libraries, then 21, testing six targets. Two tests and the constructor check wait for threads, and the four remaining standard library modules wait for the native libraries. Chapters 24 and 25 leave draft after those three. The `anti` tool follows, because every one of its tests needs the runtime archive.

## Scope and toolchain

- Written in C11 from scratch. No flex, no bison. Own lexer, parser, AST, semantic analysis, target-independent IR, optimizer, two instruction-selection back ends (x86_64, ARM64). Output is GAS-style assembly text.
- antic stops at assembly. llvm-mc (Apache 2.0 with LLVM Exceptions) assembles. lld from the pinned LLVM release links on every target: `ld.lld` for ELF, `ld64.lld` for Mach-O and `lld-link` for COFF. One pin covers llvm-mc, lld and llvm-ar. The platform linker is a fallback that `--linker platform` selects: `ld` of the Xcode Command Line Tools, GNU ld or `link.exe`. Chapter 3 links its first executable with it, because a reader has it installed. Chapter 16 and the driver default to lld. An assembler and a linker for three object formats is as much work as the compiler, times six targets, which the book states.
- The runtime archive holds `sysroot/<os>-<cpu>/` per target: the macOS SDK `.tbd` stubs from the Command Line Tools on the build Mac, the Windows UCRT and SDK import libraries fetched with xwin, and musl for Linux. `licenses/` records the licence of each. The `<name>.package.o` member of static archives stays, because every linker expects object members. The test `cross_link_<target>` links a program for every target from the host and checks the output's format and architecture with llvm-objdump.
- llvm-mc, lld and llvm-ar ship as prebuilt binaries from the official LLVM release archives. One pin covers all three: LLVM 23.1.1, written once in `tools/llvm-version`. The runtime archive build checks the installed binaries against the pin with `tools/check-llvm.cmake`. It copies them, llvm-objdump and llvm-readobj into its `bin/` directory, next to a copy of the version file.
- Minimum macOS version 11.0 for both macOS targets. The assembly output carries `.build_version macos, 11, 0`.
- Six targets: Linux, macOS, Windows on x86_64 and ARM64. Development on an Apple Silicon Mac with clang. Other targets cross-assembled and tested in CI: GitHub Actions on GitHub-hosted runners for all six targets, for this repository too.
- CI runs only on a release tag or when Eddie asks for a run, through a manual trigger. It never runs on every push or pull request. The monthly build minutes are limited.
- Chapter 21 CI runs on GitHub Actions with the runners `ubuntu-24.04`, `ubuntu-24.04-arm`, `windows-2025`, `windows-11-arm`, `macos-15-intel` and `macos-15`, on release tags and on demand. `.github/workflows/test.yml` holds the matrix and triggers on `workflow_dispatch` alone, which the test `workflows_dispatch_only` checks. It has not run.
- LLVM publishes no archive for macOS x86_64, and `tools/build-llvm.cmake` builds that host as it builds the two Linux ones, with `CMAKE_OSX_ARCHITECTURES=x86_64` on the development Mac. Rosetta runs the result for `tools/check-llvm.cmake`. The pin then covers all six hosts from one source, and nothing mentions Homebrew.
- Nothing names Homebrew. The archive of macOS on x86_64 is published beside the other five, so every host installs its tools the same way. Reason: Homebrew keeps `llvm@<major>` for a superseded major alone, so `llvm@23` did not exist while 23.1.1 was the current release, and a formula name is a pin that moves under us. The check runs `--version` of each of `ld.lld`, `ld64.lld` and `lld-link`, because antic calls lld under those three names.
- Sanitizers apply to antic, not to the code that Anti programs link. A sanitizer configuration compiles the runtime library and the C objects of the ABI tests with `-fno-sanitize=all`, and all tests pass under ASan and UBSan.
- Early binaries: chapter 3 emits assembly for the host through a direct code path and drives llvm-mc and the linker. Every later chapter extends a running compiler. The direct path lives in `direct-path/` of the chapters 3 to 5, because the finished compiler holds none of it.
- No C code generation at any stage. A C shortcut for early chapters and a C back end as a test oracle were both rejected. Back-end testing compares x86_64 and ARM64 outputs against expected results and against each other.
- The book carries a per-platform install list.
- `anti` is the user-facing build tool and `antic` the bare compiler. `anti` lives in `tools/anti/`, links `antic_core` and is MIT. The book describes `anti` and shows none of its code. Code generation that `anti` needs, such as separate objects per module and shared libraries, is part of the book.

## Core language

- Types: `int`, `uint`, `float`, `bool`, `char` (32-bit Unicode scalar value), `byte`, `str`, structs, unions, fixed-size arrays `[N]T`, slices `[]T` (pointer plus length), typed pointers `*T`, function pointers. Sized numeric types `i8 i16 i32 i64 u8 u16 u32 u64 f32 f64` and the C types, see "Numeric types". No `*const T`.
- Statements: `fn`, `extern fn`, `let` with mandatory initialisation, `const`, `struct`, `union`, `import`, `pub`, `export`, `if` / `else if` / `else`, `while cond do { }` (zero or more iterations), `do { } while cond` (at least one, no trailing semicolon), `break`, `continue`, `return`. Bodies are braced blocks. Parentheses around conditions are optional, so a struct literal is not allowed bare in condition position. No `for` in the core.
- Operators with C precedence. No implicit conversions. Integer literals in decimal and hex with `_` separators. `main` returns the process exit code, see "Program entry". Identifiers are ASCII letters, digits and `_`.
- String literals: `"..."` with escapes, may span lines. `r"..."` raw. `b"..."` byte string of type `[]byte`, any byte value allowed. `br"..."` raw bytes. Hash delimiters for every prefix: `r#"..."#`, `r##"..."##`, see "Lexical rules". `f"..."` interpolation is deferred to the standard-library phase. No `u` or `a` prefixes.
- UTF-8 throughout. The lexer validates source. `str` is a pointer plus byte length, valid UTF-8, immutable, no embedded NUL, always followed by a NUL byte outside its length so `s.ptr` is a valid C string. Byte buffers that may hold zeros are `[]byte` or `*byte`.
- Memory model as in C. Locals in the stack frame with lexical block scoping. Address-taken locals get a stack slot, others may live in registers. Structs and arrays are value types, copied on assignment, passed and returned by value under each ABI. Heap through `alloc` and `free` bound to libc `malloc` and `free`. Pointers support dereference, address-of and indexing only. Field access `p.x` on a `*T` dereferences automatically. No reference counting, no garbage collector. Safety as in C: no bounds checks, dangling pointers possible.
- Struct layout equals C struct layout on the same target: field order, alignment, padding. Unions, bitfields, `packed` and `align(N)` follow the target's C layout too. The back end computes every layout. C libraries are called with structs by value and generated bindings need no marshalling.
- The object model is below, under "Object model", which replaced the rule that the language has none on 2026-09-17. Method-call sugar on a free function stays: `v.f(args)` rewrites to `f(&v, args)` or `f(v, args)` when `f`'s first parameter is `*T` or `T`. No overloading and no operator functions.
- Globals are constants only. No mutable globals.
- Modules: a module is identified by a module path, a dotted sequence of lowercase ASCII identifiers that mirrors a directory tree under the search roots. `import com.example.geometry.vec;` resolves to `com/example/geometry/vec.anti` or `com/example/geometry/vec.antl`. The last segment is the local name, and `import ... as` renames it. A keyword is never a segment, under `anti.` too. Items are private unless `pub`. Within a module a name is unique. Modules are named after their contents, not after a type.
- Symbols carry the full module path, as `com.example.geometry.vec.push` on ELF and Mach-O. COFF mangling length-prefixes each segment: `_A3com7example8geometry3vec_push`, and `_A8geometry_length` for one segment. `extern fn` names are never mangled. `main` is mangled like every other function, because the C `main` is in `rt/start.c`.
- Every path starting with `anti.` is reserved for the language's own libraries: `anti.io`, `anti.fs`, `anti.os`, `anti.text`, `anti.math`, `anti.mem`, `anti.license`, `anti.regex`, `anti.net` and `anti.rt` for the runtime. Bindings of bundled C libraries carry the C library's own name: `anti.raylib` and `anti.miniaudio`. `antic -c` refuses to produce a library under `anti.` without the project-internal flag. Third parties use a root they own, reverse-domain style, such as `com.niese.anti.web`.
- The search roots are the `-I <dir>` options of antic, in order. Reason: C compilers name include roots the same way, and a flag needs no configuration file.
- [provisional] antic derives the module path of its input from its source path under the first root that holds it. Slashes become dots, and `.anti` is dropped. Outside every root the path is the file name alone. An import without a library file on the command line resolves to `<root>/<path>.antl` in the first root that has it. Reason: the path mirrors the directory tree, as decided.
- [provisional] The project-internal flag is `--anti-internal`. A bare `anti` is reserved like `anti.` for `antic -c`, and no compilation may define the runtime's module `anti.rt`. Reason: the runtime's entry symbol is `anti.rt.main`.
- Single-segment module names stay valid for a program's own files and are not for publishing. `antic -c` warns on them.
- Reserved from the start: `worker`, `parallel`, `thread`, `join`, `dispatch`, `chan`, `send`, `recv`, `atomic`, `sync`, `yield`, `select`. The core states that memory shared between threads has no guarantees until the threading chapter.

## Object model

Every type in the object model is a `struct` with C layout. There is no `class` keyword and no hidden field. Settled on 2026-09-17.

- Functions inside a struct body. A function declared between the braces of a struct belongs to that struct. Its symbol is `module.T.f`, one segment more than a free function. Two structs in one module may both declare `f`.
- `self` is the receiver, of type `*T`, written as the first parameter: `fn area(self) -> f32`. Fields are reached through `self.x`. A bare `x` inside the function is a local, a parameter or a module item, never a field. A function without `self` is a static function, called as `T.f(args)`.
- `v.f(args)` resolves in the namespace of `v`'s type first, then in the module, then through `use` and `inherits`. A field wins over a function, as before. `T.f(&v, args)` calls the same function without the sugar.
- `pub fn` inside a struct is visible wherever the struct is. `fn` without `pub` is visible only to the functions declared in the same struct, on any instance. A public function may call private ones, and a private function may call either.
- A function inside a struct may be `export` only when it is `pub`. It then follows the export signature rule with `self` as `T*` in the header. Its C symbol is `<Class>_<fn>`, with the receiver first, which the header section spells. Reason: a plain name would collide when two types of one module export the same function.
- A field may carry a default: `x: int = 0`. A literal may omit a field that has a default. `[e; N]` and `alloc` are unaffected. C layout is unaffected.
- A struct body may hold `const N: T = e;`, reached as `T.N`. `self.N` is refused, so a constant is never mistaken for a field.
- `enum Color { Red, Green, Blue }` declares a named integer type. `enum Mode: u8 { A = 1, B = 4 }` names the underlying type and gives explicit values. Without them the type is `c_int` and the values start at 0. An enum has C layout and converts with `as` to and from its underlying type. It compares with `== != < <= > >=`. It may hold functions and constants like a struct. `anti bind` emits a C enum as an Anti enum, and the header writes `enum`.
- `use name: T` is a field with promotion. The public functions and the fields of `T` are reachable on the containing struct. `v.f(args)` rewrites to `T.f(&v.name, args)`, and `v.x` to `v.name.x`. A `use` field may sit anywhere, and a struct may have more than one. The containing struct's own names win. A name that two `use` fields both provide is an error at the call that names both fields. Private functions of `T` do not promote. Promotion follows a chain of `use` fields. A `concrete fn` promotes like any public function, so `sp.area()` is `Square.area(&sp.hitbox)`, a direct call through no table. A `Sprite` is not a `Shape`.
- `inherits name: T` is the first field of a struct, at most once, or the struct is refused with a message that says so. `T` is nested whole at offset 0, trailing padding included, and the struct's own fields start at `size_of(T)`. The struct may not declare a field, function or constant with a name that `T` or its chain already has. The exception is `concrete fn` for an abstract or an earlier concrete function. `inherits` promotes names as `use` does, and `v.name` is the base part as a value.
- `&d` converts to `*T` for every `T` in `d`'s `inherits` chain. The conversion is the same address and is the one implicit conversion of the language. `p as *D` converts a base pointer back, unchecked. Values never convert: passing a `D` where a `T` is expected is an error, and `d.name` is the explicit way.
- `packed` and `align(N)` on a struct leave the layout of an `inherits` or `use` field as it is, as C leaves a nested struct.
- `abstract fn area(self) -> f32;` in a struct declares a contract without a body. Every struct that inherits it must declare `concrete fn area(self) -> f32` with the same signature. Otherwise the compiler refuses the struct and names the missing function. A `concrete fn` deeper in the chain replaces the one above it. A function that matches an abstract function without `concrete` is an error, and a `concrete fn` that matches nothing is an error. Both are `pub`. A struct with an unfilled abstract function is never a complete value. A literal of it is allowed only as the `inherits` part of a derived literal. `let s: Shape`, `alloc(Shape, n)`, a plain field of type `Shape` and a `use` field of type `Shape` are refused. An abstract struct appears behind a pointer and as an `inherits` field, nowhere else.
- A pointer to a struct that declares or inherits an abstract function is two words: the object and a table. Every other pointer is one word. The table is a read-only global per concrete struct, one entry per abstract function in the chain. It holds that struct's `concrete` functions. The constant-aggregate path of the back end writes it.
- `&circle` converting to `*Shape` pairs the address with `Circle`'s table. `s.area()` through a two-word pointer loads the entry and calls it with the object word as `self`. `c.area()` on a known concrete type is a direct call. `s as *Circle` keeps the object word and drops the table. `==` on two-word pointers compares the object word. `[]*Shape` holds any concrete struct at 16 bytes per element.
- The two-word pointer crosses to C as a struct of two pointers, the object and the table. The table crosses as a `const` struct of function pointers. The header writes both.
- `[&c, &s]` has type `[2]*Shape` only when the context gives that type, as in `let shapes: [2]*Shape = [&c, &s];` or an argument of type `[]*Shape`. Without context each element keeps its own pointer type, and `[&c as *Shape, &s as *Shape]` is the explicit form. Reason: the conversion happens per element, and a literal without a declared type has no base to convert to.
- Messages: `` `reset` is private to `Counter` ``. `` `Counter` has no function `rest` ``. `` `Counter` has no field `rest` ``. `` `area` is abstract in `Shape` ``, for a call through a pointer whose type has no implementation. `` `inherits` must be the first field of `Circle` ``. `` `Circle` lacks `concrete fn area` ``. `` `move` is provided by both `hitbox` and `shape` ``.
- Keywords added: `self`, `abstract`, `concrete`, `enum`, `use`, `inherits`. All reserved from chapter 2.
- Not in the language: `class`, `virtual`, `override`, `super`, interfaces separate from structs. Also not: constructors that run by themselves, destructors, private fields, operator functions. The closing guide keeps a hidden table pointer as the sketch of what a compiler adds when the pointer must stay one word.

Example, every rule once:

```anti
const PI: f32 = 3.14159;

enum Kind: u8 { Circle, Square }

struct Shape
{
    kind: Kind,
    x: f32 = 0.0,
    y: f32 = 0.0,

    const MAX_SIDE: f32 = 1000.0;

    abstract fn area(self) -> f32;

    pub fn move(self, dx: f32, dy: f32)
    {
        self.x = self.x + dx;
        self.y = self.y + dy;
    }
}

struct Circle
{
    inherits shape: Shape,
    r: f32,

    pub fn new(r: f32) -> Circle
    {
        return Circle { shape: Shape { kind: Kind.Circle }, r: r };
    }

    concrete fn area(self) -> f32
    {
        return PI * self.r * self.r;
    }
}

struct Square
{
    inherits shape: Shape,
    side: f32,

    concrete fn area(self) -> f32
    {
        return self.side * self.side;
    }

    fn clamp(self)
    {
        if self.side > Shape.MAX_SIDE {
            self.side = Shape.MAX_SIDE;
        }
    }
}

struct Sprite
{
    use hitbox: Square,
    texture: int,
}

fn total(shapes: []*Shape) -> f32
{
    let sum = 0.0;
    let i = 0;
    while i < shapes.len do {
        sum = sum + shapes[i].area();
        i = i + 1;
    }
    return sum;
}

fn main() -> int
{
    let c = Circle.new(2.0);
    let s = Square { shape: Shape { kind: Kind.Square }, side: 3.0 };
    c.move(1.0, 1.0);
    let shapes: [2]*Shape = [&c, &s];
    let t = total(shapes[0..2]);
    let sp = Sprite { hitbox: s, texture: 7 };
    let a = sp.area();
    return 0;
}
```

The lines show, in order: an enum with an underlying type, defaults that let `Shape { kind: ... }` omit `x` and `y`, a struct constant reached as `Shape.MAX_SIDE`, a contract and its two fulfilments, a static `new`, a private `clamp`, `use` promoting `area` onto `Sprite`, and dispatch through `[]*Shape` in `total`. `c.move` calls `Shape.move` through the `inherits` chain, and `shapes` converts both addresses because its type is declared.

- [provisional] A struct or enum body lists its fields first, comma separated, and then its functions and constants, each ended by its own `;` or block. Reason: a field after a function would read as a member, and the example of this section writes them in that order.
- [provisional] Three messages the list above does not spell. A constant reached through a value says `` `ORIGIN` is a constant of `Point`, reached as `Point.ORIGIN` ``. A plain function that matches an abstract one says `` `area` of `Circle` matches an abstract function and needs `concrete` ``. A concrete function that fills nothing says `` `concrete fn nothing` of `Plain` fills no abstract function ``. Reason: the decision names the errors without their wording, and each needs one.

Chapter changes:

- Chapter 2: one section for the object model, in the order above. Its first sentence states that every type stays C layout.
- Chapter 4: the six keywords.
- Chapter 5: functions, constants, `abstract` and `concrete` inside a struct body, default values, `enum`, `use` and `inherits` as field forms.
- Chapter 6: the namespace per struct, resolution order and visibility of struct functions. Also the export rule, contract checking, the pointer conversion and the seven messages.
- Chapter 8: the method rewrite through `use` and `inherits` chains, the two-word pointer, conversion, the indirect call, table emission.
- Chapter 9: struct functions, constants, enums and abstract and concrete functions in the interface of a library file. Tables in its IR. The format version rises.
- Chapter 11: the two-word pointer as an aggregate of two pointers for classification.
- Chapter 18: `inherits` in layout, defaults in literals, the nested base with its padding.
- Chapter 25: the two-word pointer and the table in the generated header, `self` as `T*`.
- The build tool chapter: `anti bind` emits enums.
- Chapter 27: the hidden table pointer sketch.

Tests:

- A private call from outside, from a public function, and from a private function on another instance.
- A struct with two functions named `f` in one module, called on each.
- Defaults: a literal that omits every defaulted field, and one that overrides a default. A literal that omits a field without a default is an error.
- Struct constants: `T.N` in a constant expression, `self.N` refused.
- Enums: explicit values, the underlying type, `as` both ways, comparison, a function inside an enum. The header of an exported enum compiled by clang.
- `use`: promotion of a function and a field, own name winning, a clash between two `use` fields.
- `inherits`: layout checked by the ABI probe against a C struct with the base nested first. Also `inherits` not first, a chain of three, a base field name redeclared.
- Pointer conversion: `&d` to every base in the chain, a value where a base value is expected, `as` back.
- Contracts: a missing `concrete fn`, a match without `concrete`, a `concrete fn` matching nothing, a deeper replacement, instantiation of a struct with an unfilled contract.
- Dispatch: a `[]*Shape` of circles and squares summed through `area`, and the same sum on the concrete types. The table decoded from the read-only section of the assembly.
- The two-word pointer through C: a C function receives one and calls through the table. It is compiled by clang against the generated header.
- Every message above pinned by a test.

## Numeric types

- Sized types: `i8 i16 i32 i64 u8 u16 u32 u64 f32 f64`. `int` is `i64`, `uint` is `u64`, `float` is `f64` and `byte` is `u8`. Each pair is one type with two spellings, so `[]byte` and `[]u8` are the same type. `char` and `bool` are distinct types.
- [provisional] `uint` and the `c_` type names are keywords. Reason: the lexical rules make every type name a keyword.
- `bool` is 1 byte holding 0 or 1, the layout of C `_Bool`. `char` has the layout of `u32`.
- C types for bindings: `c_char` `c_uchar` `c_short` `c_ushort` `c_int` `c_uint` `c_longlong` `c_ulonglong` `c_size_t` `c_float` `c_double` are aliases of the sized types, with `c_size_t` as `u64`. `c_long` and `c_ulong` are 32 bits on Windows and 64 bits elsewhere. `c_wchar` is 16 bits on Windows and 32 bits elsewhere. Arithmetic on these three wraps at the target's width. No `isize` or `usize`. Library files stay byte-identical, because the IR records the type, not its size.
- `c_wchar` has the signedness of C `wchar_t` on the target: signed on Linux x86_64 and macOS, unsigned on Linux ARM64 and Windows. Lowering writes the unsigned form of each operation on it, and the back end picks the signed form where `wchar_t` is signed. The test `program_abi_wchar` converts a `c_wchar` above 0x7FFFFFFF to `i64` and `i32` and compares with clang on the host, which disproved the earlier rule of an unsigned `c_wchar` on every target.
- [provisional] A literal or constant of `c_long`, `c_ulong` or `c_wchar` fits the narrower width of the type. Reason: the constant then has one value on every target.
- [provisional] A conversion to or from `c_long`, `c_ulong` or `c_wchar` truncates when the target type is never wider and extends otherwise. The back end makes it a copy where both widths are equal. Reason: one IR instruction serves every target.
- Literals take their type from context: the declared type, the parameter type or the other operand, as in `let x: i8 = 5;` or `a + 1` with `a: u32`. Without context an integer literal is `int` and a float literal is `float`. An out-of-range literal is a compile error. An integer literal never becomes a float.
- A `-` directly before a literal forms one constant, so `let x: i8 = -128;` is valid. Unary `-` on an unsigned type is a compile error.
- Both operands of a binary operator have the same type. The result of an arithmetic operator has that type. Integer `+ - *` wrap modulo 2^N on every width, so constant folding agrees with both CPUs.
- `/` truncates toward zero. `%` takes the sign of the dividend. Division by zero and `MIN / -1` are undefined behaviour. x86_64 traps and ARM64 returns 0 or MIN. The test suite does not exercise them.
- `>>` is arithmetic on signed types and logical on unsigned types. A shift count that is negative or at least the bit width is undefined, because the two CPUs mask it differently.
- Conversions are explicit: `x as T`. `as` binds tighter than `*` and looser than unary operators, so `-x as u8` is `(-x) as u8`.
- Integer to integer truncates, or sign- or zero-extends by the source type. Integer to float rounds to nearest. Float to integer truncates toward zero, and NaN or an out-of-range value is undefined. `f64` and `f32` convert to nearest.
- `%` applies to integer types only, as in C. `& | ^ ~ << >>` apply to integer types only. `&&`, `||` and `!` are the boolean operators.
- `as` converts between any two numeric types and between any two pointer types. `bool` is not a numeric type for `as`: `bool as` an integer type gives 0 or 1, and there is no integer to `bool` conversion. `char as u32` and `u32 as char` are allowed, the second unchecked.
- Floats are IEEE 754 binary32 and binary64. antic never fuses a multiply and an add, so both back ends produce the same bits.
- Division and remainder by zero are compile errors when the operands are constants. So are a shift count negative or at or above the type width and a float-to-integer conversion outside the target type's range. The rule holds in a constant and in a function body. The message names the operation and the values. Each is undefined in C and has no value that the constant folder could produce. A variable operand leaves the case to run-time undefined behaviour.
- The minimum value of a signed type divided by `-1` is a compile error on constant operands too. Reason: C11 leaves it undefined in the same section as division by zero, and the constant evaluator already refused it.
- A constant error message names the operation and the operand values, in the form the reader wrote them, and says what is wrong in three or four words. The four messages read `` `7 / 0` divides by zero ``, `` `-128 / -1` does not fit `i8` ``, `` `1 << 64` shifts out of range `` and `` `30000000000.0 as i32` does not fit `i32` ``. A literal operand keeps its spelling, and any other operand shows the literal of its value.
- [provisional] A float value without an Anti literal shows as `nan`, `inf` or `-inf`, and a value from `size_of` as `size_of(T)`. Reason: the rule asks for the value, and neither has a spelling of the reader or a literal.

## Declarations and statements

- `let x: T = e;` and `let x = e;`. The type is optional. Without it the variable has the initialiser's type. `let` bindings and parameters are mutable.
- `const N: T = e;` at module level and in blocks. The initialiser is a constant expression. It may use literals, other constants, every binary operator including `&&` and `||`, unary `-`, `!` and `~`, `as`, struct literals, array literals and `size_of(T)`. It may read a field of a const struct, `.len` of a const array or of a string literal, and an element of a const array at a constant index. Comparisons give `bool` constants. `null` is a constant, and `==` or `!=` against `null` is allowed. No other pointer value is constant. Calls, `alloc`, `&`, `*`, `.ptr`, slicing and indexing anything other than a const array are excluded. A constant has no address.
- A function without `-> R` returns no value and uses `return;`. There is no `void` type name. In a function with `-> R`, reaching the end without `return` is a compile error. A body ends in `return` when its last statement is a `return`, or an `if` chain with a final `else` whose every branch ends that way. Loops never count, `while true` included. Code after `return` is allowed and produces no diagnostic in the core. A warning for it is a candidate for the closing guide.
- `extern fn printf(fmt: *byte, ...) -> i32;` declares a C variadic function. Variadic arguments are 32-bit or 64-bit integers, `f64` or pointers. Other types need `as`.
- Assignment `=` and `+= -= *= /= %= &= |= ^= <<= >>=` are statements, not expressions. No `++`, `--`, `?:` or comma operator.
- An expression statement is a call. A bare `{ }` block is a statement. Blocks hold statements, `let` and `const`. `fn`, `extern fn`, `struct`, `union` and `import` appear at module level only.
- Shadowing is allowed. A `let` in an inner block may reuse the name of an outer local, a parameter or a module-level item. It hides that name until the block ends. Redeclaring a name in the same block is an error. The function body is an inner block relative to the parameters, so `let x = x + 1;` at the top of a body is allowed and its initialiser sees the parameter. The new name's scope starts after its `let` statement, so in `let x = x + 1;` the initialiser sees the outer `x`. Module-level items may be used before their declaration.
- `import` lines come first in a file. Import cycles are a compile error, because each `.antl` is built against the interfaces of its imports. `pub` applies to `fn`, `extern fn`, `struct`, `union` and `const`. A struct's fields are visible wherever the struct is.
- Conditions have type `bool`. `&&` and `||` short-circuit. Operands and arguments evaluate left to right. In an assignment, compound assignments included, the place is evaluated first and the value second.

## Expressions and types

- `struct Vec2 { x: f32, y: f32 }`, fields separated by commas. A struct has at least one field. Literal `Vec2 { x: 1.0, y: 2.0 }` names every field exactly once, in any order.
- Array literals `[1, 2, 3]` and the repeat form `[0; 4096]`. In `[e; N]` the operand `e` is evaluated once and copied into every element. `N` in `[N]T` and `[e; N]` is a constant expression of type `int`, at least 1, as C11 forbids zero-length arrays. A length computed from `size_of` is allowed for struct fields and locals. Such array types compare by their length expression, and their `.len` is not a constant. The length expression stays symbolic. The back end folds it per target and reports an error naming the target and the expression when the result is below 1.
- Trailing commas are allowed in every comma-separated list: struct declarations, struct and slice literals, array literals, parameters, arguments and function pointer type parameters.
- Structs are nominal types. Two struct declarations are two types. Arrays, slices, pointers and function pointer types are equal when spelled the same.
- `a[lo..hi]` on an array or a slice has type `[]T`. Both bounds are required. The array operand is addressable, because the slice points into it. A slice is built from a pointer as `[]T { ptr: p, len: n }`.
- `.len` has type `int`. `.ptr` has type `*T`, and `*byte` on `str`. Both are read-only on `str` and slices. Arrays have `.len` only. Index expressions have type `int`.
- `str` supports `.len`, `.ptr` and `s[i]`, which has type `byte`. `s[lo..hi]` on a `str` has type `[]byte`, which has no NUL rule to break. In the core a `str` comes from a literal or from the arguments of `main`. Building a `str` from parts is a standard-library concern: `anti.text` provides them: after `import anti.text;`, `text.equal(a, b)` compares two, and `text.from_c(p)` makes one from a C string.
- C `void *` and opaque handles such as `FILE *` are `*byte`, converted with `as`. No void pointer type and no empty struct.
- `null` is a literal of every pointer and function-pointer type. Pointers compare with `==` and `!=`. No pointer arithmetic.
- `alloc(T, n) -> *T` returns uninitialised memory, or `null` on failure. `free(p)` releases it. `size_of(T) -> int` is a type's size on the target. The three names are keywords. `alloc` and `size_of` take a type argument.
- `union` is declared like a struct and has C layout: every field at offset 0, size the largest field rounded up to the largest alignment. A literal names exactly one field. Reading a field reinterprets the bytes. Method-call sugar does not apply. Unions pass by value under the struct classifier with every field at offset 0.
- [provisional] A union literal is not a constant expression. Reason: a constant holds a value for every field of an aggregate, and a union literal names one field.
- Bitfields: `name: u32 : 4` in a struct. The type is a sized integer and carries the signedness. The back end lays bitfields out with the System V rule on Linux and macOS and the MSVC rule on Windows. It lowers access to masks and shifts.
- [provisional] A union may hold bitfields, and every bitfield of a union starts at bit 0. Reason: C allows them, and a binding emits a C union one to one.
- A named bitfield has a width from 1 to the bits of its type. An unnamed zero-width bitfield stays in the type as a unit break, as in C, under the System V and the MSVC rule. The ABI probe holds one, and `anti bind` writes one for each in a C header.
- [provisional] A zero-width bitfield is written `_: T : 0`. The name `_` names no other field, and no literal, access or constant mentions it. Reason: C's unnamed field needs a spelling, and `_` is the smallest one that the grammar already reads as a field name.
- [provisional] A union holds no zero-width bitfield. Reason: a union has no unit to break, and C compilers disagree on its effect, as MSVC gives `union { char a : 3; long long : 0; }` a size of 8 with alignment 1.
- A zero-width bitfield of type T follows the C compiler of the target, as clang 21.0.0 lays it out. System V on Linux x86_64 and macOS moves the next field to a multiple of T's alignment, in a packed struct too. It leaves the struct's alignment unchanged. AAPCS64 on Linux ARM64 also raises the struct's alignment to T's. MSVC ignores it after a field that is not a bitfield. After a bitfield it closes the unit, aligns the next field to T's alignment and raises the struct's alignment to it, both limited by `packed`.
- [provisional] A bitfield that needs more than 8 bytes from its first byte, possible only in a packed struct, is an error from the back end. Reason: the load and store lowering reads one integer of at most 8 bytes.
- `packed struct Foo` removes padding between fields. `struct Foo align(16)` raises the type's alignment. Both apply to unions too. `packed` and `align` are contextual words, not keywords. `packed` has its meaning only directly before `struct` or `union`, and `align` only after the type name and before `{`. Elsewhere both are ordinary identifiers, so `align` stays available as a field or parameter name.
- [provisional] The N of `align(N)` is a constant expression of type `int` with a power of two as its value, and a value computed from `size_of` is refused. Reason: C's `_Alignas` takes a constant power of two, and the IR stores N as a number.
- [provisional] An `align(N)` below the alignment that the fields give is an error from the back end that names the type and the target. Reason: C refuses an `_Alignas` below the natural alignment, and only the back end knows that alignment.
- `export` on `fn`, `struct`, `union` and `const` implies `pub`. An `export fn` gets the unmangled symbol of its name, the same rule as `extern fn`, and two modules may not export one name. Its signature uses sized integers, `int`, `uint`, `float`, `bool`, the C types, pointers to such types, exported structs and unions, and function pointers of such signatures. `char`, `str`, slices, arrays by value and non-exported structs are refused.
- [provisional] The fields of an `export struct` or `export union` follow the export signature rule, and a field may also be a fixed-size array of such a type. Reason: the generated header spells every field in C.
- [provisional] An `export struct` or `export union` with `align(N)` has no bitfield as its first field. Reason: the header writes `align(N)` as `_Alignas` on the first field, whose offset is 0 on every target, and C refuses `_Alignas` on a bitfield.
- [provisional] An `export const` has a numeric type, `bool` or `str`. Reason: the header writes numbers and bools as `#define` and strings as `static const`.
- [provisional] `main` cannot be exported. Reason: the C `main` of the runtime starts the program, and an exported `main` would take its symbol.
- [provisional] Whole-program optimisation keeps every `export fn`, even one that no Anti code calls. Reason: C code may call it.
- Function pointer types are written `fn(i32, i32) -> i32`. A function's name used as a value has that type.
- Method-call syntax through a pointer: with `p: *T`, `p.f(args)` becomes `f(p, args)` when `f` takes `*T`, and `f(*p, args)` when `f` takes `T`.
- A field wins over a method. When `T` has a field `f`, `v.f(args)` calls the function pointer stored in that field. The method rewrite applies only when `T` has no field named `f`.
- `==` is not defined on structs, arrays, slices or `str`. `< <= > >=` apply to numeric types and to `char`, which compares scalar values.

## Lexical rules

- Comments: `//` to the end of the line, and `/* */`, which do not nest. Doc comments have a line and a block form with one meaning: `///` and `/** */` document the next item, `//!` and `/*! */` the module for its user, `//#` and `/*# */` the next item for the library's developers, `//#!` and `/*#! */` the module for its developers.
- [provisional] `////`, `/**/` and `/***` open ordinary comments. Reason: rule lines and banners stay comments, as they do for Doxygen and rustdoc.
- A string literal may span lines, and a CRLF inside one becomes a single LF. Reason: one source gives one set of bytes whichever line ending the file carries.
- A doc comment block of one line has its text between the opener and `*/`, the form `/** Dot product. */`. Over several lines it has its text on the lines between the opener and `*/`, and text on either delimiter line is a lexical error.
- [provisional] Two doc comments with one marker before one item join with a blank line between them. Reason: no text is lost, and the blank line is the paragraph break of the doc markup.
- Module docs are the `//!` and `//#!` comments before the first import or item. A doc comment before anything other than an item or a field is dropped, and `--doc-warnings` reports the loss with the marker the reader wrote. Reason: documentation that vanishes without a word is the one failure a doc tool must not have.
- Escapes: `\n \r \t \\ \" \' \0 \xHH \u{H}` with 1 to 6 hex digits in `\u{}`. In `str` and `char` literals `\xHH` stops at 7F and `\0` is rejected. In byte strings `\xHH` takes any byte and `\0` is allowed. Non-ASCII characters in a byte string are stored as their UTF-8 bytes, so `b"é"` has length 2.
- Hash delimiters apply to all four forms: `#"..."#`, `r#"..."#`, `b#"..."#` and `br#"..."#`. In `#"..."#` and `b#"..."#` escapes still apply, and the hashes allow an unescaped `"` inside.
- A UTF-8 byte order mark at the start of a file is skipped.
- Integer literals: lowercase `0x`, hex digits in either case, `_` only between two digits. A decimal literal other than `0` does not start with `0`. No `0b` or `0o` prefixes.
- Float literals have digits on both sides of the `.`, an optional exponent with `e` or `E` and an optional sign, and `_` separators. `1e9` is not a float literal.
- Inside every string literal a CRLF line break becomes LF, so the program does not depend on how Git checked the file out.
- Keywords: the statement keywords, `as`, the type names, `true`, `false`, `null`, `alloc`, `free`, `size_of`, `union`, `export` and the threading words reserved above. `packed` and `align` are contextual words, not keywords.

## Program entry

- `main` has one of three signatures: `fn main() -> int`, `fn main(args: []str) -> int` or `fn main(args: []str, env: []str) -> int`.
- `args[0]` is the program name as the OS passed it. `env` holds `NAME=value` entries in OS order. No Map type in the core. A lookup over `env` is a loop, or a standard-library function later.
- Both are valid `str` values. The runtime converts argv and the environment to UTF-8, replaces invalid sequences with U+FFFD and NUL-terminates each entry. On Windows it reads UTF-16 through `GetCommandLineW` and `GetEnvironmentStringsW`.
- A C startup file, `rt/start.c` in `anti_rt`, does the conversion, defines the C `main` and calls the Anti `main` under its mangled name. It reaches that name through the global symbol `anti.rt.main`, which antic defines as a second name for the main module's `main` with `.set`.
- Exit code: Linux and macOS keep the low 8 bits, Windows keeps 32 bits.

## Libraries and runtime

- [provisional] antic takes the package header from `--package-name`, `--package-version`, `--dependency <name>,<constraint>,<url>`, `--license`, `--license-text <file>` and `--attribution`. Without them it writes the module path, version `0.0.0` and empty licence fields. Reason: `anti` passes what `anti.toml` holds, and antic keeps working without a manifest.
- [provisional] The public interface of a library file also holds the `//!` text of the module, and `--strip-docs` removes it with the rest. Reason: user docs are built from a `.antl` alone.
- Doc warnings belong to `anti check`. It passes `--doc-warnings` to antic, which then warns about a `//#` note on a `pub` item without a `///` comment, and about a doc comment that documents nothing. Without the option antic is silent about documentation.
- The library format has a version, `ANTL_VERSION` in `src/antl.h`, which is 10 today. Every change of the format raises it, and antic refuses a file with another number.
- Libraries are distributed as serialised IR. Object files would need six variants. Source alone would force a full front-end run on every import. `antic -c geometry.anti` writes `geometry.antl`, one library file per module. The file holds a format version, a package header, the public interface with the text of each `///` comment, and the unoptimised IR of all function bodies. The package header holds the package name, version, dependencies with repository URLs, `license`, the full licence text and `attribution`. antic refuses mismatched versions. `antic main.anti geometry.antl` type-checks against the interface and loads the IR. It then optimises the whole program, emits one assembly file, assembles and links.
- The IR is target-independent. It records types, never sizes, offsets or register classes. `size_of`, field offsets and array strides are symbolic values that the optimizer never folds. The back end computes layout and ABI per target and replaces them with immediates. Library files are byte-identical whichever host produced them. The test suite checks that.
- The IR writes an array stride as `size_of` of the element type. Reason: C's stride is the element size, and one form serves arrays, slices and pointers.
- The first field of a struct or union has offset 0 in the IR without a symbolic value. Reason: C fixes that offset on every target.
- [provisional] The back end folds a symbolic value with wrapping at the width of its type. A division by zero is an error that names the target. Reason: the fold agrees with the same arithmetic at run time.
- After folding, the back end runs the optimizer passes again on each function that held a symbolic value. Reason: a folded size reaches the simplifications a number reaches, and the machine code stays the same as with sizes in the IR.
- [provisional] A value computed from `size_of` converts only to an integer type in a constant expression. Reason: the back end folds symbolic values as integers.
- Runtime archive: prebuilt static libraries per target under `lib/<os>-<cpu>/` plus the target-independent `.antl` files. Static linking throughout. `libs/` holds the CMake build, run per target in CI. Contents: `anti_rt`, as `libanti_rt.a` and `anti_rt.lib`, with the startup file `rt/start.c` and the thread-pool runtime, raylib (zlib), miniaudio (MIT-0), Mbed TLS (Apache 2.0) and PCRE2 (BSD). Mbed TLS 4.x splits crypto into TF-PSA-Crypto, so that is two libraries, or use the 3.6 LTS line. PCRE2 is the 8-bit build with UTF support and JIT off.
- [provisional] antic and the runtime archive are published per target, as `anti-<version>-<target>.tar.xz` under `downloads/resources/anti/<version>/`. A target is published only after the test suite has run on that target, in a VM or on a runner. Reason: a binary that nobody executed is not a release.
- CMake is pinned at the version of `tools/cmake-version`, which builds the published binaries of antic. `tools/cmake-pin` names the release archive of each host with its digest, as Kitware publishes them. Reason: the version that produced a binary is the version we tested with.
- [provisional] `tools/install.sh` and `tools/install.ps1` install Anti under `~/.anti` for the user who runs them, and the site serves them at `anti-lang.com/install.sh` and `anti-lang.com/install.ps1`. They touch no other directory and need no password. Reason: `curl -fsSL https://anti-lang.com/install.sh | sh` is the shortest path from reading about Anti to compiling a program.
- [provisional] The installer takes the package of the processor it runs on, and `--arm` or `--intel` takes the other one. `ANTI_ARCH` carries the same choice, for the `irm | iex` one-liner of Windows. That pipe passes no arguments to the script. PowerShell can pass them through a script block, and that line is long enough that a variable reads better. The rule against environment variables in `docs/decisions.md` covers the options of antic, not the installer, which answers each of its questions from a variable of its own name. A package of the other processor lands in `$HOME/.anti-<cpu>` beside the native one, and the installer offers no PATH entry for it. Reason: an Apple Silicon Mac and a Windows machine on ARM both run the x86_64 package under emulation, which is the only way to test that package without a machine of that processor.
- [provisional] The installer asks before it takes the macOS SDK stubs and before xwin fetches the Microsoft CRT. `ANTI_YES=yes` answers both for an unattended install. Reason: Apple and Microsoft license those to the user, so the user is the one who accepts.
- [provisional] `tools/package-api` holds one number, the version of the interface between an installer and the scripts a package carries. The package carries a copy, each installer names the number it needs, and the test `package_api` holds the three together. An installer refuses a package with a lower number. Reason: the installer is served from the site and is always the newest, while the scripts it drives are as old as the version installed.
- [provisional] The installer downloads that CMake from the Kitware release when the host has none, and puts it under the install directory. Reason: the installer then runs the CMake scripts of the repository rather than a second copy of them in a shell.
- [provisional] A published package holds antic for its host and the five LLVM tools. It also holds the runtime library of all six targets, the standard library and the two Linux sysroots. It therefore links a program for linux-x86_64 and linux-arm64 with nothing further installed. For its own operating system it needs the SDK of that system. Reason: those parts are ours to redistribute, and both Linux sysroots together are 24 MB.
- [provisional] The two sysroots a package cannot hold are Apple's SDK stubs and Microsoft's CRT with the Windows SDK. A user takes the first from the Command Line Tools of a Mac, and the second from the Build Tools of Windows or from xwin. Reason: neither licence allows redistribution, and the Windows sysroots are 1.5 GB.
- [provisional] Those binaries are built with clang and lld of the pinned LLVM release, cross-compiled from one machine for all six targets. The headers and libraries of each platform stay local, since Apple's SDK and Microsoft's CRT cannot be redistributed. Reason: one toolchain version then produces every artefact we publish, and the pin already names it.
- The native libraries of the runtime archive are published as binaries, one static library per target under `downloads/resources/<library>/<version>/`. `libs/` builds them from the pinned sources with the pinned compiler and the same flags a build from source uses. The published file is what that build writes. Reason: a user would otherwise cross-compile six targets and install the headers that raylib wants on Linux.
- A published library archive carries the licence text of its library and nothing else. The file name holds the library, the version and the target. `libs/CMakeLists.txt` with the pins of that commit holds the compiler, the flags and the sysroot. Reason: a second copy of those facts drifts from the build that wrote the file.
- Every C file of the repository and of the cross builds is compiled with `-ffile-prefix-map`, which replaces the source and build directories with a dot. The test `no_paths` reads the runtime libraries and antic and fails on a path of this checkout. Reason: debug information and `__FILE__` otherwise carry an absolute path of the machine that built a file into every program that links it.
- System libraries cannot be bundled: OpenGL, X11 or Wayland, Cocoa frameworks, gdi32 and winmm, CoreAudio. They go on the driver's link line per imported module and into the install list.
- Linux has two link modes. The default is the static PIE against musl. A program that imports `anti.raylib` or `anti.miniaudio` links dynamically against glibc instead. Its sysroot comes from the packages of Debian, pinned as the other sysroots are, and the system libraries stand on the link line. Reason: raylib and miniaudio load X11, Wayland, OpenGL and ALSA or PulseAudio at run time. Those libraries are built against glibc, and a static musl executable cannot use them at all. Every Linux game ships the second way, and chapter 23 says so in a paragraph.
- The native libraries are built in the order PCRE2, Mbed TLS, miniaudio, raylib. Reason: the awkward one comes last, and the first three unblock `anti.regex` and `anti.net` early.
- Bindings: `anti bind raylib_api.json` writes `anti.raylib`, and `anti bind --clang miniaudio.h` writes `anti.miniaudio`. Nothing in a binding is hand-written. `static inline` C functions get exported wrappers in a generated shim. An ABI probe compares sizes, alignments, offsets and bitfield bytes between C and Anti on every target. Struct-by-value calls into raylib double as the ABI test suite.
- The runtime archive also holds `llvm-ar` from the pinned LLVM release, Mozilla's CA bundle as `lib/cacert.pem` and a `licenses/` directory with one file per component.

## Threading

- Part of the language. First extension chapter, after the six-target test suite.
- One model: structured fork-join over an array. `worker fn f(chunk: []T, ...) -> R` may run on a worker. `parallel arr -> f` and `parallel arr by N -> f` split `arr` into contiguous disjoint slices, run `f` on each through a worker pool, block until all return, and yield the results by value in chunk order.
- Safety: the runtime does the splitting. Worker parameter and return types are slices of pointer-free element types plus pointer-free values. `str` counts as pointer-free because it is immutable. The array is unreachable during the block because the dispatching thread is blocked in it. The guarantee is no data races, not memory safety. A `parallel` that cannot be scheduled runs its jobs inline. Workers may alloc and free their own heap memory.
- Worker count is detected at run time on the target machine (`sysconf` on Linux and macOS, `GetActiveProcessorCount` on Windows), overridable with `ANTI_THREADS`. `by N` sets the chunk count, never the thread count. Default chunk count is the worker count.
- [provisional] The worker stands after the arrow, alone or called: `parallel a -> f` and `parallel a by 4 -> f(x, y)`. The arguments of that call reach every chunk after the chunk itself, and `by` is a contextual word rather than a keyword. Reason: a worker that takes a factor or a limit needs those values, and the safety rule already allows pointer-free values beside the slice.
- `parallel` yields `[]R`, allocated by the runtime, because the runtime decides the chunk count when the program writes no `by`. The slice is ordinary heap memory, and the program frees it with `free(r.ptr)`. Reason: the count is known on the machine that runs the program, and a `parallel` in a loop would otherwise leak a slice per iteration. The test `program_parallel_loop` dispatches a thousand times and frees each result. Without the free it leaks 999 blocks and 47,952 bytes, which `leaks` reports.
- [provisional] The split gives the first `count % chunks` chunks one element more than the rest. A chunk count above the element count becomes the element count, and an empty array yields an empty result without touching the pool. Reason: contiguous disjoint slices that cover the array are what makes the construct free of data races.
- [provisional] The thread that writes `parallel` takes chunks beside the pool, and the pool holds one thread fewer than the worker count. A `parallel` that meets a busy pool runs its chunks in that same thread. Reason: a machine of one processor still runs every chunk, and a worker that dispatches cannot wait for a pool that only it could free.

## Standard library phase

- Starts after the core compiles on all six targets, which it now does. The scope below is decided.
- The modules that need the C library alone come first. `anti.io` gains reading a line of standard input beside its output and its exit code. `anti.fs` opens, reads, writes and closes a file, gives its size, lists a directory, removes and renames. `anti.os` gives the arguments and a lookup in the environment, the time as seconds and nanoseconds since the epoch, a sleep, the exit code of the process, and the names of the host operating system and processor. `anti.text` gains `compare`, `find`, `starts_with`, `ends_with`, a split into `[]str`, `trim`, a byte-buffer builder, and the conversions between text and the integer and float types. `anti.math` holds the functions of libm with `min`, `max`, `abs` and `clamp` per numeric type. `anti.mem` holds an arena, an allocation from it, and a free of the whole arena. <!-- docs-style:ignore --> `anti.license` stays as it is built.
- The four modules that need the native libraries of the runtime archive follow: `anti.regex`, `anti.net`, `anti.miniaudio` and `anti.raylib`.
- Interpolation with `f"..."` is compiler work over `anti.text`, and chapter 26 gives it a section of its own at the end.
- Out of scope, and the chapter says so. Containers beyond `[]T` need generics and belong to the closing guide. So do JSON, dates and calendars, path work beyond a join and a split, and anything threaded beyond `parallel`.
- Chapter 26 therefore holds seven sections and four appendices, and it unblocks chapter 27.
- Networking wraps Mbed TLS `net_sockets.c` for plain sockets and adds TLS on top. Blocking model only. The CA bundle comes from the runtime archive. On Linux `--system-certs` reads `/etc/ssl/certs` instead. macOS Keychain and Windows CryptoAPI are not read.
- Regular expressions through PCRE2 as the module `anti.regex`. Patterns are strings. No regex literals.
- Paths are passed to the OS unchanged. Windows accepts forward slashes. No drive-letter translation.
- `std/` holds `anti.io` with console output and the exit code, `anti.text` with `equal`, `from_c` and the length in bytes and in characters, and `anti.license` with `text`. Each module has `//!` documentation, `///` on every `pub` item and a test.
- [provisional] The CMake build writes the standard library as library files into `std/` of the runtime archive, as the package `anti` with the antic version and 0BSD, and antic searches that directory after the `-I` roots when `--runtime` is given. Reason: a program imports `anti.io` without a manifest, and every module of a package lies under the package root, which `anti.io` and `anti.text` do under `anti` and not under `anti.std`.
- [provisional] The lengths of `anti.text` are `byte_count` and `char_count`, and `char_count` counts the bytes that start a UTF-8 sequence. `anti.io` has `print`, `println`, `eprint`, `eprintln` and `exit`. Reason: a `str` holds valid UTF-8, so the starting bytes are the characters.
- [provisional] The modules call C functions of the runtime: `anti_rt_write` and `anti_rt_exit` in `rt/io.c`, `anti_rt_text_from_c` in `rt/text.c` and `anti_rt_license_text` in `rt/license.c`. `from_c` returns a `str` into the C string. Reason: Anti builds no `str` from a pointer and a length, and C names `stdout`, `stderr` and `anti_licenses` directly.
- A bundled runtime leaves out the object of `rt/license.c` and takes the object `anti_rt_license_stub` of the runtime archive instead. Its `anti_rt_license_text` returns an empty text, so a library that reads the notice always links, and the notice of the archive comes from its package header. The stub lies beside the runtime library, never in it, so an executable keeps the real text.

## Build tool and distribution

- A published version is never republished. A new build of the same contents gets the next patch version. Reason: a lock file records the digest of a version, and two digests under one version break it.
- No page of the book quotes a version number that the tooling prints. The number comes from a site parameter or a placeholder, and the tests that check a listing substitute it. Reason: a version bump would otherwise touch chapter text, which is how a bump gets postponed.
- The repository holds no binary. Everything a user downloads is served from the prefix in `tools/download-base`, today `https://anti-lang.com/downloads/resources`, under `<component>/<version>/<file>` with a `SHA256SUMS` beside it. `docs/distribution.md` gives the layout. Reason: a pinned path stays valid for every version we ever publish, and the files can move to a CDN without a change in the repository.
- `anti` is declarative. It reads `anti.toml`, writes `anti.lock` and has no scripting, custom steps or plugins. `antic` keeps working without a manifest, with every library file on the command line.
- A package is named by a module path, the path of its root. Every module of the package has a path under that root. Each module compiles to its own `.antl`, and a package version publishes all of them. Keys in `[dependencies]` are quoted, and the TOML subset of the manifest parser supports quoted keys.
- A repository is a URL prefix with static files. `<name>/index.toml` lists every version, with `revision`, yanked versions kept, dependencies by repository URL, and the modules of each version with their SHA-256 digests. The resolver knows which `.antl` files to fetch from the index alone. Repository URLs are `https://` or `file://`, and `http://` only for `127.0.0.1` and `localhost`.
- Build modes: dev mode is the default. It compiles one module per `antic` call into its own object file and caches by input digest, compiler version and target. Calls across modules go through the mangled symbols. Release mode compiles the whole program in one call. Both produce `build/<os>-<cpu>/<mode>/` and `dist/<os>-<cpu>/<mode>/`.
- The dev mode flag of antic is `--dev`. It compiles the input module alone into `<base>.s` and `<base>.o`, with the functions of imported modules as external symbols. A module with `main` is then linked with the object files on the command line. Every function of the module is a global hidden symbol. Reason: one flag on the existing single-input command, as the design states.
- `antic --dev` also takes a library file as its input and writes the object of its module from the IR in the file. That object never links. Reason: dev mode compiles every `.antl` of the dependency graph one per call, and a main module comes from source.
- Release mode calls `antic -c` for every module and then antic once with the main module and every library file. Reason: antic already optimises the whole program from the IR in library files, so it needs no second source input.
- Formatter style: one tab per indentation level, a wrapped line one tab deeper, nothing aligned past the indent. An item body opens `{` on its own line. A statement block opens `{` on the statement's line, with `} else {` and `} while cond` on one line. Parentheses around conditions are dropped. `anti html` and `anti tex` render a tab as four spaces.
- [provisional] Until `anti fmt` exists, `tools/scripts/format_anti.py` applies the formatter style, and it separates items by an empty line. Until `anti html` and the listing shortcode exist, chapter listings stay fenced `anti` blocks in the formatter style, with four spaces for each tab. Reason: four spaces are what `anti html` renders for a tab, and a fenced block renders on the site today.
- Doc text is a Markdown subset: paragraphs, fenced code blocks with a language tag, inline code, `-` lists and links. `anti check` compiles every `anti` block through the front end without running it. Blocks in user doc comments compile as a separate module that imports the documented one. Blocks in developer doc comments compile inside it.
- [provisional] antic writes a library for C with `--lib static` or `--lib shared`, which the design gives to `anti build --lib`, and writes `<name>.h` beside it. The name is the file name of `-o` without `lib` and its suffix, or the last segment of the module path. `--llvm-ar` names llvm-ar. Reason: the output is compiler work, so it goes into antic and its driver.
- [provisional] The header maps `c_long`, `c_ulong` and `c_wchar` to `long`, `unsigned long` and `wchar_t`, and the other `c_` types to their `<stdint.h>` names, as `int32_t` for `c_int`. Reason: the fixed-size `c_` types are the sized types, and a library file records types and not spellings.
- The public interface of a library file keeps the parameter names of every function. The generated header and `anti doc` use them.
- [provisional] In the generated header a parameter or field name that C11 or C++17 reserves, or that a macro of `<stdbool.h>`, `<stddef.h>` or `<stdint.h>` takes, gets a trailing `_`, as `default_`. Reason: the header compiles as C and C++, and the suffix keeps the Anti name readable.
- [provisional] A static library keeps the copy of its package header in `<name>.package.o`, an object whose section `anti_package` holds the bytes. Reason: Apple's linker refuses an archive member that is not an object file.
- [provisional] `--bundle-runtime` joins the library object and every runtime member except the object of `rt/start.c` into one relocatable object with `ld -r`, keeping hidden symbols global on macOS. On Windows the runtime members go into the archive beside the library object. Reason: one object makes two bundled runtimes a duplicate symbol at link time, and COFF has no relocatable link.
- [provisional] `anti_licenses` holds lines between `ANTI_LICENSES_BEGIN` and `ANTI_LICENSES_END`: `package <name> <version> <license>`, `attribution <line>`, then `text for <names>` and each distinct text once. The runtime is the package `anti.rt` with the antic version and 0BSD. A package name appears once, with the fields of its first occurrence. Reason: plain text lines are what `anti license --from` prints.
- [provisional] `--soname` needs `--package-version`. On Linux the library is `lib<name>.so.<major>` with the soname of that file and a `lib<name>.so` symlink. On macOS the install name is `@rpath/lib<name>.dylib` with compatibility version `<major>.0.0` and the full current version. Reason: these are the platform conventions that the design names.
- [provisional] The shared library's constructor calls `anti_rt_init` from `rt/init.c`, which an executable's `main` calls first. A Windows DLL's `.def` file lists every export fn and `anti_licenses DATA`. Reason: one initialisation function serves both, and the `.def` file keeps the exports of a DLL to the export items.
- Publishing goes to targets in `~/.anti/config.toml` through the transports `file://`, `ssh://`, `git+ssh://`, `git+https://` and `cmd:`. `anti publish` needs `license` and `license_text`, stages, uploads the version directory before the index, and verifies the new entry over HTTPS.
- Two sites. The book is a chapter series on foundingfuture.com. The language has its own site at anti-lang.com. That site carries the installer, the downloads, the examples and the documentation of the language. It also carries the extensions, such as a layer for graphics over raylib. Reason: a reader of the book and a user of the language come for different things. A language with its own domain also outlives the series that built it.
- The GitHub templates `anti-repository` and `anti-library` are created only after `anti` exists and has published one release by hand. Their workflows follow the CI rule: on a release tag or on demand.
- Licence data: an executable and a shared library carry the read-only object `anti_licenses` between fixed markers, which `anti license --from` reads. A static archive carries no `anti_licenses`. It carries a copy of the package header of its `.antl`, which `anti license --from-archive lib<name>.a` reads, so a C project can produce its notice mechanically.
- Libraries for C: `anti build --lib static` writes `lib<name>.a` or `<name>.lib` with `llvm-ar`, without the runtime unless `--bundle-runtime` is given, and prints the link line with `libanti_rt.a`. `anti build --lib shared` links the runtime in, initialises it from a constructor section and hides every symbol except the exports. `anti bind --header` writes the C header from the `.antl`. `*byte` becomes `uint8_t*`, and `int`, `uint` and `float` become `int64_t`, `uint64_t` and `double`.

## Compiler behaviour

- The ABI probe compares the size and the alignment of a type. It compares the offset and the value of each field. It compares a whole byte image only for a struct that both languages zeroed with `memset` first. The bitfield cases need that image. Reason: C leaves the padding of a struct indeterminate. A probe that compares whole images asserts what C never promised. A probe built by a C compiler alone would fail it on a bad day.
- A constant aggregate is a read-only global that a use copies from, rather than a stack slot that lowering fills field by field. The IR carries the constant as a typed tree. The back end writes the bytes with the padding zeroed, because the back end is what computes the layout. Reason: it removes a copy from every use of a constant, and the bytes of a program stop depending on what the frame held.
- Global data that holds an address goes to a section of its own: `.data.rel.ro` on ELF, `__DATA,__const` on Mach-O and `.rdata` on COFF. The emitter writes the address as `.quad` with the symbol it names. Reason: a `str` constant holds the address of its literal, and the loader writes that address. A section mapped read-only from the first page cannot hold it. clang chooses the same three sections for the same value, and PE protects `.rdata` after its base relocations.

What antic does that the design above leaves open, as far as a user of the language or of the tools sees it. The choices inside each pass live in `docs/notes/chapter-NN.md`, beside the chapter that made them.

- antic prints each stage with `--dump-tokens`, `--dump-ast`, `--dump-types`, `--dump-ir`, `--dump-opt`, `--dump-select` and `--dump-alloc`. `--print-targets` prints the target matrix. The listings of chapter 1 are pinned by tests.
- Driver options: `-o`, `-S`, `--target`, `--llvm-mc`, `--runtime` and `--print-host-target`. No environment variables. Without `-o` the executable is the input path without `.anti`. The `.s` and `.o` files stay beside the executable.
- Chapter 4 lexer: it validates the UTF-8 of the whole file before the first token. A file with invalid UTF-8 yields one diagnostic. After a byte order mark, positions start at line 1, column 1.
- The lexer stores a float literal as its digits without `_`. Semantic analysis converts them to `f32` or `f64` by nearest rounding.
- The parser rejects an expression statement that is not a call or `free`, the syntax-level rule of chapter 2. It leaves the check that an assignment target is a place to semantic analysis.
- Chapter 9 symbol form on Windows: `_A`, the decimal length and the text of each segment of the module path, `_` and the function name. The form stays, whatever `link.exe` accepts. `geometry.length` becomes `_A8geometry_length`, and `com.example.scale.scale` becomes `_A3com7example5scale_scale`. It holds only letters, digits and `_`, and the length keeps two name pairs apart. C reserves names that start with `_A`.
- `antic -c file.anti lib.antl...` writes `file.antl` beside the source, or at `-o`. Every library a program needs, transitive imports included, goes on the command line in any order. antic loads each file after the files it imports. `--dump-ir` prints the whole program after loading.
- The interface in a library file holds the `pub` `fn`, `extern fn`, `struct` and `const` items, each `const` with its value. Every struct of the module that those items reach is stored with its fields, private ones included. The `///` text of the fields of a private struct is not stored. A struct of another module is stored as its module and name.
- A method call on a struct of another module finds the `pub` functions of the module that declares the struct.
- The optimizer always runs on the whole program. antic has no option to skip it. `--dump-opt` prints the optimized IR.
- antic generates position-independent code on all six targets. The xnu loader requires PIE for ARM64 executables, and Windows cannot disable ASLR on ARM64.
- A float aggregate of 1 to 4 members of one float type is homogeneous on all three ARM64 conventions, the AAPCS64 rule. Microsoft's glossary starts at 2 members, and its return rules accept any non-empty one. The Windows VM tests a struct of one `f32` against MSVC.
- With `--linker platform` antic links only for the operating system it runs on, and reports `linking for` the target `with the platform linker needs a host with the same operating system` otherwise. The command lines live in `src/linker.c`.
- macOS: `ld -arch arm64|x86_64 -platform_version macos 11.0 <sdk version> -syslibroot <sdk path> -o exe obj libanti_rt.a -lSystem`, with the SDK from xcrun.
- Linux: `ld -pie --dynamic-linker=<interpreter> -o exe Scrt1.o crti.o obj libanti_rt.a -L<dir> -lc crtn.o`. The start files come from the first of `/usr/lib/<multiarch>`, `/usr/lib64` and `/usr/lib` that holds `Scrt1.o`. The interpreter is `/lib64/ld-linux-x86-64.so.2` from the psABI and `/lib/ld-linux-aarch64.so.1` from GCC's configuration, which no specification names. Only the platform linker needs a path. lld links a static PIE without one.
- Windows: `link.exe /NOLOGO /debug:none /SUBSYSTEM:CONSOLE /MACHINE:X64|ARM64 /OUT:exe obj anti_rt.lib msvcrt.lib libvcruntime.lib ucrt.lib legacy_stdio_definitions.lib`. The last library is there because `printf` and its family are inline functions in the headers of the UCRT, and `ucrt.lib` exports none of them. Without it an `extern fn printf` fails to link with an undefined symbol. The Universal CRT is a part of Windows since Windows 10, so `ucrt.lib` links against the C library of the machine, as musl and libSystem are the C library of the other two. Only `vcruntime`, the support code of the compiler, comes in statically, because that one ships with Visual Studio rather than with Windows. The executable then imports the API sets of the UCRT and `KERNEL32.dll`, needs no redistributable, and weighs 23,040 bytes for a program that prints a line instead of 91,136. A fully dynamic link weighs 12,800 and imports `VCRUNTIME140.dll`, which a machine without Visual Studio does not hold. The runtime library is `anti_rt.lib` on Windows and `libanti_rt.a` elsewhere. Without `-o` the executable gets the suffix of the target.
- [provisional] llvm-readobj of the pinned release joins the tools that `tools/get-llvm.cmake` installs, that `tools/check-llvm.cmake` checks and that the runtime archive carries. The tests `unwind_windows-x86_64` and `unwind_windows-arm64` decode the unwind data of `tests/dump/unwind.anti` with it. Reason: llvm-objdump 23.1.1 decodes only the x64 form.
- `tools/get-llvm.cmake` downloads the archive that `tools/llvm-pin` names for the host and checks its SHA-256 digest. CMake downloads, checks and unpacks it, so the one script serves every host with an archive. Only macOS on x86_64 has none.
- The download area holds the version directories of a component and nothing else. The publish step checks that what stands there is what the index lists, and refuses anything it does not name.
- The download area holds one directory per component, and the name of the component carries the kind of artefact. Planned components beside `llvm` are `anti` for the runtime archive, `raylib`, `pcre2`, `mbedtls` and `miniaudio` for the prebuilt native libraries, and `musl` for the Linux sysroots. Reason: a pin names an exact URL, so no machine browses the tree, and a level for the kind would have to classify files that sit between two kinds.
- The macOS SDK stubs and the Windows CRT and SDK are never served from the download area. Reason: they are Apple's and Microsoft's, and neither licence allows redistribution.
- The archives of the five LLVM tools are ours, at `downloads/resources/llvm/<version>/` of anti-lang.com. `tools/pack-llvm.cmake` builds them from the release archives of LLVM, with the licence beside the binaries. Reason: a user downloads 46 MB rather than 1.6 GB, and a new LLVM version is our work, not theirs.
- The two Linux archives hold tools that we build, not the ones the LLVM release carries. `tools/build-llvm.cmake` holds the recipe: LLVM of the pin, lld, the X86 and AArch64 back ends, and ICU, zlib, zstd, libxml2 and libedit off, linked with `-static -s`. `tools/llvm-upstream` marks the host as built and `tools/pack-llvm.cmake` reads `BUILT_<host>`. It refuses any Linux tool that names a shared library outside the system set. Reason: the Linux release of ld.lld needs `libicui18n.so.70` of one Ubuntu release, so it does not start on another distribution, and that failure reaches a user rather than us.
- x86_64 frames of `2^31` bytes or more are a compile error, the proposal of the former Open item. `sub` and memory displacements hold 32 bits.
- The driver links each `.o`, `.a`, `.obj` or `.lib` on the command line after the program object and before the runtime library.
- `tools/get-sysroot.cmake` installs the sysroots into `build/sysroot`, which the CMake cache variable `ANTIC_SYSROOT_DIR` names, and the build copies them into the runtime archive. The Linux sysroot takes musl 1.2.6 from the Alpine Linux 3.24 package `musl-dev` and `libclang_rt.builtins.a` from its package `compiler-rt` 22.1.3, pinned by SHA-256 in `tools/sysroot-pins`. Reason: the packages ship a built `libc.a` and the builtins for both architectures, and musl's `printf` on ARM64 needs the 128-bit float builtins.
- A macOS executable keeps the three segments that Apple's hardening expects, and `-no_data_const` stays off. On Apple Silicon the page size is 16 KB, so an empty program weighs about 50 KB, of which 32 KB is the padding of the segments. Chapter 23 shows where those bytes go in a table. Reason: 16 KB is not worth the protection of read-only data, and a smaller image is an option a user turns on rather than a default.
- An Anti executable and an Anti shared library carry no debug information, on every target and with either linker. ELF spells it `--strip-debug`, Mach-O `-S` and COFF `/debug:none`. The unit test `strips_debug` builds both command lines for all six targets and fails when one of them loses its spelling. Reason: antic emits no debug information of its own, so every byte of it would come from the C library of the target, and a debugger still shows no Anti line. The symbol names stay, so a backtrace names the functions. The day antic learns `-g`, that option turns the flag off.
- [provisional] ld.lld links a Linux executable statically against musl as a position-independent executable: `-static -pie --no-dynamic-linker --strip-debug` with `rcrt1.o`, `crti.o`, `libc.a`, `libclang_rt.builtins.a` and `crtn.o`. The debug sections come from the musl of Alpine, and antic writes none, so `--strip-debug` takes `tests/programs/letters.anti` for linux-arm64 from 98,392 bytes to 21,288. An ELF shared library links no C library. Reason: a static PIE runs on any Linux kernel without a C library on the system, and a shared library takes the C library of its process.
- The macOS sysroot takes the stubs of the newest SDK of the Command Line Tools that the pinned ld64.lld reads. It records the version in `sdk-version` for `-platform_version`. On the development Mac that is MacOSX26.5.sdk. Reason: ld64.lld 23.1.1 refuses the target `arm64e.x1` in the stubs of MacOSX27.0.sdk.
- ld64.lld has no relocatable output, so `--bundle-runtime` on Mach-O joins with Apple's `ld -r` of the host. Reason: ld64.lld 23.1.1 prints that `-r` is not yet implemented.
- antic runs the lld programs from `bin/` of the runtime archive when it holds them, and from the search path otherwise. A Windows link without a sysroot lets lld-link read the variable `LIB`. Reason: the archive carries the pinned tools, and an MSVC environment sets `LIB`, which lld-link reads as link.exe does.
- `tools/get-sysroot.cmake` runs xwin only when its caller passes `ACCEPT_LICENSE=yes`, and installs the pinned xwin when the path has none. Reason: xwin requires accepting the Microsoft licence terms, which only the owner of the build can accept.
- The Windows sysroots are pinned like the Linux one. `tools/sysroot-pins` holds xwin 0.10.0, the CRT version 14.44.17.14, the SDK version 10.0.26100 and the SHA-256 digest of each tree, and `tools/get-sysroot.cmake` checks all of them. An unpinned sysroot would repeat the problem of an unpinned LLVM from Homebrew.
- [provisional] The digest of a Windows sysroot hashes the sorted SHA-256 lines of its regular files and leaves symbolic links out. Reason: xwin adds links for other spellings only on a case-sensitive file system, and on the development Mac it disables them.
- [provisional] `tools/get-sysroot.cmake` also refuses an xwin of another version and keeps the downloads in `.download/xwin` of the sysroot directory. Reason: the version of xwin decides which files the tree keeps, and a cache in the working directory would land in the repository root.
- clang compiles the runtime library for every other target that has a sysroot, with `-O2`, hidden visibility and the warnings as errors. llvm-ar archives it into `lib/<target>/`. Reason: a link for a target needs the runtime of that target, and the CMake build otherwise makes it for the host only.
- raylib is pinned in `tools/raylib-pin` at 6.0. `tools/get-raylib.cmake` downloads the source archive and checks its SHA-256 digest. CMake downloads and unpacks it, so the one script serves macOS, Linux and Windows. GitHub publishes no digest for source archives, so the digest is the one of the archive at its first download. The CMake cache variable `ANTIC_RAYLIB_DIR` names the extracted release, and `program_abi_raymath` fails when it is missing.
- `rt/start.c` always calls `anti.rt.main` with `args` and `env`. All six conventions pass both slices in registers or as pointers in registers, so a `main` with fewer parameters ignores them. The runtime allocates the slices and strings with `malloc` and never frees them.
- On Linux and macOS the runtime reads `argv` and the POSIX variable `environ`. `rt/utf.c` replaces each maximal subpart of an ill-formed UTF-8 sequence with U+FFFD, the practice of section 3.9.6 of the Unicode Standard.
- On Windows the runtime splits `GetCommandLineW` by the rules of Microsoft's page "Parsing C command-line arguments", reads `GetEnvironmentStringsW` and converts UTF-16 to UTF-8 with U+FFFD for an unpaired surrogate. The conversions have unit tests on the development Mac. The Windows branch of `rt/start.c` is only syntax-checked with clang against a stub `windows.h`.
- `tests/excerpts.txt` lists every C and CMake fence of a chapter that a repository file holds verbatim. A line names the chapter, the number of the fence and the file. One ctest test per line, `excerpt_<chapter>_<number>`, finds the fence in that file. `tools/scripts/excerpts.py` writes the list. Reason: the listings of antic output have a test each, and a code excerpt then has one as well.
- Code that the finished compiler no longer holds lives as files in the chapter folders and builds and runs as programs of its own. These are the first `rt/start.c` of chapter 1 and the direct path of chapters 3 to 5. Every C and CMake fence of the book then has a file, and `tests/excerpts.txt` lists all of them.

## Source code of the chapters

- The repository holds one complete source tree and nothing else. No chapter fragment, no per-chapter copy of a file, no partial version of the compiler ever enters git. Settled on 2026-09-17.
- The code a reader gets per chapter is recreated after the book is finished, outside the repository, and shipped as one zip that holds `chapter01` to `chapterNN`. It is a download, not a branch, not a tag and not a directory of the repository.
- A chapter runs the tests that belong to it and prints the summary line alone, as `100% tests passed out of 7`. The tests ship in that chapter's source. No chapter lists the code of a unit test, and none discusses one at length. The exception is a chapter whose subject is the test itself. The book is about writing a compiler.
- The run lines are written again when the chapter zips are built. The command a reader runs has no excerpt tests to exclude, and its count is the chapter's own.
- The closing pointer sends the reader to the anti-lang repository for the real source, which is the single complete tree.

## Not implemented or untested

Work that is decided but not built, and code that no test runs on its target yet.

- Decided and not implemented: selection computes the address of a stack slot at each use. Lowering computes all of them in the entry block today. The addresses of a function with many address-taken locals then stay alive across its calls and spill. A function with 520 such locals on linux-arm64 saves ten callee-saved registers and spills the rest.
- Decided and not implemented: the `anti` tool, with `anti fmt`, `anti doc`, `anti check`, `anti bind`, `anti syntax`, `anti license`, the resolver, the cache, publishing and the two GitHub templates, and the standard library modules `anti.net`, `anti.regex`, `anti.raylib` and `anti.miniaudio`, which wait for the native libraries of the runtime archive. The tests of `docs/tooling.md` that need `anti` wait for it: doc equivalence through `anti doc`, header consistency with `anti.toml`, format stability, highlighter coverage, the resolver and the offline build.
- Untested off the development Mac: the MSVC bitfield rule and `c_long` and `c_wchar` at 32 and 16 bits run only as layout unit tests checked against clang's sizes. Shared libraries, `.def` files and `.CRT$XCU` constructors on Windows, `.init_array` and `--soname` on Linux, and the 16-aligned register rule of AAPCS64 outside Apple are assembled with llvm-mc and pinned by unit tests, not linked or run.
- The platform command lines come from the GNU ld manual, the glibc sources and Microsoft's linker and CRT pages. The lld command lines come from the ld.lld manual and the musl and xwin sources. Unit tests pin both. Whether `link.exe` needs `kernel32.lib` or other libraries beside the CRT ones is unknown, because lld-link is what runs. The runtime objects are compiled for the dynamic Universal CRT, `/MD` in the terms of MSVC, which matches the `msvcrt.lib` and `ucrt.lib` of the link line. The executables that lld writes for Linux and Windows now run on the two virtual machines, and all 22 program tests pass on Windows ARM64.
- macos-x86_64 programs link on the development Mac with lld against the runtime library that clang builds for macos-x86_64, which the test `cross_link_macos-x86_64` checks by format. The next item covers running them.
- The development Mac runs a macos-x86_64 program through Rosetta. Every program of `tests/programs` therefore runs for macos-arm64 and for macos-x86_64, and `program_abi_structs`, `program_abi_wchar` and `program_abi_raymath` run for both as well. The earlier claim that Rosetta hangs on a new x86_64 binary was wrong. The targets linux-x86_64 and windows-x86_64 still need their own machine.
- The x86_64 libm of the Mac rounds a few raymath results one unit in the last place away from the arm64 one. The test `program_abi_raymath_macos-x86_64` therefore has its own expected file.
- Linux programs do not run on the development Mac. The Docker daemon is not running.
- `docs/vm-setup.md` plans a Linux ARM64 VM and a Windows ARM64 VM in UTM, which Eddie sets up, with the tests each one runs. The x86_64 targets stay for the runners on a release day.
- `tools/install.ps1` has never run, and no machine here parses PowerShell. The Windows VM runs it first, with `tar.exe` for the package and the pinned CMake when the host has none.
- The packages of linux-x86_64, linux-arm64, windows-x86_64 and windows-arm64 are built and unpublished. Each holds the right file format for its host, and no test suite has run on those hosts.
- The Windows branch of `rt/start.c` has not been compiled with MSVC or run. It needs `GetCommandLineW`, `GetEnvironmentStringsW` and `FreeEnvironmentStringsW` from kernel32.
- Windows DLL functions. A C function that a DLL exports, rather than the static C runtime, has an import thunk as its address in the executable. Pointer equality with the address that the DLL itself uses is untested. Chapter 23 links the first DLL-based libraries.

## Open

- Nothing.
