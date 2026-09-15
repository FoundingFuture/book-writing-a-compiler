# VM setup

Two virtual machines on the development Mac run the test suite on Linux ARM64 and on Windows ARM64. They cover the untested items of `docs/decisions.md` that need no x86_64 processor. The targets linux-x86_64, macos-x86_64 and windows-x86_64 stay for the GitHub Actions runners on a release day, because x86_64 programs do not run on the Mac.

Eddie installs UTM, both images and the Windows licence. Each VM gets the tools below once. The test run then exports the committed tree of the Mac with `git archive`, as a reader's checkout holds it, and builds it in a fresh directory.

## Linux ARM64

### Image and machine

- Image: `ubuntu-24.04.5-live-server-arm64.iso` from https://cdimage.ubuntu.com/releases/24.04/release/. The runner `ubuntu-24.04-arm` of the workflow runs the same Ubuntu release, 24.04.
- UTM: a new machine with Virtualize, Linux and the ISO. Give it 4 cores, 8 GB of memory and 64 GB of disk.
- In the installer, select "Install OpenSSH server".

### Packages and tools

Run these commands in the VM once. The tools go to `~/anti`, outside the tree that each run replaces.

```sh
sudo apt update
sudo apt install -y build-essential cmake git curl xz-utils
git -C /tmp clone --depth 1 https://github.com/FoundingFuture/book-writing-a-compiler book
sh /tmp/book/tools/get-llvm-mc.sh ~/anti/toolchain
sh /tmp/book/tools/get-sysroot.sh ~/anti/sysroot ~/anti/toolchain/bin linux-x86_64 linux-arm64
sh /tmp/book/tools/get-raylib.sh ~/anti/raylib
```

The clone only supplies the scripts. If the repository is private, copy `tools/` with `scp -r tools anti-linux:/tmp/book/` instead.

### SSH from the Mac

Add the VM to `~/.ssh/config` on the Mac. UTM shows the address of the VM in its window.

```text
Host anti-linux
    HostName <address of the VM>
    User <user of the VM>
```

### Test run

Run this command in the repository on the Mac. It exports `HEAD`, builds it and runs every test.

```sh
git archive HEAD | ssh anti-linux 'rm -rf book && mkdir book && tar -x -f - -C book && cd book &&
  cmake -S . -B build -DANTIC_LLVM_MC="$HOME/anti/toolchain/bin/llvm-mc" \
    -DANTIC_LLVM_AR="$HOME/anti/toolchain/bin/llvm-ar" \
    -DANTIC_SYSROOT_DIR="$HOME/anti/sysroot" \
    -DANTIC_RAYLIB_DIR="$HOME/anti/raylib/raylib-6.0" &&
  cmake --build build -j"$(nproc)" &&
  ctest --test-dir build -j"$(nproc)" --output-on-failure'
```

### Coverage

| Untested item | Tests that run it |
|---|---|
| linux-arm64 programs, which the Mac only links | `program_*`, `std_*`, `dev_modules` |
| Static PIE programs against musl | `program_*`, linked with ld.lld against `~/anti/sysroot/linux-arm64` |
| C objects built against glibc in a musl program | `program_abi_structs`, `program_abi_raymath`, `program_abi_wchar`, whose C files gcc compiles |
| The 16-aligned register rule of AAPCS64 outside Apple | `program_abi_structs` |
| `.init_array` constructors of a shared library | `clib_shared`, `clib_loader`, `clib_two` |
| The dynamic linker path `/lib/ld-linux-aarch64.so.1` | `program_platform_linker` with GNU ld |
| The ABI probe on Linux | `abi_probe` |
| `--soname` | No test links a versioned library yet. The VM can run one. |

## Windows ARM64

### Image and machine

- Image: the Windows 11 ARM64 ISO from https://www.microsoft.com/en-us/software-download/windows11arm64, with Eddie's licence. Parallels Desktop works as well as UTM.
- UTM: a new machine with Virtualize, Windows and the ISO. Give it 4 cores, 8 GB of memory and 128 GB of disk.

### Packages and tools

Run these commands in an administrator PowerShell in the VM once. The first two lines install and start the OpenSSH server, as Microsoft's page "Get started with OpenSSH Server for Windows" describes.

```powershell
Add-WindowsCapability -Online -Name OpenSSH.Server~~~~0.0.1.0
Start-Service sshd; Set-Service -Name sshd -StartupType 'Automatic'
winget install --id Git.Git -e
winget install --id Kitware.CMake -e
winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.ARM64 --includeRecommended"
```

Install Chocolatey as https://chocolatey.org/install describes. Then install LLVM at the pin, as the workflow does, and copy the tools that the build checks.

```powershell
choco install llvm --version 23.1.1 -y
New-Item -ItemType Directory -Force C:\anti\toolchain\bin | Out-Null
foreach ($tool in "llvm-mc", "llvm-ar", "llvm-objdump", "llvm-readobj", "lld", "ld.lld", "ld64.lld", "lld-link") {
    Copy-Item "C:\Program Files\LLVM\bin\$tool.exe" C:\anti\toolchain\bin\
}
```

Create `C:\anti\test.cmd` with these lines. `vcvarsall.bat arm64` sets the MSVC environment, including the variable `LIB` that lld-link reads.

```bat
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" arm64
cd /d C:\anti\book
cmake -S . -B build -DANTIC_LLVM_MC=C:/anti/toolchain/bin/llvm-mc.exe -DANTIC_LLVM_AR=C:/anti/toolchain/bin/llvm-ar.exe
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

### SSH from the Mac

```text
Host anti-windows
    HostName <address of the VM>
    User <user of the VM>
```

### Test run

Run these commands in the repository on the Mac. The first one replaces `C:\anti\book` with the export of `HEAD`, and the second one runs the build and the tests.

```sh
git archive HEAD | ssh anti-windows "rmdir /s /q C:\anti\book & mkdir C:\anti\book && tar -x -f - -C C:\anti\book"
ssh anti-windows C:\anti\test.cmd
```

### Coverage

| Untested item | Tests that run it |
|---|---|
| windows-arm64 programs, which the Mac only links | `program_*`, `std_*`, linked with lld-link against `LIB` |
| The Windows branch of `rt/start.c`, compiled with MSVC | every `program_*` test, through `anti_rt.lib` |
| `c_wchar` at 16 bits and `c_long` at 32 bits against MSVC | `program_abi_wchar`, `program_abi_structs` |
| Exception unwinding through an Anti frame | The runtime test that closes the unwind data of chapter 16: an exception raised in C unwinds through an Anti frame to a handler in C. `llvm-readobj` proves that the tables parse, not that Windows walks them. The test does not exist yet. |
| Shared libraries, `.def` files and `.CRT$XCU` constructors | The `clib_*` tests skip Windows until `tests/run_clib.cmake` supports MSVC. |
| Whether `link.exe` accepts the COFF symbol form | `program_platform_linker` skips Windows until it supports `link.exe`. |
| Float aggregates of one member against MSVC | No test compares them yet. The VM can run one. |
| Pointer equality of DLL functions | Waits for the DLL-based libraries of chapter 23. |

## Runners

The workflow `.github/workflows/test.yml` covers the three x86_64 targets when Eddie starts it on a release day. They are the x86_64 programs and float conversions of chapter 17, the Windows x64 unwind data at run time and macos-x86_64 programs.
