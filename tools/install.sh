#!/bin/sh
# Install Anti for the user who runs it, under ~/.anti.
#
#   curl -fsSL https://anti-lang.com/install.sh | sh
#   curl -fsSL https://anti-lang.com/install.sh | sh -s -- --intel  # docs-style:ignore
#
# It downloads the package of this host. It checks the SHA-256 against the
# SHA256SUMS beside it, unpacks it and installs the sysroot of the host.
# The SDK of macOS belongs to Apple and the C runtime of Windows to
# Microsoft. The script asks before it takes either of them.
#
# The option --arm or --intel takes the package of that processor rather
# than the one of this machine. A machine that emulates the other
# processor runs it. ANTI_ARCH holds the same choice as arm64 or x86_64.
#
# Each question takes yes or no from a variable of its own name.
#
#   ANTI_VERSION: the version to install, default the newest.
#   ANTI_ARCH: arm64 or x86_64, default the processor of this machine.
#   ANTI_HOME: where to install, default $HOME/.anti, and
#              $HOME/.anti-<cpu> for the package of another processor.
#   ANTI_REPLACE: replace an install that is there.
#   ANTI_PATH: add the line to the shell profile.
#   ANTI_MICROSOFT: let xwin fetch the CRT and the Windows SDK.
set -eu

base=https://anti-lang.com/downloads/resources
version=${ANTI_VERSION:-}
arch=${ANTI_ARCH:-}

for argument in "$@"; do
    case $argument in
    --arm | --arm64) arch=arm64 ;;
    --intel | --x86_64 | --x64) arch=x86_64 ;;
    *)
        echo "anti: unknown option $argument" >&2
        echo "usage: install.sh [--arm | --intel]" >&2
        exit 2
        ;;
    esac
done

say() {
    echo "anti: $*"
}

fail() {
    echo "anti: $*" >&2
    exit 1
}

# Ask one yes or no question. The variable of its name answers it, and a
# plain return accepts. A pipe leaves no terminal on standard input, so
# the question goes to /dev/tty, and without one the default stands.
ask() {
    eval "given=\${ANTI_$1:-}"
    case $given in
    y | Y | yes | Yes) return 0 ;;
    n | N | no | No) return 1 ;;
    esac
    if ! { exec 3</dev/tty; } 2>/dev/null; then
        return 0
    fi
    answer=""
    printf '%s [Y/n] ' "$2" >&2
    read -r answer <&3 || true
    exec 3<&-
    case $answer in
    n | N | no | No) return 1 ;;
    *) return 0 ;;
    esac
}

sha256() {
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | cut -d ' ' -f 1
    else
        sha256sum "$1" | cut -d ' ' -f 1
    fi
}

case "$(uname -s)" in
Darwin) os=macos ;;
Linux) os=linux ;;
*) fail "no package for $(uname -s)" ;;
esac
case "$(uname -m)" in
arm64 | aarch64) native=arm64 ;;
x86_64) native=x86_64 ;;
*) fail "no package for $(uname -m)" ;;
esac
if [ -z "$arch" ]; then
    arch=$native
fi
case $arch in
arm64 | x86_64) ;;
*) fail "the processor $arch has no package, only arm64 and x86_64" ;;
esac
host=$os-$arch

# DESIGN: a package of the other processor runs under emulation and is
# installed beside the native one, never over it. Both directories then
# work, and the one on the PATH stays the native one.
if [ "$arch" = "$native" ]; then
    home=${ANTI_HOME:-$HOME/.anti}
else
    home=${ANTI_HOME:-$HOME/.anti-$arch}
    say "installing the $arch package, which this machine runs under emulation"
fi

if [ -z "$version" ]; then
    version=$(curl -fsSL "$base/anti/latest" | tr -d ' \n')
fi
asset=anti-$version-$host.tar.xz
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

say "downloading $asset"
curl -fsSL -o "$work/$asset" "$base/anti/$version/$asset"
curl -fsSL -o "$work/SHA256SUMS" "$base/anti/$version/SHA256SUMS"
want=$(grep " $asset\$" "$work/SHA256SUMS" | cut -d ' ' -f 1)
got=$(sha256 "$work/$asset")
if [ "$want" != "$got" ]; then
    fail "$asset: SHA-256 $got, expected $want"
fi

if [ -e "$home" ] && ! ask REPLACE "$home exists. Replace it?"; then
    fail "$home exists. Set ANTI_HOME elsewhere to keep it"
fi
rm -rf "$home"
mkdir -p "$(dirname "$home")"
tar -xJf "$work/$asset" -C "$work"
mv "$work/anti" "$home"
say "installed $version in $home"

# DESIGN: the site serves this installer, so it is always the newest.
# The CMake scripts it drives come from the package, which is as old as
# the version installed. The number rises when the options it passes
# change, and a package older than that fails here rather than halfway.
PACKAGE_API=1
api=0
if [ -f "$home/tools/package-api" ]; then
    api=$(tr -d ' \n' < "$home/tools/package-api")
