#!/bin/bash
# A disk image the Pi 5's EEPROM boots from the NanoKVM's virtual USB disk:
# partition 1 FAT32 with config.txt, the loader, the boot archive and the
# BCM2712 device trees; optionally partition 2 = a BFS volume image.
#
#   make-boot-disk.sh <output.img> <build dir> [bfs image] [cmdline words]
#
# The build dir is a configured Haiku build with haiku_loader.rpi and
# airos-boot.tgz built. Firmware files come from HAIKU_RPI_FIRMWARE_DIR
# (default /mnt/HaikuWork/rpi5/firmware, tools/rpi4/fetch-firmware.sh).
set -euo pipefail
out=$1; build=$2; bfs=${3:-}; words=${4:-airos.debug}
top=$(cd "$(dirname "$0")/../.." && pwd)
firmware=${HAIKU_RPI_FIRMWARE_DIR:-/mnt/HaikuWork/rpi5/firmware}
PATH=$PATH:/mnt/HaikuWork/toolchains/host/usr/bin:/usr/sbin:/sbin
loader=$(find "$build/objects" -name haiku_loader.rpi -path '*release*' | head -1)
[ -f "$loader" ] || { echo "no haiku_loader.rpi in $build" >&2; exit 1; }

fat_mib=64
bfs_mib=0
[ -n "$bfs" ] && bfs_mib=$(( ($(stat -c %s "$bfs") + 1048575) / 1048576 ))
total_mib=$(( 1 + fat_mib + bfs_mib + 1 ))
rm -f "$out"
truncate -s ${total_mib}M "$out"
{
	echo "label: dos"
	echo "start=2048, size=$((fat_mib * 2048)), type=c, bootable"
	if [ -n "$bfs" ]; then echo "start=$(( (1 + fat_mib) * 2048 )), size=$((bfs_mib * 2048)), type=eb"; fi
} | sfdisk -q "$out"
mkfs.vfat -F 32 --offset 2048 -n AIROSBOOT "$out" $((fat_mib * 1024)) >/dev/null
m() { mcopy -o -i "$out@@1048576" "$@"; }
# The lab's supply is a 3 A USB-C one: without this the EEPROM refuses to
# boot from USB at all.
{ cat "$top/data/boot/rpi/config.txt"; printf '\n# lab boot disk\n[all]\nusb_max_current_enable=1\n'; } > "$build/config.txt.tmp"
m "$build/config.txt.tmp" ::/config.txt; rm "$build/config.txt.tmp"
m "$top/data/boot/rpi/airos-boot.txt" ::/
m "$loader" ::/airos-loader.img
m "$build/airos-boot.tgz" ::/airos-boot.tgz
printf '%s\n' "$words" > "$build/cmdline.txt.tmp"; m "$build/cmdline.txt.tmp" ::/cmdline.txt; rm "$build/cmdline.txt.tmp"
for dtb in "$firmware"/bcm2712*.dtb "$firmware"/bcm2711*.dtb; do m "$dtb" ::/; done
if [ -n "$bfs" ]; then dd if="$bfs" of="$out" bs=1M seek=$((1 + fat_mib)) conv=notrunc status=none; fi
mdir -i "$out@@1048576" ::/ | tail -n +4
