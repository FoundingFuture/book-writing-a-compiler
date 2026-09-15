#!/bin/sh
# Install the sysroot of each target that lld links against into
# <dir>/<target>/, and the licence of each component into <dir>/licenses/.
#
#   tools/get-sysroot.sh <dir> <llvm-bin> [--accept-license] <target> [<target>]
#
# linux-x86_64, linux-arm64: musl and the compiler-rt builtins from the
#   Alpine packages of tools/sysroot-pins, checked against their digests.
# macos-arm64, macos-x86_64: the .tbd stubs of libSystem from the newest
#   SDK of the Command Line Tools for Xcode that ld64.lld in <llvm-bin>
#   reads. Only on a Mac.
# windows-x86_64, windows-arm64: the MSVC CRT and the Windows SDK import
#   libraries from xwin, at the versions of tools/sysroot-pins, checked
#   against the digest of the files. xwin needs the Microsoft licence terms
#   accepted, so the script runs it only when the caller passes
#   --accept-license.
set -eu

usage="usage: tools/get-sysroot.sh <dir> <llvm-bin> [--accept-license] <target> [<target>]"
dest=${1:?$usage}
llvm_bin=${2:?$usage}
shift 2
accept=no
if [ "${1:-}" = "--accept-license" ]; then
    accept=yes
    shift
fi
tools=$(cd "$(dirname "$0")" && pwd)
. "$tools/sysroot-pins"
mkdir -p "$dest/licenses"

fetch() {
    url=$1
    file=$2
    digest=$3
    if [ ! -f "$file" ]; then
        curl -fsSL -o "$file" "$url"
    fi
    if command -v shasum >/dev/null 2>&1; then
        actual=$(shasum -a 256 "$file" | cut -d ' ' -f 1)
    else
        actual=$(sha256sum "$file" | cut -d ' ' -f 1)
    fi
    if [ "$actual" != "$digest" ]; then
        echo "$file: SHA-256 $actual, expected $digest" >&2
        exit 1
    fi
}

# The digest of the regular files under a directory: their SHA-256 lines
# in the order of their paths, hashed once more. xwin adds symbolic links
# for other spellings on a case-sensitive file system, so links stay out.
tree_digest() {
    (
        cd "$1"
        find . -type f -print0 | LC_ALL=C sort -z |
            xargs -0 sh -c 'if command -v shasum >/dev/null 2>&1; then shasum -a 256 "$@"; else sha256sum "$@"; fi' sh |
            if command -v shasum >/dev/null 2>&1; then shasum -a 256; else sha256sum; fi
    ) | cut -d ' ' -f 1
}

linux() {
    target=$1
    arch=$2
    musl_digest=$3
    rt_digest=$4
    root=$dest/$target
    work=$dest/.download/$target
    mkdir -p "$work" "$root/usr/lib"
    fetch "$ALPINE_URL/$arch/musl-dev-$MUSL_APK.apk" "$work/musl-dev.apk" "$musl_digest"
    fetch "$ALPINE_URL/$arch/compiler-rt-$COMPILER_RT_APK.apk" "$work/compiler-rt.apk" "$rt_digest"
    tar -xzf "$work/musl-dev.apk" -C "$work" usr/include usr/lib 2>/dev/null
    rm -rf "$root/usr/include"
    cp -R "$work/usr/include" "$root/usr/include"
    for file in crt1.o crti.o crtn.o rcrt1.o Scrt1.o libc.a; do
        cp "$work/usr/lib/$file" "$root/usr/lib/$file"
    done
    builtins=usr/lib/llvm$COMPILER_RT_MAJOR/lib/clang/$COMPILER_RT_MAJOR/lib/$arch-alpine-linux-musl/libclang_rt.builtins-$arch.a
    tar -xzf "$work/compiler-rt.apk" -C "$work" "$builtins" 2>/dev/null
    cp "$work/$builtins" "$root/usr/lib/libclang_rt.builtins.a"
    fetch "$MUSL_SOURCE_URL" "$dest/.download/musl.tar.gz" "$MUSL_SOURCE_DIGEST"
    tar -xzf "$dest/.download/musl.tar.gz" -C "$dest/.download" "musl-$MUSL_VERSION/COPYRIGHT"
    cp "$dest/.download/musl-$MUSL_VERSION/COPYRIGHT" "$dest/licenses/musl.txt"
    fetch "$COMPILER_RT_LICENSE_URL" "$dest/licenses/compiler-rt.txt" "$COMPILER_RT_LICENSE_DIGEST"
}

