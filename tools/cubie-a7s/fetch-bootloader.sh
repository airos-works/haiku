#!/usr/bin/env bash
# Fetch the Cubie A7S SD card boot loader: Radxa's BSP boot chain from the
# u-boot-aw2501 package (the Cubie A7A files, which Radxa's A7S images use):
#
#   boot0_sdcard.bin   Allwinner boot0 (DRAM setup), at sector 256 (128 KiB)
#   boot0_ufs.bin      boot0 for UFS boot, at sector 2064
#   boot_package.fex   TF-A, SCP firmware and U-Boot 2018, at sector 24576
#
# The newer u-boot-dlan17 boot loader (mainline-style SPL, U-Boot 2026.04)
# could not read SD cards that boot0 reads, so air/OS starts its own U-Boot
# from U-Boot 2018 instead (u-boot/build.sh).
#
#   fetch-bootloader.sh [directory]   (default /mnt/HaikuWork/cubie/bootloader)
# Prints the directory holding the three files.
set -euo pipefail
VERSION=2018.07-17
SHA256=84cd88576d0a0e1f1347cbf473251ee1fd51164e44f2c3a1f8e57fbf4a72ba89
DIR=${1:-/mnt/HaikuWork/cubie/bootloader}/aw2501-$VERSION
FILES=(boot0_sdcard.bin boot0_ufs.bin boot_package.fex)

if [[ -f $DIR/SHA256SUMS ]] && (cd "$DIR" && sha256sum -c --quiet SHA256SUMS 2>/dev/null); then
	echo "$DIR"
	exit 0
fi
mkdir -p "$DIR"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/cubie-bootloader.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
curl -sSfL -o "$WORK/u-boot.deb" \
	"https://github.com/radxa-pkg/u-boot-aw2501/releases/download/$VERSION/u-boot-aw2501_${VERSION}_all.deb"
echo "$SHA256  $WORK/u-boot.deb" | sha256sum -c --quiet
(cd "$WORK" && ar x u-boot.deb && tar xf data.tar.*)
for file in "${FILES[@]}"; do
	cp "$WORK/usr/lib/u-boot/radxa-cubie-a7a/$file" "$DIR/$file"
done
(cd "$DIR" && sha256sum "${FILES[@]}" > SHA256SUMS)
echo "$DIR"
