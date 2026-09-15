---
title: "Testing six targets"
description: "Expected outputs per program, cross-assembling every target on one machine, VMs and CI."
date: 2026-09-13T22:32:00+02:00
lastmod: 2026-09-13T22:32:00+02:00
draft: true
weight: 210
---

## Links for every target

The linker lld of the pinned LLVM release links for all six targets on one host. The runtime archive holds a sysroot per target with the C library and the start files. A Mac therefore links executables for Linux and Windows as well. The ctest tests `cross_link_<target>` link one program for each target and check the file format of the executable with `llvm-objdump -h`. A target without a sysroot in the runtime archive reports its test as skipped.

On the development Mac all six tests link. The Windows sysroot comes from xwin, which `tools/get-sysroot.sh` runs only when its caller accepts the Microsoft licence terms with `--accept-license`. A link only shows that the executable has the right format and architecture. Running it needs a machine of that target, which the matrix of the GitHub Actions workflow provides when started by hand.

## Pinned tools in the workflow

Every job of `.github/workflows/test.yml` ends its installation with `tools/check-llvm.cmake`. The check compares `--version` of llvm-mc, llvm-ar, `ld.lld`, `ld64.lld` and `lld-link` with the version in `tools/llvm-version` and fails the job on any difference. The macOS x86_64 job installs `llvm@23` and `lld@23` from Homebrew, and the Windows jobs install LLVM from Chocolatey with the pinned version.
