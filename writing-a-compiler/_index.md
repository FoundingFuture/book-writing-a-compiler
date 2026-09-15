---
title: "Writing a compiler"
description: "A chapter series that builds antic, a compiler for the Anti language, in C from scratch, for Linux, macOS and Windows on x86_64 and ARM64."
date: 2026-09-13T22:32:00+02:00
lastmod: 2026-09-13T22:32:00+02:00
draft: false
---

This series builds a compiler in C, from an empty directory to a program that compiles a statically typed language for six targets. The language is Anti. The compiler is antic. Every chapter covers one part of compiler construction and includes the source that implements it. The complete source is on GitHub under the MIT licence.

<!--more-->

## Scope

antic contains the lexer, the parser, semantic analysis, a target-independent intermediate representation, an optimizer and two back ends. One back end targets x86_64, the other ARM64. The output is assembly text in GAS syntax. The assembler llvm-mc turns that text into an object file. The platform linker produces the executable. The series stops at assembly generation. An assembler and a linker for three object formats hold as much material as the rest of the compiler. Six targets multiply that work by six.

The six targets are Linux, macOS and Windows, each on x86_64 and ARM64. The code generation chapters cover the differences that the assembly output has to respect. Those are symbol naming, calling conventions, position-independent addressing, the entry point and the link against the C library.

## Language

Anti has ten kinds of type.

- `int`, signed integers
- `float`, floating point
- `bool`
- `char`, a Unicode scalar value
- `str`, an immutable UTF-8 string
- structs with C layout
- fixed-size arrays
- slices, a pointer plus a length
- typed pointers
- function types

Memory management follows C. Locals live on the stack. Heap memory is allocated and freed by the program. Source files are UTF-8, and so are string and character values. Modules map to files. Libraries are distributed in the compiler's intermediate representation, so one library file serves all six targets. A later chapter adds structured fork-join threading to the language.

## Reading order

The chapters build on each other and are numbered. Chapter 3 produces a running executable from a direct code path, before the pipeline exists. Every later chapter extends a working compiler. Each chapter opens with what the previous chapter built and closes with what the next one adds.

## Requirements

The compiler builds with CMake 3.20 and a C11 compiler. Compiling an Anti program needs llvm-mc, which ships with the project, and the platform linker. On macOS the linker comes with the Xcode Command Line Tools, on Linux with binutils, on Windows with the Visual Studio Build Tools. Chapter 3 lists the install commands per platform.
