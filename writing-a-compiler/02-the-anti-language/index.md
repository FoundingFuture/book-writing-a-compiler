---
title: "The Anti language"
description: "The Anti core language specification, with lexical rules, doc comments, C types, unions, bitfields, statements, modules with export and the grammar."
summary: "The specification of the core language. Types with the C types for bindings, literals, expressions, statements and functions. Structs, unions, bitfields, packed and aligned structs, modules with export, doc comments and the memory model. Lists the reserved and contextual words and the platform facts the language relies on."
date: 2026-09-13T22:32:00+02:00
lastmod: 2026-09-15T03:58:48+02:00
draft: false
weight: 20
tags: [compilers, programming-languages]
keywords: [anti language, language specification, ebnf grammar, struct layout, bitfields, c types for bindings, undefined behaviour, string literals]
---

## Previously

[Chapter 1, The parts of a compiler]({{% relref "/programming/writing-a-compiler/01-parts-of-a-compiler" %}}), describes the pipeline from source text to an executable. Its stages are the lexer, the parser, semantic analysis, the intermediate representation, the optimizer, instruction selection, register allocation and assembly emission. antic stops at assembly text. The assembler llvm-mc and the linker lld produce the executable, on six targets.

## Specification scope

Anti is a compiled, statically typed language that manages memory by hand, as C does. A compiler translates the program into machine code before it runs. Statically typed means that every expression, a piece of code that computes a value, has a type fixed at compile time. This chapter specifies the core language that chapters 3 to 21 compile. Chapter 22 adds threads and chapter 26 adds the standard library.

A target is one operating system on one processor architecture. Anti has six targets: Linux, macOS and Windows, each on the x86_64 and ARM64 architectures. A program consists of modules, and each module is one source file. The source file `main.anti` is the module `main`. An item is a declaration at module level, and five kinds exist: `fn`, `extern fn`, `struct`, `union` and `const`.

Two outcomes recur throughout the chapter. A compile error is a violation of a rule that antic reports, and antic then produces no program. Some compile errors depend on the target. The back end of antic, the part that generates code for one target, reports them and names the target. Undefined behaviour is a condition on which this specification places no requirement. A program that triggers it may produce any result or crash, and the result may differ between targets.

## Example program

The program below counts to three and prints its command-line arguments. It declares the C function `printf`, a struct with two fields, a function that takes a pointer and a `main` that receives the arguments. The sections after this one specify every construct it uses.

```anti
extern fn printf(fmt: *byte, ...) -> i32;

struct Counter
{
    hits: int,
    limit: int,
}

fn bump(c: *Counter) -> bool
{
    c.hits += 1;
    return c.hits < c.limit;
}

fn main(args: []str) -> int
{
    let c = Counter { hits: 0, limit: 3 };
    while c.bump() do {
        printf("hit %lld\n".ptr, c.hits);
    }
    printf("%s received %lld arguments\n".ptr, args[0].ptr, args.len);
    return 0;
}
```

The call `c.bump()` passes the address of `c` to `bump`. Inside `bump`, `c.hits` reads the field through that address. `.ptr` hands `printf` the bytes of each string as a C string. The program prints `hit 1` and `hit 2`, then its own name and the argument count, and exits with code 0.

## Source text

### Encoding and line breaks

A source file is UTF-8 text. Unicode is the standard that numbers the characters of the world's scripts, and ASCII is its first 128 characters. UTF-8 encodes each Unicode character as one to four bytes, and each ASCII character as a single byte[^1]. A file that is not valid UTF-8 is a compile error. A byte order mark, the bytes `EF BB BF` at the start of a file, is skipped[^1].

Outside comments and literals, source text is ASCII. The source text divides into tokens, the smallest units of the syntax, such as a keyword, a name, a literal or an operator. Space, horizontal tab, carriage return and line feed separate tokens and have no other meaning.

A line ends with a line feed, or with a carriage return followed by a line feed, written CRLF. Inside every string literal a CRLF becomes a single line feed. A literal that spans lines therefore holds the same bytes whether the file was checked out with CRLF or with line feed endings.

### Comments

`//` starts a comment that runs to the end of the line. `/*` starts a block comment that ends at the first `*/` after it. Block comments do not nest. In `/* a /* b */ c */` the comment ends after `b`, and `c */` is source text. A block comment without its closing `*/` is a compile error.

### Doc comments

A doc comment is a comment whose text antic keeps as the documentation of an item, a field or the module. Four markers exist. Each marker has a line form and a block form with the same meaning.

| Line form | Block form | Documents | Audience |
|---|---|---|---|
| `///` | `/** */` | the next item or field | every reader who can see the item |
| `//!` | `/*! */` | the module | the users of the module |
| `//#` | `/*# */` | the next item or field | the developers of the library |
| `//#!` | `/*#! */` | the module | the developers of the library |

Consecutive lines that start with the same line marker form one doc comment, and its text is the rest of each line. A line without that marker ends the comment, a blank line included. The block form holds its text on the lines between the opening line and the line of `*/`. Text on either delimiter line is a compile error. In both forms antic removes the leading whitespace that all non-blank lines share.

The two listings below therefore give `scale` the same doc text. The first listing is the test file `tests/dump/docs.anti`.

```anti
/// Scale by a factor.
///
/// Both operands are int.
fn scale(x: int) -> int
{
    return x * 2;
}
```

```anti
/**
    Scale by a factor.

    Both operands are int.
*/
fn scale(x: int) -> int
{
    return x * 2;
}
```

The openers `////`, `/**/` and `/***` start ordinary comments, so rule lines and banners stay comments. Two doc comments with one marker before the same item join into one text, with a blank line between them. The module documentation is the `//!` and `//#!` comments before the first `import` or item. A doc comment in any other position is dropped without a message.

The doc text is a subset of Markdown with paragraphs, fenced code blocks with a language tag, inline code, lists with `-` and links. A library file holds the `///` text of the public items and the `//!` text of the module. The section on modules specifies `pub` and library files. The doc warnings belong to `anti check` of chapter 24, The build tool. It passes the option `--doc-warnings`, and only then does antic warn about a `//#` note on a `pub` item without a `///` comment. A warning is a message that does not stop the compilation. For the test file `tests/errors/note.anti`, `antic --doc-warnings` prints this line.

```text
note.anti:2:8: warning: the pub item `warm` has a `//#` note and no `///` comment
```

### Grammar notation

The rules in this chapter are written in Extended Backus-Naur Form, EBNF. A rule of the form `name = definition ;` defines `name`. Text in double or single quotes stands for itself. The bar `|` separates alternatives. Square brackets `[ x ]` make `x` optional. Braces `{ x }` repeat `x` zero or more times, and parentheses `( x )` group. The name `letter` stands for an ASCII letter and `digit` for `0` to `9`. The name `nonzero_digit` stands for `1` to `9`, and `hex_digit` for a digit or a letter from `a` to `f` in either case.

### Identifiers and keywords

```ebnf
ident = ( letter | "_" ) { letter | digit | "_" } ;
```

An identifier names a variable, a function, a type, a field or a module. Identifiers are case-sensitive, so `count` and `Count` are two names. A keyword is a word with a fixed meaning in the language, and it cannot be an identifier. The table lists every keyword.

| Group | Keywords |
|---|---|
| Declarations | `const` `export` `extern` `fn` `import` `let` `pub` `struct` `union` |
| Control flow | `break` `continue` `do` `else` `if` `return` `while` |
| Types | `bool` `byte` `char` `f32` `f64` `float` `i8` `i16` `i32` `i64` `int` `str` `u8` `u16` `u32` `u64` `uint` |
| C types | `c_char` `c_double` `c_float` `c_int` `c_long` `c_longlong` `c_short` `c_size_t` `c_uchar` `c_uint` `c_ulong` `c_ulonglong` `c_ushort` `c_wchar` |
| Values and conversion | `as` `false` `null` `true` |
| Built-in functions | `alloc` `free` `size_of` |
| Reserved for threads | `atomic` `chan` `dispatch` `join` `parallel` `recv` `select` `send` `sync` `thread` `worker` `yield` |

The words reserved for threads have no meaning in the core language, and no program can use them as names. The letters `r`, `b` and `br` directly before `"` or `#` start a string literal. Anywhere else they are identifiers.

A contextual word is an identifier that has a meaning in one position of the grammar. Anti has two contextual words. The word `packed` has its meaning only directly before `struct` or `union`. The word `align` has its meaning only after the name of a struct or union and before its `{`. Everywhere else both are ordinary identifiers, so `align` may name a field or a parameter.

### Numeric literals

A literal writes a value directly in the source.

