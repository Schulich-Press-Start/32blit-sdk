#!/bin/sh
# Cross-build libdrm for the T113 build (t113.toolchain) and install it into
# the toolchain's sysroot, so CMake finds it like any other target library.
# Static only, with every GPU-specific part turned off: the port only needs
# libdrm's core mode-setting API.
#
#   tools/build-libdrm.sh [toolchain folder]
#
# The toolchain folder defaults to $T113_TOOLCHAIN_DIR, otherwise the place
# tools/get-t113-toolchain.sh installs to. Run that script first.
#
# Needs: curl, sha256sum, tar with xz support, python3 with venv (for meson
# and ninja, installed into a private folder under ~/.cache). Runs on x86-64
# Linux (or WSL).
#
# PC (x86) builds don't use this: install the distro's package instead
# (Debian/Ubuntu: sudo apt install libdrm-dev).
set -eu

VERSION=2.4.134
# Checked against the release's GPG signature (Simon Ser, libdrm maintainer) when pinned.
SHA256=ac5e74d157830eb8bee44c6a6bf3ad49774ef0dd2a72bdad74a8f20308b52a95
URL=https://dri.freedesktop.org/libdrm/libdrm-$VERSION.tar.xz

TOOLCHAIN=${1:-${T113_TOOLCHAIN_DIR:-$HOME/x-tools/armv7-eabihf--musl--stable-2026.08-1}}
TARGET=arm-buildroot-linux-musleabihf
SYSROOT=$TOOLCHAIN/$TARGET/sysroot

if [ ! -x "$TOOLCHAIN/bin/$TARGET-gcc" ]; then
	echo "T113 toolchain not found in $TOOLCHAIN (run tools/get-t113-toolchain.sh first)" >&2
	exit 1
fi

if grep -qx "Version: $VERSION" "$SYSROOT/usr/lib/pkgconfig/libdrm.pc" 2>/dev/null; then
	echo "Already installed: libdrm $VERSION in $SYSROOT"
	exit 0
fi

# meson and ninja, kept out of the system Python
VENV=${XDG_CACHE_HOME:-$HOME/.cache}/sps-meson
if [ ! -x "$VENV/bin/meson" ]; then
	python3 -m venv "$VENV"
	"$VENV/bin/pip" install -q "meson>=1.4,<2" ninja
fi
PATH=$VENV/bin:$PATH

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

echo "Downloading libdrm $VERSION..."
curl -fL --retry 3 -o "$WORK/libdrm.tar.xz" "$URL"
echo "$SHA256  $WORK/libdrm.tar.xz" | sha256sum -c -
tar -C "$WORK" -xf "$WORK/libdrm.tar.xz"

cat > "$WORK/cross.ini" <<EOF
[binaries]
c = '$TOOLCHAIN/bin/$TARGET-gcc'
ar = '$TOOLCHAIN/bin/$TARGET-ar'
strip = '$TOOLCHAIN/bin/$TARGET-strip'

[built-in options]
c_args = ['-mcpu=cortex-a7', '-mfpu=neon-vfpv4', '-mfloat-abi=hard']

[host_machine]
system = 'linux'
cpu_family = 'arm'
cpu = 'cortex-a7'
endian = 'little'
EOF

meson setup "$WORK/build" "$WORK/libdrm-$VERSION" \
	--cross-file "$WORK/cross.ini" \
	--prefix=/usr --libdir=lib --buildtype=release \
	-Ddefault_library=static \
	-Dintel=disabled -Dradeon=disabled -Damdgpu=disabled -Dnouveau=disabled \
	-Dvmwgfx=disabled -Domap=disabled -Dexynos=disabled -Dfreedreno=disabled \
	-Dtegra=disabled -Dvc4=disabled -Detnaviv=disabled \
	-Dcairo-tests=disabled -Dman-pages=disabled -Dvalgrind=disabled \
	-Dfreedreno-kgsl=false -Dinstall-test-programs=false -Dudev=false -Dtests=false \
	> "$WORK/setup.log" || { cat "$WORK/setup.log" >&2; exit 1; }

ninja -C "$WORK/build" > /dev/null
DESTDIR=$SYSROOT meson install -C "$WORK/build" --no-rebuild --quiet

echo "Installed: libdrm $VERSION into $SYSROOT/usr"
