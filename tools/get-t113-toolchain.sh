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
# Needs: curl, sha256sum (or shasum), tar with xz support. Runs on x86-64
# Linux (or WSL) and on macOS. Bootlin's compilers only run on x86-64 Linux,
# so on macOS t113.toolchain uses Homebrew's clang and lld with this
# download's sysroot and libraries: brew install llvm lld.
set -eu

NAME=armv7-eabihf--musl--stable-2026.08-1
SHA256=9147bafae4aa272321a3c6440d04d83b7e23411b2d344f875541d84c4444ba9b
URL=https://toolchains.bootlin.com/downloads/releases/toolchains/armv7-eabihf/tarballs/$NAME.tar.xz

DEST=${1:-$HOME/x-tools}
HOST=$(uname -s)

if [ "$HOST" = Darwin ]; then
	LLVM=${T113_LLVM_DIR:-$(brew --prefix llvm 2>/dev/null || true)}
	LLD=$LLVM/bin/ld.lld
	[ -x "$LLD" ] || LLD=$(brew --prefix lld 2>/dev/null || true)/bin/ld.lld
	if [ ! -x "$LLVM/bin/clang" ] || [ ! -x "$LLD" ]; then
		echo "T113 builds on macOS need Homebrew's LLVM and lld: brew install llvm lld" >&2
		exit 1
	fi
fi

if [ -x "$DEST/$NAME/bin/arm-buildroot-linux-musleabihf-gcc" ]; then
	echo "Already installed: $DEST/$NAME"
	exit 0
fi

mkdir -p "$DEST"
cd "$DEST"

echo "Downloading $NAME (about 80 MB)..."
curl -fL --retry 3 -o "$NAME.tar.xz" "$URL"
if command -v sha256sum >/dev/null; then
	echo "$SHA256  $NAME.tar.xz" | sha256sum -c -
else
	echo "$SHA256  $NAME.tar.xz" | shasum -a 256 -c -
fi

if [ "$HOST" = Darwin ]; then
	# terminfo has names that differ only in case, which macOS's
	# case-insensitive disks can't hold; games don't use it
	tar xf "$NAME.tar.xz" --exclude '*/usr/share/terminfo/*'
else
	tar xf "$NAME.tar.xz"
fi
rm "$NAME.tar.xz"

if [ "$HOST" = Darwin ]; then
	echo "Installed: $DEST/$NAME (compiled with $LLVM/bin/clang on macOS)"
	exit 0
fi

# The toolchain contains absolute paths from Bootlin's build machine; this
# rewrites them to where it was unpacked. Its "can't read" messages are for
# files Bootlin leaves out of the download, and are harmless.
(cd "$NAME" && ./relocate-sdk.sh) >/dev/null 2>&1

"$NAME/bin/arm-buildroot-linux-musleabihf-gcc" --version | head -n 1
echo "Installed: $DEST/$NAME"