```ebnf
int_lit   = "0" | nonzero_digit { [ "_" ] digit }
          | "0x" hex_digit { [ "_" ] hex_digit } ;
float_lit = digits "." digits [ exponent ] ;
digits    = digit { [ "_" ] digit } ;
exponent  = ( "e" | "E" ) [ "+" | "-" ] digits ;
```

An integer literal is decimal, or hexadecimal after a lowercase `0x`. Hexadecimal is base 16, with the digits `0` to `9` and `a` to `f` in either case. The character `_` between two digits separates groups and leaves the value unchanged. Only the literal `0` starts with the digit `0`, so `007` is a compile error. Anti has no octal or binary literals.

A float literal has digits on both sides of the dot and an optional exponent. The exponent multiplies the value by a power of ten, so `1.5e3` is `1500.0`. A float literal denotes the nearest value of its type. `1e9`, `.5` and `5.` are not float literals. Because a digit must follow the dot, `0..n` reads as the three tokens `0`, `..` and `n`.

| Literal | Value |
|---|---|
| `42` | 42 |
| `1_000_000` | 1000000 |
| `0xFF` | 255 |
| `0xdead_beef` | 3735928559 |
| `0.5` | 0.5 |
| `6.25e-2` | 0.0625 |

A numeric literal has no sign. `-5` is the operator `-` applied to `5`, and the section on literal types states how the two form one value. A hexadecimal literal denotes a non-negative value, so `0xFFFF_FFFF` is 4294967295.

### Character literals

```ebnf
char_lit = "'" ( char_item | escape ) "'" ;
escape   = "\" ( "n" | "r" | "t" | "\" | '"' | "'" | "0"
         | "x" hex_digit hex_digit | "u{" hex_digit { hex_digit } "}" ) ;
```

Unicode assigns each character a code point, a number from `0` to `0x10FFFF`. A Unicode scalar value is any code point outside the range `0xD800` to `0xDFFF`[^2]. A character literal holds exactly one Unicode scalar value and has type `char`. `char_item` is any character except `'`, `\`, carriage return and line feed.

An escape is a backslash sequence that stands for one character or byte. Character literals and string literals share the escapes.

| Escape | Meaning |
|---|---|
| `\n` | line feed, `0x0A` |
| `\r` | carriage return, `0x0D` |
| `\t` | horizontal tab, `0x09` |
| `\\` | backslash |
| `\"` | double quote |
| `\'` | single quote |
| `\0` | NUL, the character with value 0 |
| `\xHH` | the value of the two hexadecimal digits `HH` |
| `\u{H}` | the Unicode scalar value with 1 to 6 hexadecimal digits `H` |

In a character literal, `\xHH` is at most `\x7F`. A character literal cannot hold NUL, whether written `\0` or `\u{0}`. A `\u{}` escape names a scalar value, so `\u{D800}` and `\u{110000}` are compile errors. `'a'`, `'é'`, `'\n'` and `'\u{1F600}'` are character literals.

### String literals

```ebnf
string_lit = [ "r" | "b" | "br" ] { "#" } '"' { string_item | escape } '"' { "#" } ;
```

A prefix of `r`, `b` or `br` before the opening quote selects the type and the handling of escapes. The rule `string_item` covers any character except `"` and `\`, with the exceptions for raw forms and hashes below. The type `str` is the string type and `[]byte` is a byte slice. Both are specified in the section on types.

| Form | Type | Escapes |
|---|---|---|
| `"text"` | `str` | processed |
| `r"text"` | `str` | none |
| `b"text"` | `[]byte` | processed |
| `br"text"` | `[]byte` | none |

A raw literal, with prefix `r` or `br`, takes every character between its quotes as written, including `\`. Every form may add `#` characters around its quotes, as in `#"text"#`, `r#"text"#` and `b##"text"##`. The literal then ends at a `"` followed by the same number of `#`, and a `"` inside is part of the text. Escapes still apply in `#"text"#` and `b#"text"#`. A string literal may span lines.

A `str` literal holds valid UTF-8 and no NUL. Its `\xHH` escapes stop at `\x7F`. The escapes `\0` and `\u{0}` and a NUL character in the source text are compile errors. After the last byte of the literal, outside its length, antic stores a NUL byte.

A byte string holds any bytes. In it, `\xHH` takes any value from `00` to `FF`, and `\0` is allowed. A non-ASCII character in a byte string is stored as its UTF-8 bytes, so `b"é"` has length 2. The bytes of every string literal are in read-only memory. Writing through the slice of a byte string literal is undefined behaviour.

## Types and layout

A byte is 8 bits. Every value has a type, and the type fixes the possible values, the operations and the layout in memory. The layout is a size in bytes and an alignment. The alignment is a count of bytes, and a value is stored at an address that is a multiple of it. An address is the position of a byte in memory.

Anti uses the C layout of each target. A target's application binary interface, its ABI, fixes the sizes and alignments of the C types, the layout of structs and the way functions are called. Linux on x86_64 uses the System V AMD64 ABI[^3], and Apple's calling conventions for macOS on x86_64 are the same[^4]. The ARM64 targets use AAPCS64, the Arm procedure call standard[^5]. Microsoft[^6] and Apple[^7] document their changes to it for Windows and macOS. Windows on x86_64 uses the Microsoft x64 ABI[^8].

The back end of antic computes the layout of every type for its target. The six targets agree on the size and alignment of most Anti types. The exceptions are `c_long`, `c_ulong`, `c_wchar`, bitfields and every type that holds one of them.

### Numeric types

| Type | Bytes | Values |
|---|---|---|
| `i8` | 1 | `-128` to `127` |
| `i16` | 2 | `-32768` to `32767` |
| `i32` | 4 | `-2147483648` to `2147483647` |
| `i64`, `int` | 8 | `-9223372036854775808` to `9223372036854775807` |
| `u8`, `byte` | 1 | `0` to `255` |
| `u16` | 2 | `0` to `65535` |
| `u32` | 4 | `0` to `4294967295` |
| `u64`, `uint` | 8 | `0` to `18446744073709551615` |
| `f32` | 4 | IEEE 754 binary32 |
| `f64`, `float` | 8 | IEEE 754 binary64 |

System V[^3], the AAPCS64 standard[^5] and the Microsoft x64 ABI[^8] give each numeric type an alignment equal to its size. The sized types are `i8` to `i64`, `u8` to `u64`, `f32` and `f64`. An alias is a second name of a type: `int` is `i64`, `uint` is `u64`, `float` is `f64` and `byte` is `u8`. The slice types `[]byte` and `[]u8` are therefore one type.

A signed type stores its values in two's complement. A bit pattern with the highest bit clear has its unsigned value. A pattern with the highest bit set has its unsigned value minus 256 in `i8`, minus 65536 in `i16`, minus 4294967296 in `i32` and minus 18446744073709551616 in `i64`.

`f32` and `f64` use the 32-bit and 64-bit floating-point formats of the IEEE 754 standard. System V[^3] and the AAPCS64 standard[^5] specify those formats for the C types `float` and `double`.

The types in the table have the same size on every target. Anti has no pointer-sized integer type. Lengths and indexes have type `int`.

### C types for bindings

A binding is an Anti module that declares the functions and types of a C library. The C types give a binding the type names of the C header. Eleven of them are aliases, and each alias is one type with the sized type in the table.

| C type | Same type as |
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

The other three C types are types of their own. The back end gives each of them its width on the target and an alignment equal to its size.

| Type | Values | Windows | Linux and macOS |
|---|---|---|---|
| `c_long` | signed | 32 bits | 64 bits |
| `c_ulong` | unsigned | 32 bits | 64 bits |
| `c_wchar` | signed or unsigned, as C `wchar_t` | 16 bits | 32 bits |

C `long` is 4 bytes on Windows[^9]. It is 8 bytes on Linux and macOS, as System V[^3], the AAPCS64 standard[^5] and Apple's ARM64 notes[^7] state. The raylib header declares `long GetFileModTime(const char *fileName)`[^10], and a binding gives that function the result type `c_long`. C `wchar_t` is 2 bytes with the values 0 to 65535 on Windows[^9]. The AAPCS64 standard gives it 32 bits in its SysV-like LP64 data model and 16 bits in its Windows-like LLP64 data model[^5].

The signedness of C `wchar_t` differs between the six targets. Apple clang 21.0.0 on the development Mac shows it with `clang -target <triple> -dM -E -x c /dev/null`. The output defines `__WCHAR_UNSIGNED__` for `aarch64-unknown-linux-gnu`, `x86_64-pc-windows-msvc` and `aarch64-pc-windows-msvc` only. It prints `__WCHAR_WIDTH__` as 16 for the two Windows triples and as 32 for the four others. The type `c_wchar` takes the signedness of `wchar_t` on each target. It is signed on linux-x86_64 and both macOS targets and unsigned on the other three. A conversion or a comparison therefore gives the result of C on the same target.

