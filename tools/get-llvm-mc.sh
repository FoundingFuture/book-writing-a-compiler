#!/bin/sh
# Install llvm-mc, lld, llvm-ar, llvm-objdump and llvm-readobj from the pinned
# LLVM release for this host into <dir>/bin. llvm-mc assembles, lld links
# under the names ld.lld, ld64.lld and lld-link, and llvm-ar writes static
# libraries. llvm-objdump reads the format and architecture of an output,
# and llvm-readobj decodes the Windows unwind data for the tests.
#
#   tools/get-llvm-mc.sh <dir>
#
# The release is the one in tools/llvm-version. Set LLVM_ARCHIVE to the path
# of an archive that is already downloaded to skip the download. The archive
# is checked against its SHA-256 digest from the GitHub release either way.
set -eu

VERSION=$(cat "$(dirname "$0")/llvm-version")
dest=${1:?usage: tools/get-llvm-mc.sh <dir>}

case "$(uname -s)-$(uname -m)" in
Darwin-arm64)
    asset=LLVM-$VERSION-macOS-ARM64.tar.xz
    digest=64220f1c99132ef7e580447781b84f96fbba6862a43a8f6522b423052cd67502 ;;
Linux-x86_64)
    asset=LLVM-$VERSION-Linux-X64.tar.xz
    digest=832aeb58d105de1cabc7b982dd2c65de0610f7377df48ae8fc2dd8e97420a15c ;;
Linux-aarch64)
    asset=LLVM-$VERSION-Linux-ARM64.tar.xz
    digest=3fbaaa6a1f147557a4095b9911f8dc2d4c745a11863982f76ee20e248c190a80 ;;
*)
    echo "LLVM $VERSION publishes no archive for $(uname -s) $(uname -m)" >&2
    exit 1 ;;
esac

mkdir -p "$dest/bin"
archive=${LLVM_ARCHIVE:-}
if [ -z "$archive" ]; then
    archive=$dest/$asset
    curl -fL -o "$archive" \
        "https://github.com/llvm/llvm-project/releases/download/llvmorg-$VERSION/$asset"
fi

if command -v shasum >/dev/null 2>&1; then
    actual=$(shasum -a 256 "$archive" | cut -d ' ' -f 1)
else
    actual=$(sha256sum "$archive" | cut -d ' ' -f 1)
fi
if [ "$actual" != "$digest" ]; then
    echo "$archive: SHA-256 $actual, expected $digest" >&2
    exit 1
fi

top=${asset%.tar.xz}
tar -xJf "$archive" -C "$dest" --strip-components=1 \
    "$top/bin/llvm-mc" "$top/bin/llvm-ar" "$top/bin/llvm-objdump" \
    "$top/bin/llvm-readobj" \
    "$top/bin/lld" "$top/bin/ld.lld" "$top/bin/ld64.lld" "$top/bin/lld-link"
cmake -DLLVM_BIN="$dest/bin" -P "$(dirname "$0")/check-llvm.cmake"
