---
title: "The parts of a compiler"
description: "The stages of a compiler from source text to executable, followed through one Anti function, and the point where llvm-mc and the linker take over."
summary: "The pipeline from source text to executable. Lexer, parser, semantic analysis, intermediate representation, optimizer, instruction selection, register allocation, assembly emission, then the assembler and linker. States the scope. antic stops at assembly text. llvm-mc and lld do the rest, on six targets."
date: 2026-09-13T22:32:00+02:00
lastmod: 2026-09-15T03:47:03+02:00
draft: false
weight: 10
tags: [compilers, assembly]
keywords: [compiler pipeline, lexer, parser, syntax tree, intermediate representation, instruction selection, register allocation, llvm-mc]
---

A compiler translates a program written as text into an executable, a file that the operating system runs. The compiler this book builds, antic, works in seven stages, and two existing tools turn its output into the executable. This chapter follows one function of the Anti language through every stage and shows what each stage produces. The book repository holds the finished compiler, and the chapters walk through it in the order it was built. Anti is a recursive acronym, "Anti's Not Too Impressive". The output of antic is assembly text. The assembler llvm-mc and the linker lld finish the work on six targets, the combinations of Linux, macOS and Windows with the x86_64 and ARM64 processors.

## Compiler pipeline

<!-- requires site shortcode: figsvg -->
{{< figsvg src="pipeline.svg" alt="Seven boxes stacked from top to bottom on the antic side of a dashed vertical line: Lexer, Parser, Semantic analysis, Optimizer, Instruction selection, Register allocation and Assembly emission. The arrows between them carry source text, tokens, syntax tree, IR, IR, target instructions and target instructions. Braces on the left group Lexer, Parser and Semantic analysis as the front end, Optimizer as the middle end, and the last three stages as the back end. An arrow labelled assembly text crosses the dashed line to a box llvm-mc, whose object file arrow leads to a box Linker, whose arrow carries the executable." caption="The stages of antic on the left of the dashed line, grouped into front end, middle end and back end, and the external tools on the right. Each arrow names what one stage hands to the next. The right-hand boxes are the tools that antic calls." >}}