A literal or constant of `c_long`, `c_ulong` or `c_wchar` fits the narrower of the two widths, so it has one value on every target. A `c_long` constant therefore lies between -2147483648 and 2147483647, and a `c_wchar` constant between 0 and 65535. Arithmetic on the three types wraps at the width on the target. A conversion with `as` moves a value between them and the sized types. The test program `tests/programs/c_types.anti` calls two functions of the C library.

```anti
extern fn labs(n: c_long) -> c_long;
extern fn wcslen(s: *c_wchar) -> c_size_t;

fn main() -> int
{
    let wide: [4]c_wchar = [72, 105, 33, 0];
    let big: c_ulong = 4294967295;
    let n = labs(-40) as int;
    return n + wcslen(&wide[0]) as int + (big / 4294967295) as int - 1;
}
```

The constant 4294967295 fits 32 bits, so `c_ulong` accepts it. `labs` returns 40 and `wcslen` counts 3 characters, so the program exits with 43.

### Booleans and characters

`bool` has the values `true` and `false`. It occupies 1 byte that holds 0 or 1, the layout that System V[^3] and the AAPCS64 standard[^5] give the C type `_Bool`.

`char` holds one Unicode scalar value. Its layout is the layout of `u32`.

### Pointers

`*T` is a pointer to a value of type `T`. A pointer holds an address. It is 8 bytes in System V[^3], the AAPCS64 standard[^5] and the Microsoft x64 ABI[^8]. The literal `null` is the pointer that points to no value. System V[^3] and the AAPCS64 standard[^5] both represent it as zero. Pointers support these operations.

- `*p` is the value that `p` points to.
- `&x` is the address of `x`, for an `x` that is a place as defined under Places.
- `p[i]` is the value `i` elements after the one `p` points to.
- `p.f` is the field `f` of the struct or union that `p` points to.
- `==` and `!=` compare two pointers of the same type.

Pointers have no arithmetic. `&p[i]` is the address `i` elements after `p`. An opaque handle is a pointer to a C type whose layout the C library does not publish, such as `FILE *`. C `void *` and opaque handles are `*byte` in Anti and convert to other pointer types with `as`.

### Arrays and slices

`[N]T` is an array of `N` values of type `T`, stored one after another. Its size is `N` times the size of `T`. As a struct field or an array element, it has the alignment of `T` in System V[^3], the AAPCS64 standard[^5] and the Microsoft x64 ABI[^8]. The length `N` is a constant expression of type `int`, as defined under Variables and constants, with a value of at least 1. C11 requires a constant array length greater than zero, and Anti keeps that rule[^11]. The type `[3][4]f32` is an array of three `[4]f32` arrays. For an array `a`, `a.len` is the constant `N` unless `N` is symbolic.

A symbolic value is a constant computed from `size_of`. Only the back end evaluates it, for its target. An array length computed from `size_of` is symbolic. It is allowed in the type of a struct field and of a local variable, and anywhere else it is a compile error. Two array types with symbolic lengths are the same type when their length expressions are the same after each named constant is replaced by its definition. The `.len` of such an array is symbolic too, and a constant expression cannot read it. The test program `tests/programs/sizes.anti` declares these items.

```anti
struct Header
{
    tag: u8,
    count: i32,
    next: *Header,
}

const HEADER: int = size_of(Header);

struct Buffer
{
    head: Header,
    pad: [HEADER - 12]byte,
}
```

Its `main` declares the local `let b: [HEADER]byte = [1; HEADER];`. The types `[HEADER]byte` and `[size_of(Header)]byte` are one type, and `[HEADER + 0]byte` is another. On all six targets `Header` is 16 bytes, so `pad` holds 4 bytes and `Buffer` is 24 bytes. The back end computes each length for its target and reports a length below 1 with the expression and the target. The test file `tests/errors/zero_length.anti` declares `[size_of(Pair) - 8]byte` for a struct `Pair` of two `i32` fields, and antic prints this line for linux-x86_64.

```text
antic: the array length `size_of(Pair) - 8` is 0 on linux-x86_64, and an array length is at least 1
```

`[]T` is a slice. A slice is 16 bytes, a pointer `ptr` of type `*T` followed by a length `len` of type `int`. It refers to `len` values stored elsewhere, in an array or in memory from `alloc`, as described under Heap memory and sizes. Copying a slice copies the pointer and the length, and the elements stay where they are. `.ptr` and `.len` are read-only.

### Strings

`str` is 16 bytes, a pointer `ptr` of type `*byte` followed by a length `len` of type `int`. Every `str` value keeps three properties.

- The `len` bytes at `ptr` are valid UTF-8 and contain no NUL byte.
- The byte at `ptr[len]` is NUL.
- A program never changes the bytes.

A C string is a pointer to bytes that end at the first zero byte. The second property makes `s.ptr` a C string. The length `s.len` counts bytes, and a non-ASCII character counts 2 to 4. The expression `s[i]` is byte `i`, of type `byte`. A slice `s[lo..hi]` has type `[]byte`, as specified under Indexing and slicing. Both `.ptr` and `.len` are read-only.

In the core language a `str` comes from a string literal or from the arguments of `main`. Building a `str` from parts belongs to the standard library module `anti.text`.

### Structs

A struct is a type made of named fields.

```anti
struct Pixel
{
    tag: u8,
    value: i32,
    flag: u8,
}
```

A struct has at least one field, and its field names are distinct. A trailing comma after the last field is allowed. A struct cannot hold a field of its own type, and it can hold a pointer to its own type. The offset of a field is its distance in bytes from the start of the struct. Four rules fix the layout of a struct without bitfields, `packed` or `align`. They are the C struct rules of System V[^3], the AAPCS64 standard[^5] and the Microsoft x64 ABI[^8].

1. Fields are placed in declaration order.
2. Each field starts at the lowest offset that is at or after the end of the previous field and a multiple of the field's alignment.
3. The alignment of the struct is the largest alignment among its fields.
4. The size of the struct is the end of its last field, rounded up to a multiple of the struct's alignment.

The bytes that the rules leave unused are padding, and their contents are unspecified[^3]. `Pixel` places `tag` at offset 0, `value` at offset 4 and `flag` at offset 8. Its alignment is 4 and its size is 12. C code reads an Anti struct without any conversion.

### Unions

A union is a type whose fields share one location in memory. It is declared like a struct, with the keyword `union`, at least one field and distinct field names.

```anti
union Value
{
    i: i64,
    f: f64,
    bytes: [8]u8,
}
```

Every field of a union starts at offset 0. The alignment of the union is the largest alignment among its fields. Its size is the size of its largest field, rounded up to a multiple of that alignment. C11 gives a union the room for its largest member and lets a pointer to the union point to each member[^11]. The union `Value` has size 8 and alignment 8 on all six targets.

A union literal names exactly one field, as in `Value { f: 1.5 }`, and it is not a constant expression. Reading a field reinterprets the bytes of the union as a value of the field's type, without a tag or a check. After `let v = Value { f: 1.5 };` the field `v.i` holds `0x3FF8000000000000`, the IEEE 754 bits of 1.5. C11 describes the same reading as a reinterpretation of the object representation[^11].

Method syntax does not apply to unions. A union passes and returns by value under the rules for structs, with every field at offset 0. [Chapter 18, Structs and arrays]({{% relref "/programming/writing-a-compiler/18-structs-and-arrays" %}}), specifies those rules for the three calling conventions.

### Bitfields

A bitfield is a field of a struct or union that occupies a given number of bits. Its declaration adds a colon and the width after the field type. The test program `tests/programs/bitfields.anti` declares this struct.

```anti
struct Flags
{
    visible: u32 : 1,
    layer: u32 : 4,
    level: i8 : 3,
    pad: u16 : 9,
}
```

The type of a bitfield is a sized integer type or an alias of one, and it fixes the signedness. The width is a constant expression of type `int` from 1 to the bits of the type. A zero-width bitfield, C's unnamed field of width 0, has the name `_` and the width 0, as `_: u32 : 0`. It holds no value and breaks the unit of the bitfields before it, so no literal or field access names it. A union holds no zero-width bitfield. A bitfield of type `bool`, `char`, `f32`, `f64`, `c_long`, `c_ulong` or `c_wchar` is a compile error. A bitfield has no address, as in C[^11], so `&` on a bitfield is a compile error.

