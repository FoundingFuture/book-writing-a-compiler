#!/bin/sh
# Download the pinned raylib release source into <dir>/raylib-<version>.
#
#   tools/get-raylib.sh <dir>
#
# GitHub publishes no digest for source archives. The SHA-256 digest below
# is the one of the archive that antic was first tested with.
set -eu

VERSION=6.0
DIGEST=2b3ee1e2120c7a0796b33062c7e9a694dd8a8caa56a96319ac8c8ecf54a90d0b
dest=${1:?usage: tools/get-raylib.sh <dir>}

mkdir -p "$dest"
archive=$dest/raylib-$VERSION.tar.gz
curl -fL -o "$archive" \
    "https://github.com/raysan5/raylib/archive/refs/tags/$VERSION.tar.gz"

if command -v shasum >/dev/null 2>&1; then
    actual=$(shasum -a 256 "$archive" | cut -d ' ' -f 1)
else
    actual=$(sha256sum "$archive" | cut -d ' ' -f 1)
fi
if [ "$actual" != "$DIGEST" ]; then
    echo "$archive: SHA-256 $actual, expected $DIGEST" >&2
    exit 1
fi

tar -xzf "$archive" -C "$dest"
echo "$dest/raylib-$VERSION"
