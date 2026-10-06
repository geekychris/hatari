#!/bin/sh
#
# Install Thorsten Otto's prebuilt m68k-atari-mintelf GCC cross toolchain
# (GCC + binutils + mintbin + mintlib + fdlibm + gemlib) into a user
# directory, no root needed.  https://tho-otto.de/crossmint.php
#
# Usage: fetch-cross-mint.sh [install-dir]   (default ~/computers/atari-cc)
# Then:  export PATH=<install-dir>/opt/cross-mint/bin:$PATH
#
# macOS binaries are x86_64 (run via Rosetta on Apple Silicon).

set -e

DEST=${1:-$HOME/computers/atari-cc}
URL=https://tho-otto.de/download/mint
case "$(uname -s)" in
	Darwin) HOST=macos ;;
	Linux)  HOST=linux64 ;;
	*) echo "unsupported host $(uname -s)" >&2; exit 1 ;;
esac

TOOLS="binutils-2.45-mintelf-20250812-bin-$HOST.tar.xz
gcc-14.3.0-mintelf-20250702-bin-$HOST.tar.xz
mintbin-0.4-mintelf-20230911-bin-$HOST.tar.xz"
LIBS="mintlib-0.60.1-mintelf-20240718-dev.tar.xz
fdlibm-20240425-mintelf-dev.tar.xz
gemlib-0.44.0-mintelf-20240425-dev.tar.xz"

mkdir -p "$DEST/dl" "$DEST/libtmp"
cd "$DEST"
for f in $TOOLS $LIBS; do
	[ -f "dl/$f" ] || { echo "Fetching $f"; curl -fsSL -o "dl/$f" "$URL/$f"; }
done
for f in $TOOLS; do
	tar xJf "dl/$f"
done
# libraries are packaged for /usr/m68k-atari-mintelf/sys-root, while
# the (relocatable) GCC looks for its sysroot next to its own install
for f in $LIBS; do
	tar xJf "dl/$f" -C libtmp
done
cp -R libtmp/usr/m68k-atari-mintelf/sys-root/ opt/cross-mint/m68k-atari-mintelf/sys-root/
rm -rf libtmp

GCC=$DEST/opt/cross-mint/bin/m68k-atari-mintelf-gcc
"$GCC" --version | head -1
echo "sysroot: $("$GCC" -print-sysroot)"
echo "Add to PATH: export PATH=$DEST/opt/cross-mint/bin:\$PATH"