Reading a bitfield gives a value of its type, sign-extended for a signed type and zero-extended for an unsigned one. Storing a value keeps its low bits. In `Flags`, storing 20 in `layer` leaves 4, and storing 4 in `level` leaves -4. A union may hold bitfields, and every bitfield of a union starts at bit 0.

The back end lays bitfields out by the rule of the target's C compiler. Linux and macOS follow System V, where a bitfield is contained in a storage unit of its declared type and may share it with other members[^3]. A bitfield that would cross a boundary of its type's alignment starts at that boundary, as the AAPCS64 standard defines[^5]. Windows follows MSVC, which packs adjacent bitfields into one allocation unit when their types have the same size and the next one fits[^19].

The two rules give `Flags` two layouts. Under System V, `visible`, `layer` and `level` take bits 0 to 7. The 9 bits of `pad` would cross the 16-bit boundary of `u16` at bit 16, so `pad` takes bits 16 to 24 and `Flags` is 4 bytes. Under MSVC, `visible` and `layer` share a 4-byte unit at offset 0. The field `level` has a type of another size and takes a 1-byte unit at offset 4. The field `pad` takes a 2-byte unit at offset 6, and `Flags` is 8 bytes.

The back end computes `size_of(Flags)` as 4 for the four Linux and macOS targets and as 8 for the two Windows targets. The `--dump-select` output of `return size_of(Flags);` shows the value. Apple clang 21.0.0 gives the equivalent C struct the same sizes for the six triples. A bitfield that needs more than 8 bytes from its first byte is an error from the back end. Only a packed struct can place a bitfield that way.

A zero-width bitfield follows the rule of the same C compiler. The struct `Breaks` of the ABI probe has one of type `u32` after a plain field and one of type `u64` after a bitfield.

```anti
struct Breaks
{
    a: u8,
    _: u32 : 0,
    b: u8 : 3,
    _: u64 : 0,
    c: u8 : 5,
}
```

Under System V on Linux x86_64 and macOS a zero-width bitfield moves the next field to a multiple of its type's alignment. That holds in a packed struct too. The field `b` lands at byte 4, `c` at byte 8, and `Breaks` is 9 bytes with alignment 1. AAPCS64 on Linux ARM64 also raises the struct's alignment to that of the type, so `Breaks` is 16 bytes with alignment 8. MSVC ignores a zero-width bitfield after a field that is not a bitfield. After a bitfield it closes the unit and aligns the next field and the struct to its type. That puts `b` at byte 1 and `c` at byte 8 of 16 bytes. Apple clang 21.0.0 gives the equivalent C struct the same layouts for the six triples.

### Packed and aligned types

The word `packed` before `struct` or `union` removes the padding. Every field gets alignment 1, and without `align(N)` the type has alignment 1. The modifier `align(N)` after the type name raises the alignment of the type to `N`, and the size rounds up to it. The first struct below is from the ABI probe `tests/abi/probe.anti`.

```anti
packed struct Tight
{
    a: i8,
    b: i32,
    c: i16,
}

struct Block align(16)
{
    packed: bool,
    align: i32,
}
```

`Tight` places `b` at offset 1 and `c` at offset 5, and its size is 7. `Block` has alignment 16 and size 16, and its field names use the contextual words as identifiers. Both modifiers may appear on one declaration. A packed struct with an `i8` and an `i32` field and `align(4)` has size 8.

The `N` of `align(N)` is a constant expression of type `int` with a power of two as its value. C11 requires every alignment to be a power of two[^11]. A value of `N` computed from `size_of` is a compile error. An `align(N)` below the alignment that the fields give is an error from the back end, as C11 refuses an alignment specifier weaker than the type requires[^11]. The message names the type and the target, as for this struct in the module `layout`.

```anti
struct Low align(2)
{
    a: i32,
}

fn main() -> int
{
    return size_of(Low);
}
```

```text
antic: `layout.Low` has align(2), below its alignment 4 on linux-x86_64
```

### Function pointer types

The spelling `fn(i32, i32) -> i32` stands for the address of code that takes two `i32` values and returns an `i32`. The spelling `fn(*byte)` stands for code that returns no value. A function pointer is 8 bytes in System V[^3], the AAPCS64 standard[^5] and the Microsoft x64 ABI[^8]. The name of a function, used as a value, has its function pointer type. The literal `null` is a value of every function pointer type, and `==` and `!=` compare function pointers. A variadic `extern fn` has no function pointer type.

### Type identity

Two types are the same type when they are spelled the same, after each alias is read as its sized type. The aliases are `int`, `uint`, `float`, `byte` and the eleven C types of the alias table. The types `c_long`, `c_ulong` and `c_wchar` differ from every other type. Array types with symbolic lengths compare by their length expressions. Structs and unions follow a separate rule. Every struct or union declaration introduces a new type, so two declarations with identical fields are two types.

## Literal types and conversions

### Literal types

A numeric literal takes its type from its context. These positions give a literal its context.

- the declared type in `let` or `const`
- the variable or location on the left of an assignment
- the parameter in a call
- the return type in `return`
- the field in a struct literal and the element in an array literal
- the other operand of a binary operator, an operator with two operands

Without context, an integer literal has type `int` and a float literal has type `float`. An index, the count of `alloc` and an array length give the context `int`. A literal whose value does not fit its type is a compile error. A literal of `c_long`, `c_ulong` or `c_wchar` that does not fit the narrower width of its type is a compile error too. An integer literal never takes a float type. `null` also takes its type from context, and `null` without context is a compile error.

A unary `-` directly before a numeric literal forms one constant with it. `let x: i8 = -128;` is valid, although `128` alone does not fit `i8`. Unary `-` on a value of unsigned type is a compile error.

```anti
let a: u32 = 7;
let b = a + 1; // 1 is a u32, so b is a u32
let x: i8 = -128; // one constant
let big = 5_000_000_000; // int
let half: f32 = 0.5;

let n: u8 = 300; // compile error: 300 does not fit u8
let m: u8 = -1; // compile error: -1 does not fit u8
let g: f32 = 1; // compile error: integer literal in a float context
let w: c_wchar = 65536; // compile error: does not fit c_wchar on every target
```

### Explicit conversions

Anti converts no value implicitly. NaN is the float value that marks an invalid result. The expression `x as T` converts `x` to type `T`. `as` binds tighter than `*` and looser than the unary operators, so `-x as u8` means `(-x) as u8`. The table lists every conversion, and any other conversion is a compile error.

| From | To | Result |
|---|---|---|
| integer type | integer type | The low bits of the source that fit the target. A wider target is filled with copies of the highest bit from a signed source and with zeros from an unsigned source. |
| integer type | `f32` or `f64` | The nearest value of the float type |
| `f32` or `f64` | integer type | The value rounded toward zero. NaN or a value outside the range of the target is undefined behaviour. |
| `f64` | `f32` | The nearest `f32` value |
| `f32` | `f64` | The same value |
| `bool` | integer type | 0 for `false`, 1 for `true` |
| `char` | `u32` | The scalar value |
| `u32` | `char` | The same bits, unchecked. A value that is not a Unicode scalar value is undefined behaviour. |
| `*T` | `*U` | The same address |

On x86_64 the 64-bit conversion instruction returns `0x8000000000000000` for NaN and for a value out of range[^12]. The type `bool` is not a numeric type for `as`. No conversion leads from an integer to `bool`, and `x != 0` gives that result.

For `c_long`, `c_ulong` and `c_wchar` the first row of the table uses the width of the type on the target. With `n` of type `c_long`, `n as int` sign-extends 32 bits on Windows. On Linux and macOS both widths are 64 bits, and the conversion copies the value.

```anti
let small = 300 as u8; // 44, the low 8 bits of 300
let code = 'A' as u32 + 1; // 66
let all = -1 as u64; // 18446744073709551615
let ratio = 7 as f64 / 2.0; // 3.5
```

## Expressions

### Operators and precedence

| Level | Operators | Operand types |
|---|---|---|
| 1 | call `f(x)`, index `a[i]`, slice `a[lo..hi]`, field `v.f` | as specified below |
| 2 | unary `-`, `!`, `~`, `*`, `&` | signed integer or float, `bool`, integer, pointer, place |
| 3 | `as` | as in the conversion table |
| 4 | `*` `/` `%` | numeric, and integer only for `%` |
| 5 | `+` `-` | numeric |
| 6 | `<<` `>>` | integer |
| 7 | `<` `<=` `>` `>=` | numeric or `char` |
| 8 | `==` `!=` | numeric, `bool`, `char`, pointer or function pointer |
| 9 | `&` | integer |
| 10 | `^` | integer |
| 11 | `\|` | integer |
| 12 | `&&` | `bool` |
| 13 | `\|\|` | `bool` |