fi
if [ "$api" -lt "$PACKAGE_API" ]; then
    fail "Anti $version speaks version $api of the installer interface, and \
this installer needs $PACKAGE_API. Install a newer version of Anti."
fi

# lld answers to its four names through argv[0], and the package carries
# one copy of it.
if [ -e "$home/bin/lld" ]; then
    for name in ld.lld ld64.lld lld-link; do
        if [ ! -e "$home/bin/$name" ]; then
            cp "$home/bin/lld" "$home/bin/$name"
        fi
    done
fi

# CMake installs the sysroot of this host. The package holds the script
# and the pins, and the pinned CMake stands in when the host has none.
cmake=$(command -v cmake || true)
if [ -z "$cmake" ]; then
    pin=$(grep "^$host-url=" "$home/tools/cmake-pin" | cut -d = -f 2-)
    cmake_version=$(tr -d ' \n' < "$home/tools/cmake-version")
    url=$(echo "$pin" | sed "s/@VERSION@/$cmake_version/g")
    digest=$(grep "^$host-digest=" "$home/tools/cmake-pin" | cut -d = -f 2-)
    say "installing CMake $cmake_version, which the sysroot step needs"
    curl -fsSL -o "$work/cmake.tar.gz" "$url"
    got=$(sha256 "$work/cmake.tar.gz")
    if [ "$digest" != "$got" ]; then
        fail "CMake: SHA-256 $got, expected $digest"
    fi
    mkdir -p "$home/tools/cmake"
    tar -xzf "$work/cmake.tar.gz" -C "$home/tools/cmake" --strip-components=1
    cmake=$(find "$home/tools/cmake" -name cmake -type f -perm -u+x | head -1)
fi

case $host in
macos-*)
    if [ ! -d /Library/Developer/CommandLineTools/SDKs ]; then
        say "the macOS SDK is missing. Apple installs it with xcode-select --install"
        if ask "Run xcode-select --install now?"; then
            xcode-select --install || true
            say "run this installer again when the Command Line Tools are in place"
            exit 0
        fi
        fail "no SDK, so no linking for macOS"
    fi
    say "taking the SDK stubs from the Command Line Tools"
    "$cmake" -DDEST="$home/sysroot" -DLLVM_BIN="$home/bin" \
        -DTARGETS="macos-arm64;macos-x86_64" \
        -P "$home/tools/get-sysroot.cmake" >/dev/null
    ;;
esac

if ask MICROSOFT "Also install the Microsoft CRT and Windows SDK, so that this host builds Windows programs?"; then
    say "xwin downloads them, and Microsoft licenses them to you"
    # The progress bar of xwin draws only when its own standard output is
    # a terminal. CMake writes the command and this shell runs it, so the
    # bar has the terminal of the user rather than a pipe.
    "$cmake" -DDEST="$home/sysroot" -DLLVM_BIN="$home/bin" \
        -DACCEPT_LICENSE=yes -DSPLAT=script \
        -DTARGETS="windows-x86_64;windows-arm64" \
        -P "$home/tools/get-sysroot.cmake" >/dev/null
    sh "$home/sysroot/.download/splat.sh"
    "$cmake" -DDEST="$home/sysroot" -DLLVM_BIN="$home/bin" \
        -DACCEPT_LICENSE=yes -DSPLAT=done \
        -DTARGETS="windows-x86_64;windows-arm64" \
        -P "$home/tools/get-sysroot.cmake"
else
    say "Windows programs need the Microsoft CRT. Add it later with:"
    echo "    $cmake -DDEST=$home/sysroot -DLLVM_BIN=$home/bin \\"
    echo "        -DACCEPT_LICENSE=yes -DTARGETS='windows-x86_64;windows-arm64' \\"
    echo "        -P $home/tools/get-sysroot.cmake"
fi

# DESIGN: the PATH names one antic, and it is the one this machine runs
# without emulation. A package of the other processor is therefore called
# by its path.
if [ "$arch" != "$native" ]; then
    say "run it with: $home/bin/antic hello.anti -o hello"
    exit 0
fi

# The profile of the login shell, which the installer writes only with a
# yes. The marker lets the uninstaller remove its own line and leave one
# that the user wrote.
case ${SHELL:-} in
*/zsh) profile=$HOME/.zshrc ;;
*/bash) profile=$HOME/.bashrc ;;
*) profile=$HOME/.profile ;;
esac
line="export PATH=\"$home/bin:\$PATH\"  # anti"
case :${PATH}: in
*:"$home/bin":*)
    say "$home/bin is already on the PATH"
    ;;
*)
    if [ -f "$profile" ] && grep -q "# anti\$" "$profile"; then
        say "$profile already holds the line for Anti"
    elif ask PATH "Add $home/bin to the PATH in $profile?"; then
        printf '%s\n' "$line" >> "$profile"
        say "added the line to $profile, which a new shell reads"
    else
        say "add this line to your shell profile yourself:"
        echo "    $line"
    fi
    ;;
esac
say "then compile a program with: antic hello.anti -o hello"