macos() {
    target=$1
    arch=$2
    root=$dest/$target
    chosen=
    for sdk in $(ls -d /Library/Developer/CommandLineTools/SDKs/MacOSX[0-9]*.[0-9]*.sdk |
                 sed 's/.*MacOSX\(.*\)\.sdk/\1/' | sort -t . -k 1,1nr -k 2,2nr); do
        path=/Library/Developer/CommandLineTools/SDKs/MacOSX$sdk.sdk
        if "$llvm_bin/ld64.lld" -arch "$arch" -platform_version macos 11.0 "$sdk" \
            -syslibroot "$path" -dylib -o "$dest/.probe.dylib" -lSystem >/dev/null 2>&1; then
            chosen=$sdk
            break
        fi
    done
    rm -f "$dest/.probe.dylib"
    if [ -z "$chosen" ]; then
        echo "no SDK of the Command Line Tools links with $llvm_bin/ld64.lld" >&2
        exit 1
    fi
    path=/Library/Developer/CommandLineTools/SDKs/MacOSX$chosen.sdk
    rm -rf "$root"
    mkdir -p "$root/usr/lib"
    cp -P "$path/usr/lib/libSystem.tbd" "$path/usr/lib/libSystem.B.tbd" "$root/usr/lib/"
    cp -R "$path/usr/lib/system" "$root/usr/lib/system"
    echo "$chosen" > "$root/sdk-version"
    cat > "$dest/licenses/macos-sdk.txt" <<EOF
The .tbd stubs in sysroot/macos-arm64 and sysroot/macos-x86_64 are copied
from MacOSX$chosen.sdk of the Command Line Tools for Xcode on the build Mac.
Apple distributes that SDK under the Xcode and Apple SDKs Agreement.
EOF
}

windows() {
    target=$1
    arch=$2
    digest=$3
    if [ "$accept" != yes ]; then
        echo "$target: xwin downloads the Microsoft CRT and Windows SDK. Pass --accept-license to accept their licence terms." >&2
        exit 1
    fi
    found=$(xwin --version | cut -d ' ' -f 2)
    if [ "$found" != "$XWIN_VERSION" ]; then
        echo "xwin has version $found, the pin is $XWIN_VERSION" >&2
        exit 1
    fi
    rm -rf "$dest/$target"
    xwin --accept-license --cache-dir "$dest/.download/xwin" --arch "$arch" \
        --crt-version "$XWIN_CRT_VERSION" --sdk-version "$XWIN_SDK_VERSION" \
        splat --output "$dest/$target"
    actual=$(tree_digest "$dest/$target")
    if [ "$actual" != "$digest" ]; then
        echo "$dest/$target: SHA-256 of the files $actual, expected $digest" >&2
        exit 1
    fi
    cat > "$dest/licenses/windows-sdk.txt" <<EOF
The import libraries in sysroot/windows-x86_64 and sysroot/windows-arm64
come from the Microsoft C runtime $XWIN_CRT_VERSION and the Windows SDK
$XWIN_SDK_VERSION, fetched with xwin $XWIN_VERSION. Microsoft distributes
them under the licence terms that xwin shows and that the caller accepted
with --accept-license.
EOF
}

for target in "$@"; do
    case $target in
    linux-x86_64) linux "$target" x86_64 "$MUSL_DEV_X86_64" "$COMPILER_RT_X86_64" ;;
    linux-arm64) linux "$target" aarch64 "$MUSL_DEV_AARCH64" "$COMPILER_RT_AARCH64" ;;
    macos-arm64) macos "$target" arm64 ;;
    macos-x86_64) macos "$target" x86_64 ;;
    windows-x86_64) windows "$target" x86_64 "$XWIN_TREE_X86_64" ;;
    windows-arm64) windows "$target" aarch64 "$XWIN_TREE_AARCH64" ;;
    *) echo "unknown target $target" >&2; exit 1 ;;
    esac
    echo "$dest/$target"
done