Level 1 binds tightest. Binary operators group from left to right, so `a - b - c` is `(a - b) - c`. Both operands of a binary operator have the same type. Arithmetic and bitwise operators give a result of that type, and comparisons give a `bool`.

`&` binds looser than `==`. `x & 1 == 0` therefore parses as `x & (1 == 0)`, which is a compile error because `1 == 0` is a `bool`. The test for an even `x` is `(x & 1) == 0`.

### Integer arithmetic

An N-bit integer operation keeps the low N bits of the exact result. For `+`, `-` and `*` that rule applies to every width and every signedness. For `c_long`, `c_ulong` and `c_wchar`, N is the width of the type on the target. In `u8`, `250 + 10` is `4`. In `i8`, `127 + 1` is `-128`, and negating an `i8` variable that holds `-128` gives `-128`.

`/` rounds the quotient toward zero, and `%` takes the sign of the dividend. `-7 / 2` is `-3` and `-7 % 2` is `-1`. For every `a` and `b` where `a / b` is defined, `(a / b) * b + a % b` equals `a`. Division by zero is undefined behaviour. Dividing the minimum value of a signed type by `-1` is undefined behaviour too, as in `-128 / -1` for `i8`. On x86_64 the signed division instruction raises a divide error in both cases[^12].

The operators `&`, `|` and `^` combine bits, and `~` inverts every bit. The shift `a << n` moves bits left and fills the vacated bits with zeros. The shift `a >> n` moves bits right. It fills the vacated bits with copies of the highest bit when `a` is signed, and with zeros when `a` is unsigned. A shift count that is negative, or at least the bit width of `a`, is undefined behaviour. On x86_64 the processor masks the count to 5 bits, or 6 bits for a 64-bit operand[^12]. An `i32` shifted by 32 therefore stays unchanged there.

### Float arithmetic

The operators `+`, `-`, `*` and `/` on `f32` and `f64` compute in the IEEE 754 format of the type. The compiler emits a multiplication and a following addition as two instructions and never fuses them into one. Both x86_64 and ARM64 therefore compute the same bits for the same float expression.

### Comparison and logic

`==` and `!=` compare two values of the same numeric type, `bool`, `char`, pointer type or function pointer type. `<`, `<=`, `>` and `>=` compare numeric values, and `char` values by scalar value. Structs, unions, arrays, slices and `str` have no comparison operators.

`&&`, `||` and `!` take `bool` operands. `a && b` evaluates `b` only when `a` is `true`. `a || b` evaluates `b` only when `a` is `false`. `!a` is the opposite value.

### Evaluation order

The operands of an operator and the arguments of a call are evaluated from left to right. In `f(g(), h())`, `g` runs before `h`. In `g() + h()`, `g` also runs before `h`. An assignment evaluates its place before its value, and a compound assignment does the same. In `a[i()] = j();`, `i` runs before `j`.

### Places

A place is an expression that denotes a location in memory. These expressions are places.

- a local variable, declared with `let` inside a function, or a parameter
- `*p` for a pointer `p`
- `p[i]` for a pointer `p`, and `s[i]` for a slice `s`
- `a[i]` for an array `a` that is a place
- `e.f` for a struct or union `e` that is a place, and `p.f` for a pointer `p` to a struct or union

Constants, literals, call results, `.len`, `.ptr` and the bytes of a `str` are not places. `&` applies to places only and gives a pointer to the location. A bitfield is a place without an address, so `&` does not apply to it. The left side of an assignment is a place.

### Indexing and slicing

The expression `a[i]` selects element `i`, counted from 0, of an array, a slice, a pointer or a `str`. The index has type `int`. An element of a `str` has type `byte`, and an element of the other kinds has the element type. No bounds check happens, and an index outside the elements is undefined behaviour.

`a[lo..hi]` is the slice of elements `lo` up to and excluding `hi`, with length `hi - lo`. Both bounds are required and have type `int`. It applies to an array that is a place, where the slice points into the array, and to a slice. The result has type `[]T`. On a `str` the result is a `[]byte`, because a byte slice has no rule about a trailing NUL. Bounds that violate `0 <= lo <= hi <= n`, where `n` is the number of elements, are undefined behaviour. A pointer has no slice syntax, and a slice literal builds a slice from a pointer.

### Struct, union, array and slice literals

`Pixel { tag: 1, value: -5, flag: 0 }` is a struct literal. It names every field of the struct exactly once, in any order. A union literal, such as `Value { f: 1.5 }`, names exactly one field of the union. A qualified name is the local name of an imported module, a dot and a name from that module. It writes a struct from another module, as in `geometry.Vec2 { x: 1.0, y: 2.0 }`.

`[1, 2, 3]` is an array literal of type `[3]T`, where `T` comes from the context and is `int` without one. `[e; N]` is an array of `N` copies of `e`. It evaluates `e` once. `[]T { ptr: p, len: n }` is a slice literal, which builds a `[]T` from a pointer `p` of type `*T` and a length `n` of type `int`.

A trailing comma is allowed after the last element of every comma-separated list. That covers struct declarations, struct and slice literals, array literals, parameter lists, argument lists and the parameter types of a function pointer type. A condition, as in `if` and `while`, is followed by a body in braces. Inside a condition, a struct or slice literal therefore appears only within parentheses.

### Heap memory and sizes

`alloc(T, n)` reserves space for `n` values of type `T` and returns a `*T` to the first of them. The space comes from the heap, the region that a program reserves and releases explicitly, independent of function calls. The function `alloc` obtains it through the C function `malloc`. The space is uninitialised, so its contents are unspecified until the program writes them. When the reservation fails, `alloc` returns `null`. The C function `malloc` returns memory aligned for every C object type, and therefore for every Anti type[^11].

`free(p)` releases memory that `alloc` returned, through the C function `free`. The expression `size_of(T)` is the size of `T` in bytes on the target, as an `int`. The size differs between targets for the types that the section on types and layout names. A value computed from `size_of` is symbolic, and in a constant expression it converts only to an integer type. The three names `alloc`, `free` and `size_of` are keywords. Both `alloc` and `size_of` take a type as their first argument.

### Calls and method syntax

A call `f(a, b)` evaluates the arguments and passes copies of them to `f`. Each argument has the type of its parameter. `f` is a function name, a qualified name such as `geometry.length`, or an expression of a function pointer type.

Method syntax writes the first argument before the function name. For a struct type `T` without a field named `f`, antic finds the function `f` in the module that declares `T` and rewrites the call.

| Receiver | First parameter of `f` | Call |
|---|---|---|
| `v` of type `T` | `*T` | `f(&v, args)` |
| `v` of type `T` | `T` | `f(v, args)` |
| `p` of type `*T` | `*T` | `f(p, args)` |
| `p` of type `*T` | `T` | `f(*p, args)` |

In the first row `v` is a place, because the rewrite takes its address. A call from another module requires `f` to be `pub`. When `T` has a field named `f`, `v.f(args)` calls the function pointer stored in that field. On a union, `v.f(args)` always calls the function pointer in the field `f`. Anti has no overloading, so a name in a module denotes one function.

```anti
struct Shape
{
    area: fn(*Shape) -> f64,
    size: f64,
}

fn square_area(s: *Shape) -> f64
{
    return s.size * s.size;
}

fn scale(s: *Shape, k: f64)
{
    s.size *= k;
}

fn demo() -> f64
{
    let sq = Shape { area: square_area, size: 2.0 };
    sq.scale(1.5); // scale(&sq, 1.5)
    return sq.area(&sq); // calls the field, returns 9.0
}
```

A struct of function pointers, such as `Shape`, lets one piece of code call different functions for different values. Anti has no classes and no inheritance.

## Statements

A statement performs an action and computes no value.

### Variables and constants

`let name: T = e;` declares a local variable of type `T` with the initial value `e`. The type may be left out, and the variable then has the type of `e`. Every variable has an initial value. Local variables and parameters are mutable.

`const NAME: T = e;` declares a constant at module level or in a block. Its initialiser is a constant expression, which antic evaluates at compile time. A constant expression is built from these parts.

- literals, other constants and `null`
- every binary operator, including `&&` and `||`, and the unary operators `-`, `!` and `~`
- conversions with `as`
- struct literals and array literals
- `size_of(T)`
- a field of a constant struct, as in `C.x`
- `.len` of a constant array or of a string literal
- an element of a constant array at a constant index, as in `C[2]`

Comparisons give `bool` constants. The only constant pointer is `null`, and a comparison with `null` through `==` or `!=` is a constant expression. Calls, `alloc`, `&`, `*`, `.ptr`, slicing and indexing anything other than a constant array are excluded. A union literal is excluded too, because a constant holds a value for every field. A constant has no address, so `&NAME` is a compile error. Module level holds no variables, so Anti has no mutable global state.

