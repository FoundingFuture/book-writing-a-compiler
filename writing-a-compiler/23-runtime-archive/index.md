---
title: "The runtime archive"
description: "The libraries directory, its CMake build for six targets, static linking and the archive layout."
date: 2026-09-13T22:32:00+02:00
lastmod: 2026-09-13T22:32:00+02:00
draft: true
weight: 230
---

## Sysroots

The directory `sysroot/<target>/` of the runtime archive holds what lld links a program against on that target. The script `tools/get-sysroot.sh` installs each one, and the CMake build copies them into the runtime archive with the licence of each component in `licenses/`.

| Target | Contents | Source | Licence |
|---|---|---|---|
| linux-x86_64, linux-arm64 | musl 1.2.6 and `libclang_rt.builtins.a` | Alpine Linux 3.24 packages `musl-dev` and `compiler-rt` | MIT, Apache 2.0 with LLVM Exceptions |
| macos-x86_64, macos-arm64 | `.tbd` stubs of libSystem | Command Line Tools for Xcode | Xcode and Apple SDKs Agreement |
| windows-x86_64, windows-arm64 | Microsoft C runtime 14.44.17.14 and Windows SDK 10.0.26100 | xwin 0.10.0 | Microsoft licence terms |

The file `tools/sysroot-pins` holds the versions and the SHA-256 digests of the downloaded files. For a Windows sysroot xwin writes a tree of about 5600 files, and the pin is the SHA-256 digest of the sorted digests of those files. The script checks it after each download. The build also compiles the runtime library for every other target that has a sysroot, with clang and the headers of that sysroot.

## Mixing musl with glibc

A Linux program that antic links carries musl inside the executable. A static library of Linux, such as a bundled native library, is compiled against the headers of some C library. A library that a distribution builds against glibc with `_FORTIFY_SOURCE` calls checked functions such as `__printf_chk`. The `libc.a` of musl 1.2.6 defines `printf` and no function whose name ends in `_chk`. The link then stops with an undefined symbol. The runtime archive therefore compiles every static library it bundles for Linux against the musl headers of the sysroot. It takes no prebuilt archive from a glibc system.