The figure is drawn from [pipeline.tex in the book repository](https://github.com/FoundingFuture/book-writing-a-compiler/blob/main/writing-a-compiler/01-parts-of-a-compiler/pipeline.tex). A stage reads one artefact and writes the next. The source text is the program as characters in files. The seven stages of antic turn the source text into assembly text, a readable form of the processor's instructions. The assembler llvm-mc turns assembly text into an object file, and the linker combines object files into the executable.

The stages fall into three groups. The front end, made of the lexer, the parser and semantic analysis, depends on the language and on no processor. The middle end, the intermediate representation and the optimizer, depends on neither. The back end, from instruction selection to assembly emission, depends on the processor and the operating system. The compiler has one front end, one middle end and two back ends, one for x86_64 and one for ARM64.

## Example program

```anti
fn scale(x: int) -> int
{
    let k = 2 + 4;
    return x * k;
}

fn main() -> int
{
    return scale(7);
}
```

The file `main.anti` holds two functions. The function `scale` multiplies its argument by 6. The function `main` returns `scale(7)`. The exit code of a process is the number it hands to the operating system when it ends, and the result of `main` becomes that number, here 42. The sections below trace `scale` through the stages. The last sections show the output for the whole file on macOS ARM64.

## Lexical analysis

The lexer reads the source text one character at a time and groups the characters into tokens. A token is the smallest unit of the syntax, such as a keyword, a name, a number or a symbol. Each token records its kind, its text and its position as line and column, so that an error message can point at the source. Spaces, line breaks and ordinary comments separate tokens and produce none. A doc comment, such as a line that starts with `///`, documents the item after it and becomes a token of the kind `doc`.

The command `antic --dump-tokens main.anti` prints the tokens, and the listing shows the 23 tokens of `scale`. Each kind is named after the rule of chapter 2 that matches it: `keyword`, `ident` for an identifier, `int_lit` for an integer literal and `symbol` for operators and punctuation. The file indents each statement with one tab, which the listing of the program shows as four spaces. The lexer counts the tab as one column, so `let` starts at column 2.

```text
1:1   keyword  fn
1:4   ident    scale
1:9   symbol   (
1:10  ident    x
1:11  symbol   :
1:13  keyword  int
1:16  symbol   )
1:18  symbol   ->
1:21  keyword  int
2:1   symbol   {
3:2   keyword  let
3:6   ident    k
3:8   symbol   =
3:10  int_lit  2
3:12  symbol   +
3:14  int_lit  4
3:15  symbol   ;
4:2   keyword  return
4:9   ident    x
4:11  symbol   *
4:13  ident    k
4:14  symbol   ;
5:1   symbol   }
```

Chapter 4 builds the lexer in C.

## Parsing and the syntax tree

The grammar in [chapter 2, The Anti language]({{% relref "/programming/writing-a-compiler/02-the-anti-language" %}}) fixes the order in which tokens may appear. The parser checks the tokens against it and builds a syntax tree, which has one node per construct. Each node holds the nodes of its parts. In the listing, indentation shows which node holds which, and each node is named after its grammar rule.

```text
function scale
  param x
    type int
  result
    type int
  block
    let_stmt k
      additive +
        int_lit 2
        int_lit 4
    return_stmt
      multiplicative *
        ident x
        ident k
```

The tree holds no `(`, `:`, `{` or `;`. Those tokens fix the structure of the source, and the tree records that structure in its shape. A missing `;` stops the pipeline at this stage with a compile error. Chapter 5 builds the parser and the tree.

## Semantic analysis

Some rules of the language lie outside the grammar, such as the type rules of chapter 2. Semantic analysis applies them. It connects every name to its declaration and gives every expression a type. The listing shows the tree of `scale` with the type of each node on the right.

```text
function scale             fn(int) -> int
  param x                  int
    type int
  result
    type int
  block
    let_stmt k             int
      additive +           int
        int_lit 2          int
        int_lit 4          int
    return_stmt
      multiplicative *     int
        ident x            int
        ident k            int
```

The literals `2` and `4` have no context that fixes their type, so both are `int`. The variable `k` has no declared type and takes the type of `2 + 4`. The product `x * k` multiplies two `int` values, and its type matches the declared result. A body of `return k > 0;` would stop here with a compile error, because `k > 0` has type `bool` and the declared result is `int`. Chapter 6 builds semantic analysis.

## Intermediate representation

The last step of the semantic analysis stage is lowering. Lowering translates the checked tree into an intermediate representation, IR. IR is a list of instructions, each with one operation and at most two operands, and neither the language nor the processor fixes its form. Every later stage works on IR rather than on the tree.

The command `antic --dump-ir main.anti` prints the IR of the whole file.

```text
fn main.scale(%0: i64) -> i64 {
b0:
    %1 = add i64 2, 4
    %2 = copy i64 %1
    %3 = mul i64 %0, %2
    ret i64 %3
}
fn main.main() -> i64 {
b0:
    %0 = call i64 @main.scale(7)
    ret i64 %0
}
```

A name that starts with `%` is a temporary, a name under which IR keeps an intermediate value. The temporary `%0` of `scale` holds the parameter `x`, and `%2` holds the variable `k`. The label `b0` names the first block of each function. A block is a list of instructions that starts at its label and ends with a jump, a branch or a return. The type `i64` is the IR type of `int`. The instruction `ret` returns its operand.

A struct is a record of named fields. IR holds no size of a struct and no offset of a field, because the layout of a struct differs between targets. It writes such a value by name, as the size of a type or the offset of a field. The back end computes the number for its target. Chapter 7 defines the IR, and chapter 8 builds lowering.

## Optimization

The optimizer rewrites IR into IR that computes the same results with fewer instructions. Constant folding is one of its rewrites. It computes an operation at compile time when every operand is a constant. The instruction `add i64 2, 4` has two constant operands, so the optimizer replaces `%1` with 6. The copy in `%2` then holds the constant 6 as well, and the multiplication uses 6 directly. The optimizer removes the two instructions whose results are no longer used and numbers the remaining temporaries again. The command `antic --dump-opt main.anti` prints the result.

```text
fn main.scale(%0: i64) -> i64 {
b0:
    %1 = mul i64 %0, 6
    ret i64 %1
}
fn main.main() -> i64 {
b0:
    %0 = call i64 @main.scale(7)
    ret i64 %0
}
```

The optimizer runs on the whole program, after antic has loaded every module. Chapter 10 builds it, together with a mode that optimizes one module alone. No pass folds a size that IR writes by name, because the number depends on the target.

## Instruction selection

A processor executes a fixed list of operations, its instruction set. This stage replaces each IR instruction with instructions from the set of the target processor. Before it selects, the back end lays out every struct for the target. Each size and offset that IR writes by name then becomes a number. A register is a storage cell inside the processor that holds one value. ARM64 has 31 general-purpose registers, `x0` to `x30`[^1].

At this stage the selector does not know which register each value will use. It writes virtual registers, placeholder names that the next stage replaces. Some IR instructions need more than one target instruction. The ARM64 `mul` instruction multiplies two registers and accepts no constant operand. The assembler llvm-mc 23.1.1 rejects `mul x0, x0, #6` with the message `invalid operand for instruction`. The IR instruction `mul i64 %0, 6` therefore becomes two instructions, one that loads 6 into a virtual register and one that multiplies.

Chapter 12 builds the selector, and chapters 14 and 15 give its instruction patterns for x86_64 and ARM64.

## Register allocation

Each virtual register now receives a real register. A calling convention fixes part of the choice. It is the set of rules for how functions pass arguments and results. On ARM64, `x0` to `x7` pass arguments and return results[^1]. Registers `x9` to `x15` hold intermediate values that a call is free to overwrite[^1].

For `scale`, the argument `x` arrives in `x0` and the result must leave in `x0`. The allocator keeps the argument and the product in `x0` and places the constant 6 in `x9`. Chapter 13 builds the allocator and the layout of stack frames.

## Assembly emission

The last stage of antic writes the target instructions as assembly text, one instruction or directive per line. A directive is a line that starts with a dot and tells the assembler how to lay out the object file. The command `antic -S main.anti` on macOS ARM64 writes the file `main.s`, and the listing is its complete text.

```asm
    .build_version macos, 11, 0
    .text
    .globl _anti.rt.main
    .set _anti.rt.main, _main.main
    .p2align 2
_main.scale:
L_main.scale.b0:
    mov x9, #6
    mul x0, x0, x9
    ret
    .p2align 2
_main.main:
L_main.main.b0:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    mov x0, #7
    bl _main.scale
    ldp x29, x30, [sp], #16
    ret
```

The listing was checked on an Apple Silicon Mac with macOS 26.6.2. llvm-mc from the LLVM 23.1.1 release archive assembled it without a warning. The linker `ld64.lld` of the same release linked it with the runtime library built from `rt/` in the book repository. The executable names lld 23.1.1 as its build tool, and the program exits with code 42.

### Directives and symbols

`.build_version macos, 11, 0` records in the object file that the code runs on macOS 11.0 and later. The directive `.text` starts the section that holds code. The directive `.p2align 2` places the next instruction at an address that is a multiple of 4. Every ARM64 instruction in this listing is 4 bytes.

A symbol is a name for an address in the object file. `_main.scale` and `_main.main` are symbols, derived from the module `main` and the function names. The leading `_` follows the macOS convention, under which clang writes the C function `main` as the symbol `_main`. The directive `.set` defines `_anti.rt.main` as a second name for `_main.main`, and `.globl` makes `_anti.rt.main` visible to other object files. The C function `main` in the runtime library calls the program through `_anti.rt.main`, a name that stays the same whatever the module is called. The name is the function `main` of the module `anti.rt`, which belongs to the runtime, and antic refuses to compile a module of that name. The symbols `_main.main` and `_main.scale` stay local to the object file.

A line that ends with `:` defines a name for the address of the next instruction. The labels `L_main.scale.b0` and `L_main.main.b0` mark the block `b0` of each function, the target of a jump inside the function. The assembler llvm-mc leaves a label that starts with `L` out of the symbol table of a Mach-O object file.

### Instructions

In `_main.scale`, `mov x9, #6` loads 6 into `x9`. `mul x0, x0, x9` multiplies `x0` by `x9` and stores the product in `x0`, and `ret` returns to the caller.

`_main.main` calls another function, and `bl` stores the return address in `x30`, the link register[^1]. The function `main` therefore saves `x30` before the call. The instruction `stp x29, x30, [sp, #-16]!` moves the stack pointer `sp` down by 16 bytes and stores `x29` and `x30` there. Those two values form a frame record, and `mov x29, sp` makes the frame pointer `x29` point at it. Apple requires `x29` to point at a valid frame record, and a function that calls nothing may skip it[^2]. The function `scale` calls nothing and has none. The final `ldp x29, x30, [sp], #16` restores both registers and moves `sp` back. The stack pointer stays a multiple of 16, as AAPCS64 requires[^1].

## Assembler and linker

llvm-mc, the assembler, encodes each instruction as its bytes and writes an object file. The object file holds the machine code, the symbols it defines and the symbols it uses. The object file from the listing defines `_anti.rt.main` as a global symbol, and `_main.main` and `_main.scale` as local ones. The block labels are absent from its symbol table.

The linker combines object files and libraries into an executable. It connects each symbol that one file uses to the address where another file defines it. For an Anti program the linker combines antic's object file with the C library and with anti_rt, the runtime library of antic. The library file is `libanti_rt.a` on Linux and macOS and `anti_rt.lib` on Windows. The file `rt/start.c` in anti_rt defines the C function `main`, which calls `_anti.rt.main` and returns its result as the exit code of the process. Chapter 2 specifies that entry sequence.

```c
/* The C entry point of every Anti program. It calls the program's main
   through anti.rt.main, the runtime entry symbol that antic defines, and
   returns the result as the process exit code. */
#include <stdint.h>

/* A symbol with a dot is not a C identifier, so the declaration names it
   with an assembler label. Mach-O adds '_' to C symbols, ELF does not. */
#if defined(__APPLE__)
#define ANTI_ENTRY_SYMBOL "_anti.rt.main"
#else
#define ANTI_ENTRY_SYMBOL "anti.rt.main"
#endif

extern int64_t anti_main(void) __asm__(ANTI_ENTRY_SYMBOL);

int main(void)
{
    return (int)anti_main();
}
```

The listing is the first version of `rt/start.c`, from [chapter 3]({{% relref "/programming/writing-a-compiler/03-setup-and-first-executable" %}}). The finished runtime has outgrown it, so the chapter folder keeps it as `start.c`. The test `start_01_return42` links it with a program of antic and runs the result. The first version holds only the call, with no arguments and no initialisation of the runtime, and later chapters extend it. The Windows symbol comes with [chapter 16]({{% relref "/programming/writing-a-compiler/16-assembly-emission" %}}) as `_A4anti2rt_main`, which names the entry without a dot. [Chapter 19]({{% relref "/programming/writing-a-compiler/19-strings-slices-and-bytes" %}}) extends the file to pass the command-line arguments and the environment to `main`. The call that initialises the runtime before `main` runs belongs to chapter 23, Libraries for C.

## Scope of antic

antic writes assembly text and stops. Producing the executable needs encoders for the instructions of two processors and support for the object file formats of three operating systems. The book leaves both tasks to existing tools.

| Targets | Object file format | Linker |
|---|---|---|
| Linux on x86_64 and ARM64 | ELF[^3] | `ld.lld` |
| macOS on x86_64 and ARM64 | Mach-O | `ld64.lld` |
| Windows on x86_64 and ARM64 | COFF[^4] | `lld-link` |

The tool objdump of Apple LLVM 21.0.0 reports the object file from the listing as `mach-o arm64`. The assembler llvm-mc covers all six targets, and so does lld of the same LLVM release under its three names. Chapter 16 gives their command lines for each target. The first executable of chapter 3 links with the linker of the host's own toolchain: GNU ld, `ld` of the Xcode Command Line Tools or `link.exe`. Chapter 3 installs the tools and runs them on the host before any stage of antic exists.

## Stages and chapters

| Stage | Chapters |
|---|---|
| Lexer | 4, The lexer |
| Parser | 5, The parser and the syntax tree |
| Semantic analysis | 6, Semantic analysis |
| Intermediate representation and lowering | 7, The intermediate representation, and 8, Lowering the syntax tree to IR |
| Optimizer | 10, The optimizer |
| Instruction selection | 12, Instruction selection |
| Register allocation | 13, Register allocation and stack frames |
| Assembly emission | 14, The x86_64 back end, 15, The ARM64 back end, and 16, Assembly emission per operating system |
| Assembler and linker | 3, Setup and the first executable, and 16 |

Chapter 9 adds modules and library files to the middle end. Chapter 11 describes the six targets, their object file formats and their calling conventions.

## Scope of the book

The compiler this book builds is the compiler that builds the language. It is not a teaching model beside a real one. Every listing is a file of the repository, every test runs, and the chapter tags hold the compiler as it stood at each chapter.

The book ends there. The language, its standard library, its build tool, its package repository and its editor support live at [anti-lang.com](https://anti-lang.com/), and that site is their reference. This book covers the compiler. It covers the parts of the language and the runtime that the compiler needs for its own tests. Chapter 25 says in detail what lives where.

## Next

[Chapter 2, The Anti language]({{% relref "/programming/writing-a-compiler/02-the-anti-language" %}}), specifies the core language that antic compiles. It defines the types, with the C types for bindings, and the literals, expressions, statements and functions of the language. Structs, unions and bitfields follow, together with packed and aligned structs. The chapter then specifies modules with export, doc comments and the memory model. It also lists the reserved and contextual words and the facts about the six targets that the language relies on.

## References

[^1]: Arm Limited, *Procedure Call Standard for the Arm 64-bit Architecture (AArch64)*, release 2025Q4, https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst

[^2]: Apple, *Writing ARM64 code for Apple platforms*, https://developer.apple.com/documentation/xcode/writing-arm64-code-for-apple-platforms

[^3]: Linux man-pages project, *elf(5), format of Executable and Linking Format (ELF) files*, https://man7.org/linux/man-pages/man5/elf.5.html

[^4]: Microsoft, *PE Format*, 2026, https://learn.microsoft.com/en-us/windows/win32/debug/pe-format