```anti
const WIDTH: int = 320;
const HEIGHT: int = WIDTH * 3 / 4; // 240
const SCREEN: [2]int = [WIDTH, HEIGHT];
const PIXELS: int = SCREEN[0] * SCREEN[1]; // 76800
const WIDE: bool = WIDTH > HEIGHT && HEIGHT >= 200;
const PIXEL_BYTES: int = size_of(Pixel) * 2;
```

The constant `PIXEL_BYTES` is symbolic, and the back end computes it as 24 on every target.

### Scope and shadowing

A block `{ }` opens a scope. A name declared with `let` or `const` is visible from the end of its declaration to the end of its block. A `let` in an inner block may reuse the name of an outer local, a parameter or a module-level item. The body of a function counts as an inner block relative to its parameters. The new name hides the old one until the inner block ends. Declaring a name twice in the same block is a compile error. Module-level items are visible in the whole module, before and after their declaration.

```anti
fn shadow(x: int) -> int
{
    let total = 0;
    {
        let x = x + 1; // the initialiser reads the parameter x
        total += x; // reads the inner x
    }
    total += x; // the parameter x again
    return total;
}
```

The initialiser of a `let` sees the outer name, because the new name becomes visible after its statement. `shadow(1)` returns 3.

### Assignment

`place = e;` stores `e` in the place. `place op= e;`, where `op` is one of `+ - * / % & | ^ << >>`, stores `place op e` and evaluates the place once. Assignments are statements, so `a = b = c;` and `if x = 1 { }` are syntax errors. Anti has no `++`, `--`, conditional operator `?:` or comma operator.

### Expression statements and blocks

A call followed by `;` is a statement, and its result is discarded. Any other expression followed by `;` is a compile error. A block is a statement. Blocks contain statements, including `let` and `const`. `fn`, `extern fn`, `struct`, `union` and `import` occur at module level only.

### Conditionals

```anti
if n < 0 {
    sign = -1;
} else if n == 0 {
    sign = 0;
} else {
    sign = 1;
}
```

A condition has type `bool`. Parentheses around a condition are optional. Every branch is a block in braces.

### Loops

```anti
let i = 0;
while i < 10 do {
    i += 1;
    if i % 2 == 0 {
        continue;
    }
    printf("%lld\n".ptr, i);
}

do {
    i -= 1;
} while i > 0
```

A `while cond do { }` loop tests the condition before each pass, so its body runs zero or more times. A `do { } while cond` loop tests after each pass, so its body runs at least once. No semicolon follows the condition of `do`. Inside a loop, `break` leaves the innermost loop and `continue` proceeds to the next test of the condition. Either statement outside a loop is a compile error. The core language has no `for` loop.

### Return

The statement `return e;` ends a function declared with `-> R` and returns `e`, which has type `R`. A function without a result ends with `return;` or at its closing brace.

A function with `-> R` that can reach the end of its body is a compile error. antic decides this from the form of the body. The body ends in `return` when its last statement is a `return`. It also ends in `return` when its last statement is an `if` chain with a final `else`, where every branch ends in `return` by the same rule. A loop never counts, `while true` included. Statements after a `return` are allowed and produce no diagnostic.

```anti
fn sign_of(n: int) -> int
{
    if n < 0 {
        return -1;
    } else if n == 0 {
        return 0;
    } else {
        return 1;
    }
}

fn first_even(a: []int) -> int
{
    let i = 0;
    while true do {
        if a[i] % 2 == 0 {
            return a[i];
        }
        i += 1;
    }
    return -1; // required, because a loop never counts
}
```

## Functions

### Function declarations

`fn name(p: T, q: U) -> R { }` declares a function with parameters `p` and `q` and a result of type `R`. Without `-> R` the function returns no value. Parameters are local variables that hold copies of the arguments. Arguments and results pass by value, structs and arrays included, following the ABI of the target. Functions are declared at module level. Anti has no function overloading. Structs have no constructors and no destructors.

### External functions

```anti
extern fn puts(s: *byte) -> i32;
extern fn printf(fmt: *byte, ...) -> i32;
```

An `extern fn` declaration introduces a function written in C. The linker, the tool that combines compiled files into one executable, adds its code to the program. Calls to it follow the calling convention of the target, the part of the ABI that passes arguments and results. Its declared name is its symbol, the name under which the linker finds the function, without any change.

A variadic function takes a variable number of arguments. `...` after at least one parameter declares one. A variadic argument has type `i32`, `u32`, `i64`, `u64`, `f64` or a pointer type. Every other type is converted first, as in `x as f64` for an `f32` value or `n as int` for a `c_long` value. A struct passed to an `extern fn` follows the rules of the target's ABI.

### Program entry

A program has one `main` function with one of three signatures.

- `fn main() -> int`
- `fn main(args: []str) -> int`
- `fn main(args: []str, env: []str) -> int`

The runtime library `anti_rt` is C code that antic links into every program. Its file is `libanti_rt.a` on Linux and macOS and `anti_rt.lib` on Windows. The element `args[0]` is the program name as the operating system passed it, and the other elements are the command-line arguments. The slice `env` holds the environment variables as `NAME=value` entries, in the order the operating system gives them.

Both slices hold valid `str` values. The runtime converts the arguments and the environment to UTF-8 text. It replaces each invalid sequence with the replacement character `0xFFFD` and ends each entry with a NUL byte[^2]. Windows represents Unicode text as UTF-16, which encodes each character as one or two 16-bit units[^13]. On Windows the runtime reads the command line through `GetCommandLineW`[^14] and the environment through `GetEnvironmentStringsW`[^15].

The conversion is in `rt/start.c`, a C file in `anti_rt`. That file defines the C function `main`. It calls the Anti `main` through `anti.rt.main`, a global symbol that antic defines as a second name for the mangled `main` of the main module. The core language has no map type, and a lookup in `env` is a loop.

```anti
fn find(env: []str, prefix: []byte) -> []byte
{
    let i = 0;
    while i < env.len do {
        let e = env[i];
        if e.len >= prefix.len {
            let j = 0;
            while j < prefix.len && e[j] == prefix[j] do {
                j += 1;
            }
            if j == prefix.len {
                return e[prefix.len..e.len];
            }
        }
        i += 1;
    }
    return []byte { ptr: null, len: 0 };
}
```

The call `find(env, b"HOME=")` returns the value of `HOME`, or an empty slice. The result of `main` becomes the exit code of the process. On Linux and macOS, a parent process that waits with the POSIX functions `wait` or `waitpid` receives the low 8 bits of that code[^16]. A `main` that returns -1 therefore exits with 255 there. On Windows, `ExitProcess` takes the exit code as a `UINT`[^17], an unsigned type[^18] with the values 0 to 4294967295. The process keeps the low 32 bits of the result.

## Modules

### Files, imports and visibility

```anti
import geometry;
import geometry as g;

fn perimeter(r: geometry.Rect) -> f32
{
    return 2.0 * (r.w + r.h);
}

fn unit() -> g.Rect
{
    return g.Rect { w: 1.0, h: 1.0 };
}
```

A module is identified by its module path, a sequence of lowercase ASCII identifiers separated by dots. The path mirrors a directory tree under the search roots, the directories that the `-I <dir>` options of antic name, in order. The declaration `import anti.collection.list;` resolves to the source file `anti/collection/list.anti` or to the library file `anti/collection/list.antl` under a root. The last segment of the path is the local name, so the `pub` items of that module are visible as `list.name`. With `import anti.collection.list as l;` they are visible as `l.name`. Imports come first in a file.

The compiler derives the module path of its input from the source path under the first root that holds it. Slashes become dots and the suffix `.anti` is dropped, so `antic -I src src/com/example/geo.anti` compiles the module `com.example.geo`. A source file outside every root is the module of its file name alone. A keyword is never a segment, so a source file named `union.anti` is a compile error. For an import that no library file on the command line provides, antic uses the library file under the first root that has it.

A path of one segment names a file of the program itself. The source file `geometry.anti` is the module `geometry`, which `import geometry;` imports. Such a module is not for publishing, and `antic -c` warns when it compiles one into a library file. A module is named after its contents, not after a type that it declares.

