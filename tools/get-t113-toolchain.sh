#!/bin/sh
# Download the compiler for the T113 build (t113.toolchain): Bootlin's
# armv7-eabihf musl toolchain, pinned to one release and checked against its
# SHA-256.
#
#   tools/get-t113-toolchain.sh [install folder]
#
# The install folder defaults to ~/x-tools, which is where t113.toolchain
# looks. Anywhere else needs -DT113_TOOLCHAIN_DIR=<folder>/<name> (or the
# T113_TOOLCHAIN_DIR environment variable) when configuring.
#
# Needs: curl, sha256sum, tar with xz support. Runs on x86-64 Linux (or WSL).
set -eu

NAME=armv7-eabihf--musl--stable-2026.08-1
SHA256=9147bafae4aa272321a3c6440d04d83b7e23411b2d344f875541d84c4444ba9b
URL=https://toolchains.bootlin.com/downloads/releases/toolchains/armv7-eabihf/tarballs/$NAME.tar.xz

DEST=${1:-$HOME/x-tools}

if [ -x "$DEST/$NAME/bin/arm-buildroot-linux-musleabihf-gcc" ]; then
	echo "Already installed: $DEST/$NAME"
	exit 0
fi

mkdir -p "$DEST"
cd "$DEST"

echo "Downloading $NAME (about 80 MB)..."
curl -fL --retry 3 -o "$NAME.tar.xz" "$URL"
echo "$SHA256  $NAME.tar.xz" | sha256sum -c -

tar xf "$NAME.tar.xz"
rm "$NAME.tar.xz"

# The toolchain contains absolute paths from Bootlin's build machine; this
# rewrites them to where it was unpacked. Its "can't read" messages are for
# files Bootlin leaves out of the download, and are harmless.
(cd "$NAME" && ./relocate-sdk.sh) >/dev/null 2>&1

"$NAME/bin/arm-buildroot-linux-musleabihf-gcc" --version | head -n 1
echo "Installed: $DEST/$NAME"
