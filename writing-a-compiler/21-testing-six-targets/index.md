---
title: "Testing six targets"
description: "How antic is tested for six targets from one machine: expected outputs per program, cross assembly, cross links, and the machines that run the rest."
summary: "Expected outputs per program. Assembling and linking every target on one machine, running on VMs and CI started by hand. Comparing x86_64 and ARM64 results against each other. Dev and release modes in the test suite. Byte-level output comparison and the raw-bytes rule for Windows consoles."
date: 2026-09-13T22:32:00+02:00
lastmod: 2026-09-16T21:40:00+02:00
draft: false
weight: 210
tags: [compilers, programming-languages]
keywords: [test suite, ctest, cross assembly, expected output, chapter tags, virtual machines, line endings, llvm-mc]
---

## Previously

[Chapter 20, Function pointers]({{% relref "/programming/writing-a-compiler/20-function-pointers" %}}),
finishes the types of chapter 2. Every type now lowers, both back ends cover every
operation, and the emitter writes assembly for six targets. This chapter is about
proving that on one machine.

## Numbers

The suite holds 723 ctest tests, and none of them is skipped. The largest family is
the 388 `excerpt_*` tests, which find every code listing of this book in the file it
came from. After those come 120 `asm_*` tests, 53 `program_*` tests and 45 `dump_*`
tests. The programs themselves are 22 files in `tests/programs/`, each with an
expected output beside it.

## Links for every target

The linker lld of the pinned LLVM release links for all six targets on one host. The runtime archive holds a sysroot per target with the C library and the start files. A Mac therefore links executables for Linux and Windows as well. The ctest tests `cross_link_<target>` link one program for each target and check the file format of the executable with `llvm-objdump -h`. A target without a sysroot in the runtime archive has no such test, and the build says so at configuration time.

On the development Mac all six tests link. The Windows sysroot comes from xwin, which `tools/get-sysroot.cmake` runs only when its caller accepts the Microsoft licence terms with `--accept-license`. A link only shows that the executable has the right format and architecture. Running it needs a machine of that target. The development Mac runs macos-x86_64 programs through Rosetta, and the matrix of the GitHub Actions workflow provides the other four when started by hand.

## Pinned tools in the workflow

Every job of `.github/workflows/test.yml` ends its installation with `tools/check-llvm.cmake`. The check compares `--version` of llvm-mc, llvm-ar, `ld.lld`, `ld64.lld` and `lld-link` with the version in `tools/llvm-version` and fails the job on any difference. Every job but one installs the tools with `tools/get-llvm.cmake`, which downloads the archive of that host from anti-lang.com and checks its SHA-256 digest. No job is an exception. LLVM publishes no archive for macOS on x86_64. That host is built from the pinned source, as the two Linux hosts are, and its archive stands beside the other five.

## A test per program

One `.anti` file, its exit code and its output make a test. The runner compiles the
file, runs it and compares what came back with a file beside the source. The first
line of that file is the exit code, and every byte after it is the expected output.

```cmake
file(READ "${expected_file}" expected)
if(NOT expected MATCHES "^exit ([0-9]+)\n")
    message(FATAL_ERROR "${expected_file} does not start with 'exit N'")
endif()
set(expected_exit "${CMAKE_MATCH_1}")
string(REGEX REPLACE "^exit [0-9]+\n" "" expected_stdout "${expected}")

# DESIGN: an expected file records the exit code that POSIX shows, which
# is the low eight bits of what the program returned. Windows reports all
# thirty-two, so a program that returns 521 gives 521 there and 9 on
# Linux. The comparison takes the low eight bits on a Windows host.
if(CMAKE_HOST_WIN32 AND exit_code MATCHES "^-?[0-9]+$")
    math(EXPR exit_code "((${exit_code}) % 256 + 256) % 256")
endif()
if(NOT exit_code STREQUAL expected_exit)
    message(FATAL_ERROR "exit code ${exit_code}, expected ${expected_exit}")
endif()
if(NOT stdout STREQUAL expected_stdout)
    message(FATAL_ERROR "standard output differs\nexpected:\n${expected_stdout}\ngot:\n${stdout}")
endif()
```

The comparison is exact. No test trims whitespace, folds line endings or reads the
output as text in a locale. A program that prints `hello` and a program that prints
`hello ` differ, and the test says so.

## Line endings

A checkout that rewrites the files defeats that exactness. Git on Windows turns line
feeds into carriage return and line feed for a text file, and an expected output
rewritten that way matches nothing. The repository therefore pins the line endings.

```text
* text=auto eol=lf

*.cmd text eol=crlf
*.bat text eol=crlf
```