Every path that starts with `anti.` belongs to the language's own libraries, such as `anti.collection.list` and the runtime `anti.rt`. The modules `anti.io`, `anti.net`, `anti.regex`, `anti.text` and `anti.license` belong to the standard library. The binding of a bundled C library is named after the library, as `anti.raylib` for raylib and `anti.miniaudio` for miniaudio. The command `antic -c` refuses to write a library named `anti` or under `anti.` without the flag `--anti-internal`. No compilation may define the module `anti.rt`, the module of the runtime's entry symbol `anti.rt.main`. A third-party library uses a root that its author owns, in reverse-domain form, such as `com.niese.anti.web`.

Items are private to their module unless declared `pub`. `pub` applies to `fn`, `extern fn`, `struct`, `union` and `const`. The fields of a struct are visible wherever the struct is visible. Within a module each name is unique.

An import cycle is a compile error. The command `antic -c geometry.anti` compiles a module into the library file `geometry.antl`. That file holds the public interface of the module and its code in the intermediate representation of antic, the program form between parsing and code generation. Each library file is built against the interfaces of the modules it imports, so the imports cannot form a cycle.

### Export

The keyword `export` before `fn`, `struct`, `union` or `const` makes an item available to C code. An exported item is also `pub`, and `export` and `pub` never appear on one item. The test library `tests/clib/com/example/geo.anti` holds these two items among others.

```anti
/// A point on the plane.
export struct Vec2
{
    x: c_int,
    y: c_int,
}

/// The dot product of a and b.
export fn geo_dot(a: Vec2, b: Vec2) -> c_int
{
    return a.x * b.x + a.y * b.y;
}
```

An `export fn` has its own name as its symbol, the rule of `extern fn`, and the symbol is global. With `-I tests/clib` for linux-x86_64 the symbol of `geo_dot` is `geo_dot`. The private function `square` of the same file has the symbol `com.example.geo.square`. Two modules may not export one name, and the second export of a name is a compile error.

The parameters and the result of an `export fn` have types with a C representation. Those are the numeric types, `bool`, pointers to such types, exported structs and unions, and function pointers whose types follow the same rule. A parameter or result of type `char` or `str` is a compile error. So is one of a slice type, an array type or a struct type that is not exported.

The fields of an `export struct` or `export union` follow the same rule, and a field may also be a fixed-size array of such a type. An `export const` has a numeric type, `bool` or `str`. The function `main` cannot be exported, because the C `main` of the runtime starts the program. The whole-program optimizer keeps every `export fn`, including one that no Anti code calls, because C code may call it. Chapter 25 writes a C header and a static or shared library from the exported items.

### Symbol names

Mangling derives the symbol of a function from its full module path and its name. On Linux and macOS the symbol of `push` in module `anti.collection.list` is `anti.collection.list.push`, and the symbol of `length` in module `geometry` is `geometry.length`. A Windows symbol holds only letters, digits and `_`. It starts with `_A`, puts the length of each segment before the segment and ends with `_` and the function name. The two symbols become `_A4anti10collection4list_push` and `_A8geometry_length`.

The names of `extern fn` and `export fn` declarations are never mangled. The function `main` is mangled like every other function. Its symbol gets the second name `anti.rt.main`, written `_A4anti2rt_main` on Windows, and the C `main` in `rt/start.c` calls that name.

## Memory model

### Stack frames and blocks

A call reserves a stack frame, the memory that holds the parameters and locals of the function, and the return releases it. A local exists from its declaration to the end of its block. A local whose address is taken with `&` has a location in the stack frame. Every other local may be kept in a processor register, a storage cell inside the processor. A pointer to a local is invalid after the block of the local ends.

### Values and copies

Structs, unions and arrays are values. Assignment, argument passing and `return` copy all of their bytes. Copying a slice or a `str` copies the pointer and the length, and the elements are shared.

### Heap lifetime

Memory from `alloc` stays reserved until the program passes its address to `free`. Anti has no reference counting and no garbage collector. The program alone decides when heap memory is released.

### Undefined behaviour

Anti performs no run-time checks. These conditions are undefined behaviour.

- an index or a slice bound outside the elements
- reading or writing through `null`, through a released pointer, or through a pointer to a local whose block has ended
- releasing memory twice, or releasing memory that `alloc` did not return
- writing to the bytes of a `str` or of a string literal
- integer division by zero, and the minimum value of a signed type divided by `-1`
- a shift count that is negative or at least the bit width
- converting NaN or an out-of-range float value to an integer type
- converting a `u32` that is not a Unicode scalar value to `char`

C11 leaves the integer division, the shift and the float conversion of this list undefined as well[^11]. None of the three has a value that antic could compute at compile time. When all operands are constants, each is therefore a compile error, in a constant and in a function body alike. The message names the operation and its values, as in `` `7 / 0` divides by zero ``. With a variable operand the case stays undefined behaviour at run time.

### Threads

Chapter 22 adds threads to the language. In the core language, memory that two threads share has no guarantees.

## Syntax grammar

The EBNF below defines the core language above the level of tokens. The rules `ident`, `int_lit`, `float_lit`, `char_lit` and `string_lit` are in the section on source text.

```ebnf
module         = { import } { item } ;
import         = "import" module_path [ "as" ident ] ";" ;
module_path    = ident { "." ident } ;
item           = [ "pub" ] ( function | extern_fn | struct_decl | union_decl
               | const_decl )
               | "export" ( function | struct_decl | union_decl | const_decl ) ;

function       = "fn" ident "(" [ params ] ")" [ "->" type ] block ;
extern_fn      = "extern" "fn" ident "(" [ params | variadic ] ")" [ "->" type ] ";" ;
params         = param { "," param } [ "," ] ;
variadic       = param { "," param } "," "..." [ "," ] ;
param          = ident ":" type ;
struct_decl    = [ "packed" ] "struct" ident [ alignment ] fields ;
union_decl     = [ "packed" ] "union" ident [ alignment ] fields ;
alignment      = "align" "(" expr ")" ;
fields         = "{" field { "," field } [ "," ] "}" ;
field          = ident ":" type [ ":" expr ] ;
const_decl     = "const" ident ":" type "=" expr ";" ;

type           = builtin_type | ident [ "." ident ] | "[" expr "]" type
               | "[" "]" type | "*" type | fn_type ;
fn_type        = "fn" "(" [ type { "," type } [ "," ] ] ")" [ "->" type ] ;
builtin_type   = "int" | "uint" | "float" | "bool" | "char" | "byte" | "str"
               | "i8" | "i16" | "i32" | "i64" | "u8" | "u16" | "u32" | "u64"
               | "f32" | "f64" | c_type ;
c_type         = "c_char" | "c_uchar" | "c_short" | "c_ushort" | "c_int"
               | "c_uint" | "c_long" | "c_ulong" | "c_longlong"
               | "c_ulonglong" | "c_size_t" | "c_float" | "c_double"
               | "c_wchar" ;

block          = "{" { statement } "}" ;
statement      = let_stmt | const_decl | simple_stmt | if_stmt | while_stmt
               | do_stmt | "break" ";" | "continue" ";" | return_stmt | block ;
let_stmt       = "let" ident [ ":" type ] "=" expr ";" ;
simple_stmt    = expr [ assign_op expr ] ";" ;
assign_op      = "=" | "+=" | "-=" | "*=" | "/=" | "%=" | "&=" | "|=" | "^="
               | "<<=" | ">>=" ;
if_stmt        = "if" cond block { "else" "if" cond block } [ "else" block ] ;
while_stmt     = "while" cond "do" block ;
do_stmt        = "do" block "while" cond ;
return_stmt    = "return" [ expr ] ";" ;
cond           = expr ;

expr           = or_expr ;
or_expr        = and_expr { "||" and_expr } ;
and_expr       = bit_or { "&&" bit_or } ;
bit_or         = bit_xor { "|" bit_xor } ;
bit_xor        = bit_and { "^" bit_and } ;
bit_and        = equality { "&" equality } ;
equality       = relation { ( "==" | "!=" ) relation } ;
relation       = shift { ( "<" | "<=" | ">" | ">=" ) shift } ;
shift          = additive { ( "<<" | ">>" ) additive } ;
additive       = multiplicative { ( "+" | "-" ) multiplicative } ;
multiplicative = cast { ( "*" | "/" | "%" ) cast } ;
cast           = unary { "as" type } ;
unary          = ( "-" | "!" | "~" | "*" | "&" ) unary | postfix ;
postfix        = primary { "(" [ args ] ")" | "[" expr [ ".." expr ] "]" | "." ident } ;
args           = expr { "," expr } [ "," ] ;
primary        = int_lit | float_lit | char_lit | string_lit | "true" | "false"
               | "null" | ident | struct_lit | slice_lit | array_lit
               | builtin_call | "(" expr ")" ;
struct_lit     = ident [ "." ident ] "{" field_init { "," field_init } [ "," ] "}" ;
slice_lit      = "[" "]" type "{" field_init "," field_init [ "," ] "}" ;
field_init     = ident ":" expr ;
array_lit      = "[" expr ( { "," expr } [ "," ] | ";" expr ) "]" ;
builtin_call   = "alloc" "(" type "," expr [ "," ] ")"
               | "free" "(" expr [ "," ] ")"
               | "size_of" "(" type [ "," ] ")" ;
```

