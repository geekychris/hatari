#!/bin/sh
#
# Download the free (GPL) EmuTOS ROM images into a ROM directory
# usable with Hatari's --agent-rom-dir option / agent API /roms endpoint.
#
# Usage: fetch-emutos.sh [rom-dir] [emutos-version]
#
# Original Atari TOS images are copyrighted; if you own them,
# copy them into the same directory and they will be listed too.

set -e

ROMDIR=${1:-roms}
VERSION=${2:-1.4}
URL=https://sourceforge.net/projects/emutos/files/emutos/$VERSION

mkdir -p "$ROMDIR/emutos"
cd "$ROMDIR/emutos"

for size in 192k 256k 512k 1024k; do
	zip=emutos-$size-$VERSION.zip
	echo "Fetching $zip..."
	curl -fsSL -o "$zip" "$URL/$zip/download"
	unzip -qo "$zip"
	rm -f "$zip"
done

echo
echo "EmuTOS $VERSION images in $ROMDIR/emutos:"
find . -name '*.img' | wc -l | sed 's/^ */  /;s/$/ images/'
echo
echo "Which image to use:"
echo "  192k   ST / Mega ST (TOS 1.x address range), one language per image"
echo "  256k   STE / Mega STE (TOS 2.x range), one language per image"
echo "  512k   STE / TT / Falcon (Hatari also accepts it for ST), one language per image"
echo "  1024k  all machines, all languages in one image"
