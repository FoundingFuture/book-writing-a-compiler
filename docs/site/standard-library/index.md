---
title: "The standard library"
description: "Console and file I/O, string formatting, regular expressions, graphics, audio and networking modules."
date: 2026-09-13T22:32:00+02:00
lastmod: 2026-09-13T22:32:00+02:00
draft: true
weight: 260
---

## First modules

The directory `std/` holds the modules of the standard library under the reserved root `anti`, under the 0BSD licence of `std/LICENSE`. The build writes each module as a library file into `std/` of the runtime archive, as the package `anti` with the version of antic. Every module of a package lies under the package root, and `anti.io`, `anti.text` and `anti.license` lie under `anti`. The notice of a program that imports one of them therefore names the package `anti`, which the test `program_licenses_std` checks. A program imports them without `-I`, because antic searches `std/` of the runtime archive after the roots of the command line.

| Module | Functions | C side |
|---|---|---|
| `anti.io` | `print`, `println`, `eprint`, `eprintln`, `exit` | `rt/io.c` |
| `anti.text` | `equal`, `from_c`, `byte_count`, `char_count` | `rt/text.c` |
| `anti.license` | `text` | `rt/license.c` |

The module `anti.io` writes through the C streams `stdout` and `stderr`, so its output and the output of `printf` in one program keep their order. The function `text.from_c` returns a `str` that points into the C string, because Anti builds no `str` from a pointer and a length. The function `license.text` returns the lines of the notice `anti_licenses` between its markers. In a static library for C with a bundled runtime it returns an empty text, because an archive carries no notice, as chapter 25 describes. Each module has `//!` documentation and a `///` comment on every `pub` item, and the tests `std_io`, `std_text` and `std_license` run one program for each.

The modules `anti.net`, `anti.regex`, `anti.raylib` and `anti.miniaudio` wait for the native libraries of the runtime archive.