Rules outside the notation complete the grammar.

- Outside a `cond`, an identifier or a qualified name followed by `{` starts a `struct_lit`. Inside a `cond`, a `struct_lit` or `slice_lit` occurs only within parentheses.
- A `simple_stmt` without `assign_op` is a call. With `assign_op`, its left expression is a place.
- The fields of a `slice_lit` are `ptr` and `len`, each once, in either order.
- A `variadic` parameter list occurs only in an `extern_fn`.
- Each `ident` of a `module_path` holds only lowercase ASCII letters, digits and `_`.
- The words `packed` and `align` in `struct_decl`, `union_decl` and `alignment` are the contextual words. Every other `ident` may spell either word.
- The `expr` after the type of a `field` is the width of a bitfield.
- Doc comments are outside the grammar. A `///` or `//#` comment belongs to the `item` or `field` after it. The `//!` and `//#!` comments before the first `import` or `item` belong to the module.

## Platform facts

The language relies on these facts about its six targets.

| Fact | Where it applies | Sources |
|---|---|---|
| Data pointers and function pointers are 8 bytes. | `*T`, `str`, slices, function pointer types | System V[^3], the AAPCS64 standard[^5], Microsoft x64 ABI[^8] |
| Each integer, float and pointer type has an alignment equal to its size. | numeric types, struct layout | System V[^3], the AAPCS64 standard[^5], Microsoft x64 ABI[^8] |
| Structs are laid out in declaration order, with aligned fields and a size that is a multiple of the largest alignment. | struct layout | System V[^3], the AAPCS64 standard[^5], Microsoft x64 ABI[^8] |
| A union has room for its largest member, and a pointer to the union points to each member. | union layout | the C11 draft[^11] |
| Bitfields lie in a storage unit of their declared type and may share it with other members. | bitfields on Linux and macOS | System V[^3], the AAPCS64 standard[^5] |
| Adjacent bitfields share an allocation unit when their types have the same size and the next one fits. | bitfields on Windows | Microsoft structure padding and alignment[^19] |
| An alignment is a power of two, and an alignment specifier cannot weaken the alignment of its type. | `align(N)` | the C11 draft[^11] |
| C `_Bool` is 1 byte holding 0 or 1. The Microsoft compiler gives `bool` 1 byte. | `bool` | System V[^3], the AAPCS64 standard[^5], Microsoft data type ranges[^9] |
| The null pointer is zero. | `null` | System V[^3], the AAPCS64 standard[^5] |
| C `float` and `double` are IEEE 754 binary32 and binary64. | `f32`, `f64` | System V[^3], the AAPCS64 standard[^5] |
| C `long` is 4 bytes on Windows and 8 bytes on Linux and macOS. | `c_long`, `c_ulong` | Microsoft data type ranges[^9], System V[^3], the AAPCS64 standard[^5], Apple ARM64 notes[^7] |
| C `wchar_t` is 16 bits on Windows and 32 bits on Linux and macOS. | `c_wchar` | Microsoft data type ranges[^9], the AAPCS64 standard[^5], Apple clang 21.0.0 |
| `malloc` returns memory aligned for every object type. | `alloc` | the C11 draft[^11] |
| `wait` and `waitpid` report the low 8 bits of an exit status. | exit code on Linux and macOS | POSIX `exit`[^16] |
| `ExitProcess` takes a 32-bit unsigned exit code. | exit code on Windows | `ExitProcess`[^17], Windows data types[^18] |
| The Windows command line and environment are UTF-16. | `args` and `env` on Windows | Working with Strings[^13], `GetCommandLineW`[^14], `GetEnvironmentStringsW`[^15] |
| x86_64 signed division raises a divide error on a zero divisor and on overflow. | integer division | x86 reference, IDIV[^12] |
| x86_64 masks a shift count to 5 or 6 bits. | shift operators | x86 reference, SAL and SAR[^12] |
| x86_64 float to integer conversion returns `0x8000000000000000` out of range. | `as` from float to integer | x86 reference, CVTTSD2SI[^12] |

## Next

[Chapter 3, Setup and the first executable]({{% relref "/programming/writing-a-compiler/03-setup-and-first-executable" %}}), lists what to install on Linux, macOS and Windows, with the assembler llvm-mc, the linker lld and the archiver llvm-ar. It sets up the repository layout and the CMake build and runs llvm-mc and the platform linker by hand. A direct code path in antic then compiles a program that returns an exit code for the host machine. The toolchain runs before the compiler pipeline exists.

## References

[^1]: F. Yergeau, *UTF-8, a transformation format of ISO 10646*, RFC 3629, 2003, sections 3 and 6, https://www.rfc-editor.org/rfc/rfc3629

[^2]: Unicode Consortium, *Glossary of Unicode Terms*, entries "Code Point", "Unicode Scalar Value" and "Replacement Character", https://www.unicode.org/glossary/

[^3]: Edited by H.J. Lu and five others, *System V Application Binary Interface, AMD64 Architecture Processor Supplement*, version 1.0, https://gitlab.com/x86-psABIs/x86-64-ABI

[^4]: Apple, *OS X ABI Function Call Guide*, "x86-64 Function Calling Conventions", https://developer.apple.com/library/archive/documentation/DeveloperTools/Conceptual/LowLevelABI/140-x86-64_Function_Calling_Conventions/x86_64.html

[^5]: Arm Limited, *Procedure Call Standard for the Arm 64-bit Architecture*, release 2025Q4, https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst

[^6]: Microsoft, *Overview of ARM64 ABI conventions*, 2025, https://learn.microsoft.com/en-us/cpp/build/arm64-windows-abi-conventions?view=msvc-170

[^7]: Apple, *Writing ARM64 code for Apple platforms*, https://developer.apple.com/documentation/xcode/writing-arm64-code-for-apple-platforms

[^8]: Microsoft, *Overview of x64 ABI conventions*, 2025, section "x64 type and storage layout", https://learn.microsoft.com/en-us/cpp/build/x64-software-conventions?view=msvc-170

[^9]: Microsoft, *Data Type Ranges*, 2020, https://learn.microsoft.com/en-us/cpp/cpp/data-type-ranges?view=msvc-170

[^10]: raylib, `src/raylib.h` at commit c172619, line 1154, https://github.com/raysan5/raylib/blob/c172619cdad216b9d496b01debd0c722cffab08e/src/raylib.h#L1154

[^11]: ISO/IEC JTC1/SC22/WG14, *N1570, Committee Draft of ISO/IEC 9899:201x*, 2011, sections 6.2.8, 6.3.1.4, 6.5.2.3, 6.5.3.2, 6.5.5, 6.5.7, 6.7.2.1, 6.7.5, 6.7.6.2 and 7.22.3, https://www.open-std.org/jtc1/sc22/wg14/www/docs/n1570.pdf

[^12]: F. Cloutier, *x86 and amd64 instruction reference*, pages CVTTSD2SI, IDIV and SAL/SAR/SHL/SHR, from the Intel Software Developer's Manual, https://www.felixcloutier.com/x86/

[^13]: Microsoft, *Working with Strings*, Win32 documentation, https://learn.microsoft.com/en-us/windows/win32/learnwin32/working-with-strings

[^14]: Microsoft, *GetCommandLineW function (processenv.h)*, 2018, https://learn.microsoft.com/en-us/windows/win32/api/processenv/nf-processenv-getcommandlinew

[^15]: Microsoft, *GetEnvironmentStringsW function (processenv.h)*, 2022, https://learn.microsoft.com/en-us/windows/win32/api/processenv/nf-processenv-getenvironmentstringsw

[^16]: The Open Group, *The Open Group Base Specifications Issue 8*, IEEE Std 1003.1-2024, `exit`, https://pubs.opengroup.org/onlinepubs/9799919799/functions/exit.html

[^17]: Microsoft, *ExitProcess function (processthreadsapi.h)*, 2018, https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-exitprocess

[^18]: Microsoft, *Windows Data Types*, 2024, entry `UINT`, https://learn.microsoft.com/en-us/windows/win32/winprog/windows-data-types

[^19]: Microsoft, *Padding and Alignment of Structure Members*, 2018, https://learn.microsoft.com/en-us/cpp/c-language/padding-and-alignment-of-structure-members?view=msvc-170
