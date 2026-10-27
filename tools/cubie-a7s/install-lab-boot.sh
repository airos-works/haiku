#!/usr/bin/env bash
# Install the lab boot chain on the Cubie A7S lab card (Radxa's Debian 11
# image, whose boot0 and U-Boot 2018 read every SD card) through its Debian:
#
#   boot.scr          lab/bsp-boot.cmd, run by U-Boot 2018: starts the air/OS
#                     U-Boot once when airos-once holds "1"
#   airos/u-boot.bin  the air/OS U-Boot (u-boot/build.sh), for UEFI
#   airos/chain.scr   lab/chain.cmd: boots EFI/airos/haiku_loader.efi
#
#   install-lab-boot.sh [u-boot.bin]
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
UBOOT=${1:-/mnt/HaikuWork/cubie/build-uboot/u-boot.bin}
source "$HERE/lab-esp.sh"

[[ -f $UBOOT ]] || { echo "missing $UBOOT" >&2; exit 1; }
WORK=$(mktemp -d "${TMPDIR:-/mnt/HaikuWork/tmp}/cubie-lab-boot.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

for script in bsp-boot chain; do
	mkimage -A arm64 -O linux -T script -C none -d "$HERE/lab/$script.cmd" \
		"$WORK/$script.scr" >/dev/null
done
lab_esp_put "$WORK/bsp-boot.scr" boot.scr
lab_esp_put "$WORK/chain.scr" airos/chain.scr
lab_esp_put "$UBOOT" airos/u-boot.bin
echo "installed boot.scr, airos/chain.scr and airos/u-boot.bin"