A batch file is the one thing that wants the other ending. The test `line_endings`
reads the rule and then every expected output and every test program.

```cmake
file(GLOB_RECURSE files "${ROOT}/tests/programs/*.expected"
     "${ROOT}/tests/programs/*.anti" "${ROOT}/tests/std/*.expected")
foreach(file IN LISTS files)
    file(READ "${file}" text HEX)
    if(text MATCHES "0d0a")
        get_filename_component(name "${file}" NAME)
        message(FATAL_ERROR "${name} holds a carriage return before a line "
                            "feed, which no test of it can match")
    endif()
endforeach()
```

## Assembly for every target

A program test runs on the host alone, because only the host can run its executable.
The assembly of every target is another matter, and 120 `asm_*` tests cover it. Each
one compiles a program with `-S --target <name>` and hands the text to llvm-mc for
that triple. An instruction that the emitter spells wrongly fails there, on the
machine that wrote it, without a virtual machine of that target.

The `dump_*` tests go one level deeper. They pin the IR, the selected machine code and
the register allocation of a program as text. A change in a pass then shows up as a
diff rather than as a wrong answer three stages later. The `--dev` mode of antic
compiles one module into its own object, and the dump tests cover both that and the
whole program compile.

## Chapter tags

The repository holds the finished compiler, and a reader who checks out `chapter-9`
gets the tree as it stood at chapter 9. The tests of later chapters would fail there,
so every test names its chapter.

```cmake
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/chapters.txt" chapter_rules
    REGEX "^[0-9a-z]+ ")
get_property(all_tests DIRECTORY PROPERTY TESTS)
foreach(test IN LISTS all_tests)
    set(test_chapter "")
    foreach(rule IN LISTS chapter_rules)
        string(FIND "${rule}" " " space)
        string(SUBSTRING "${rule}" 0 ${space} chapter)
        math(EXPR start "${space} + 1")
        string(SUBSTRING "${rule}" ${start} -1 pattern)
        if(test MATCHES "${pattern}")
            if(chapter STREQUAL "excerpt")
                math(EXPR test_chapter "${CMAKE_MATCH_1}")
            else()
                set(test_chapter ${chapter})
            endif()
            break()
        endif()
    endforeach()
    if(test_chapter STREQUAL "")
        message(FATAL_ERROR "the test ${test} has no chapter in tests/chapters.txt")
    endif()
```

The file `tests/chapters.txt` maps a test name to a chapter with a regular expression.
A test of a chapter beyond the last one in the tree is disabled rather than run. A
test that no rule names stops the configuration, which is what keeps the map honest. A
new test without a chapter is a configuration error rather than a silent gap.

## Machines of the other targets

A link proves the format and the architecture of an executable. It proves nothing
about what the program does when a processor runs it. Four of the six targets need a
machine of their own.

| Target | Where it runs |
|---|---|
| macos-arm64 | The development Mac |
| macos-x86_64 | The same Mac, through Rosetta |
| linux-arm64 | A virtual machine on the Mac, per `docs/vm-setup.md` |
| windows-arm64 | A second virtual machine on the same Mac |
| linux-x86_64, windows-x86_64 | The GitHub Actions runners, started by hand |

The two virtual machines take the tree as `git archive HEAD` writes it, which is what
a reader's checkout holds. They reach what stays beyond the Mac. The Windows branch of
`rt/start.c` runs against the Microsoft C runtime, `c_wchar` is 16 bits wide, and a
linker that consumes the unwind data of chapter 16 reads it there.

## Comparing the two processors

A program that gives one answer on ARM64 and another on x86_64 has a bug. It sits in a
back end, in the ABI code or in the layout. Every program test shares one expected
file across the targets for that reason. Output that differs by design gets a second file, named
in the test rather than guessed.

## Tests

The suite is the subject of this chapter, so the summary is its own state. Every one
of the 723 tests passes on the development Mac, none is skipped, and the build has
zero warnings on clang. The virtual machines run the same suite from an export of the
same commit.

## Next

[Chapter 22, Threads]({{% relref "/programming/writing-a-compiler/22-threads" %}}), adds the `worker` and `parallel` constructs. It covers
the rule
in the type checker that keeps one worker away from the memory of another, and the
pool that runs the chunks. It measures what the construct buys on a machine with
fourteen logical processors.

## References

[^1]: Kitware, *ctest(1)*, CMake 4.2 documentation, the options `-R`, `-L` and `-N`, https://cmake.org/cmake/help/v4.2/manual/ctest.1.html

[^2]: Git, *gitattributes(5)*, the attributes `text` and `eol`, https://git-scm.com/docs/gitattributes
