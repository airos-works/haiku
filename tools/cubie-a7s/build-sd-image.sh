#!/usr/bin/env bash
# Build the air/OS SD card image for the Radxa Cubie A7S:
#
#   0-16 MiB  GPT and Radxa's boot chain (fetch-bootloader.sh): boot0 at
#             128 KiB, TF-A, SCP firmware and U-Boot 2018 at 12 MiB
#   p1        ESP: boot.scr (sd-boot.cmd), which U-Boot 2018 runs to start
#             the air/OS U-Boot (airos/u-boot.bin, u-boot/build.sh); that one
#             boots EFI/BOOT/BOOTAA64.EFI (Haiku's loader) with
#             dtb/allwinner/sun60i-a733-cubie-a7s.dtb (build-dtb.sh)
#   p2        Haiku's BFS volume
#
#   build-sd-image.sh <bfs image> <output image> [haiku_loader.efi]
#
# UBOOT_BIN overrides the air/OS U-Boot (default: build it).
#
# Flash the result to a card (dd, Etcher). No root needed.
set -euo pipefail

BFS_IMAGE=$1
OUTPUT=$2
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BUILD=${CUBIE_BUILD:-/mnt/HaikuWork/cubie/build}
LOADER=${3:-$BUILD/objects/haiku/arm64/release/system/boot/efi/haiku_loader.efi}
FAT_SHELL=${FAT_SHELL:-$BUILD/objects/linux/x86_64/release/tools/fat_shell/fat_shell}
ESP_MIB=${ESP_MIB:-64}

UBOOT_BIN=${UBOOT_BIN:-}

for tool in sgdisk mkimage; do
	command -v $tool >/dev/null || { echo "missing $tool" >&2; exit 1; }
done
[[ -x $FAT_SHELL ]] || { echo "missing fat_shell ($FAT_SHELL)" >&2; exit 1; }
[[ -f $BFS_IMAGE && -f $LOADER ]] || { echo "missing $BFS_IMAGE or $LOADER" >&2; exit 1; }

WORK=$(mktemp -d "${TMPDIR:-/mnt/HaikuWork/tmp}/cubie-sd.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

BOOTLOADER=$("$HERE/fetch-bootloader.sh")
if [[ -z $UBOOT_BIN ]]; then
	UBOOT_BIN=$("$HERE/u-boot/build.sh")
fi
"$HERE/build-dtb.sh" "$WORK/cubie-a7s.dtb" >/dev/null
mkimage -A arm64 -O linux -T script -C none -d "$HERE/sd-boot.cmd" \
	"$WORK/boot.scr" >/dev/null

echo "ESP"
truncate -s "${ESP_MIB}M" "$WORK/esp.img"
"$FAT_SHELL" --initialize "$WORK/esp.img" 'airOS ESP' >/dev/null
fat() { echo "$1" | "$FAT_SHELL" "$WORK/esp.img" >/dev/null; }
fat "cp :$WORK/boot.scr myfs/boot.scr"
fat "mkdir myfs/airos"
fat "cp :$UBOOT_BIN myfs/airos/u-boot.bin"
fat "mkdir myfs/EFI"
fat "mkdir myfs/EFI/BOOT"
fat "cp :$LOADER myfs/EFI/BOOT/BOOTAA64.EFI"
fat "mkdir myfs/dtb"
fat "mkdir myfs/dtb/allwinner"
fat "cp :$WORK/cubie-a7s.dtb myfs/dtb/allwinner/sun60i-a733-cubie-a7s.dtb"

echo "Assembling $OUTPUT"
espStart=32768							# 16 MiB
espCount=$((ESP_MIB * 2048))
bfsStart=$((espStart + espCount))
bfsCount=$((($(stat -c %s "$BFS_IMAGE") + 511) / 512))
total=$(((bfsStart + bfsCount + 33 + 2047) / 2048 * 2048))
rm -f "$OUTPUT"
truncate -s $((total * 512)) "$OUTPUT"
sgdisk -o \
	-n 1:$espStart:+$espCount -t 1:EF00 -c 1:esp -A 1:set:2 \
	-n 2:$bfsStart:+$bfsCount -t 2:42465331-3BA3-10F1-802A-4861696B7521 \
		-c 2:airOS \
	"$OUTPUT" >/dev/null
dd if="$BOOTLOADER/boot0_sdcard.bin" of="$OUTPUT" bs=512 seek=256 conv=notrunc \
	status=none
dd if="$BOOTLOADER/boot0_ufs.bin" of="$OUTPUT" bs=512 seek=2064 conv=notrunc \
	status=none
dd if="$BOOTLOADER/boot_package.fex" of="$OUTPUT" bs=512 seek=24576 \
	conv=notrunc status=none
dd if="$WORK/esp.img" of="$OUTPUT" bs=512 seek=$espStart conv=notrunc,sparse \
	status=none
dd if="$BFS_IMAGE" of="$OUTPUT" bs=512 seek=$bfsStart conv=notrunc,sparse \
	status=none
sgdisk -v "$OUTPUT" | grep -q "No problems found" \
	|| { sgdisk -v "$OUTPUT"; exit 1; }
sgdisk -p "$OUTPUT" | tail -3
echo "$OUTPUT: $(($(stat -c %s "$OUTPUT") / 1048576)) MiB"
