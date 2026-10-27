#!/usr/bin/env bash
# Build the Cubie A7S lab SD card image from Radxa's Debian 11 A733 image
# (radxa-a733_bullseye_cli_r6), whose boot chain (boot0, TF-A, U-Boot 2018)
# reads every SD card; the boot loader of the newer Debian 13 images (t6,
# mainline-style SPL) failed on the lab's cards.
#
#   0-16 MiB  Radxa's boot chain
#   p1        rsetup "config" FAT partition (first-boot configuration)
#   p2        ESP: boot.scr (lab/bsp-boot.cmd) for U-Boot 2018, which starts
#             the air/OS U-Boot (airos/u-boot.bin) once when airos-once holds
#             "1"; its airos/chain.scr (lab/chain.cmd) boots
#             EFI/airos/haiku_loader.efi with airos/cubie-a7s.dtb
#   p3        Debian recovery system (Radxa's rootfs), ssh with the lab key,
#             hostname cubie-recovery
#   p4        Haiku BFS
#
#   build-lab-image.sh <radxa image> <output image> [haiku_loader.efi] [bfs image]
#
# UBOOT_BIN overrides the air/OS U-Boot (default: u-boot/build.sh).
#
# Needs no root: partitions are edited as files with debugfs, resize2fs and
# Haiku's fat_shell. Credentials come from $CUBIE_STATE (never committed):
# lab_key.pub (authorized for root) and lab_root_password.hash (crypt(3)).
set -euo pipefail

RADXA_IMAGE=$1
OUTPUT=$2
LOADER=${3:-}
BFS_IMAGE=${4:-}
CUBIE_STATE=${CUBIE_STATE:-/mnt/HaikuWork/cubie/state}
FAT_SHELL=${FAT_SHELL:-/mnt/HaikuWork/cubie/build/objects/linux/x86_64/release/tools/fat_shell/fat_shell}
ROOTFS_MIB=${ROOTFS_MIB:-}
BFS_MIB=${BFS_MIB:-3072}
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

[[ -x $FAT_SHELL ]] || FAT_SHELL=/mnt/HaikuWork/build/arm64/objects/linux/x86_64/release/tools/fat_shell/fat_shell
for tool in debugfs e2fsck resize2fs sgdisk mkimage; do
	command -v $tool >/dev/null || { echo "missing $tool" >&2; exit 1; }
done

WORK=$(mktemp -d "${TMPDIR:-/mnt/HaikuWork/tmp}/cubie-lab.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

sector_of() { # <image> <partition number> -> "start count"
	sgdisk -i "$2" "$1" | awk '/^First sector/ {s=$3} /^Last sector/ {e=$3} END {print s, e - s + 1}'
}

echo "Extracting partitions"
dd if="$RADXA_IMAGE" of="$WORK/boot.bin" bs=1M count=16 status=none
# Radxa's GPT goes; sgdisk writes a new one
dd if=/dev/zero of="$WORK/boot.bin" bs=512 count=34 conv=notrunc status=none
for n in 1 2 3; do
	read -r start count < <(sector_of "$RADXA_IMAGE" $n)
	dd if="$RADXA_IMAGE" of="$WORK/p$n.img" bs=512 skip="$start" count="$count" \
		conv=sparse status=none
done

# resize2fs refuses to shrink Radxa's root file system much (it was
# created for 10 GiB and shrunk once already), so by default it keeps its
# size: 16 + 16 + 300 + 3718 + 3072 MiB still fits an 8 GB card.
if [[ -n $ROOTFS_MIB ]]; then
	echo "Shrinking the Debian root file system to $ROOTFS_MIB MiB"
	e2fsck -fy "$WORK/p3.img" >/dev/null 2>&1 || [[ $? -le 1 ]]
	resize2fs "$WORK/p3.img" "${ROOTFS_MIB}M" >/dev/null
	truncate -s "${ROOTFS_MIB}M" "$WORK/p3.img"
else
	ROOTFS_MIB=$((($(stat -c %s "$WORK/p3.img") + 1048575) / 1048576))
	truncate -s "${ROOTFS_MIB}M" "$WORK/p3.img"
fi

echo "Configuring the recovery system"
dfs() { debugfs -w -R "$1" "$WORK/p3.img" 2>&1 | grep -v '^debugfs ' || true; }
put() { # <local file> <path> <mode>
	dfs "rm $2" >/dev/null
	dfs "write $1 $2" >/dev/null
	dfs "sif $2 mode 0100$3"
	dfs "sif $2 uid 0"
	dfs "sif $2 gid 0"
}
echo cubie-recovery > "$WORK/hostname"
put "$WORK/hostname" /etc/hostname 644
debugfs -R "cat /etc/hosts" "$WORK/p3.img" 2>/dev/null > "$WORK/hosts"
grep -q cubie-recovery "$WORK/hosts" || printf '127.0.1.1\tcubie-recovery\n' >> "$WORK/hosts"
put "$WORK/hosts" /etc/hosts 644

dfs "mkdir /root/.ssh"
dfs "sif /root/.ssh mode 040700"
put "$CUBIE_STATE/lab_key.pub" /root/.ssh/authorized_keys 600
printf 'PermitRootLogin prohibit-password\n' > "$WORK/airos-lab.conf"
put "$WORK/airos-lab.conf" /etc/ssh/sshd_config.d/airos-lab.conf 644

debugfs -R "cat /etc/shadow" "$WORK/p3.img" 2>/dev/null > "$WORK/shadow"
hash=$(cat "$CUBIE_STATE/lab_root_password.hash")
awk -F: -v OFS=: -v h="$hash" '$1 == "root" {$2 = h} {print}' "$WORK/shadow" > "$WORK/shadow.new"
put "$WORK/shadow.new" /etc/shadow 640
dfs "sif /etc/shadow gid 42"

# Kernel messages on the serial console, no splash.
debugfs -R "cat /boot/extlinux/extlinux.conf" "$WORK/p3.img" 2>/dev/null \
	| sed -e 's/ quiet splash / /' -e 's/ console=tty1 / console=tty1 console=ttyAS0,115200n8 /' \
	> "$WORK/extlinux.conf"
put "$WORK/extlinux.conf" /boot/extlinux/extlinux.conf 644

# Partition 4 belongs to Haiku: a partition after the root file system keeps
# growroot from taking the rest of the card.
e2fsck -fy "$WORK/p3.img" >/dev/null 2>&1 || [[ $? -le 1 ]]

echo "First-boot configuration (ssh on)"
mkdir -p "$WORK/cfg"
(cd "$WORK/cfg" && 7z x -y "$WORK/p1.img" before.txt >/dev/null)
sed -i -e '/^disable_service ssh/d' -e 's/^if headless enable_service ssh/enable_service ssh/' \
	"$WORK/cfg/before.txt"
echo "rm myfs/before.txt" | "$FAT_SHELL" "$WORK/p1.img" >/dev/null
echo "cp :$WORK/cfg/before.txt myfs/before.txt" | "$FAT_SHELL" "$WORK/p1.img" >/dev/null

echo "ESP: lab boot chain"
UBOOT_BIN=${UBOOT_BIN:-$("$HERE/u-boot/build.sh")}
"$HERE/build-dtb.sh" "$WORK/cubie-a7s.dtb" >/dev/null
for script in bsp-boot chain; do
	mkimage -A arm64 -O linux -T script -C none -d "$HERE/lab/$script.cmd" \
		"$WORK/$script.scr" >/dev/null
done
fat() { echo "$1" | "$FAT_SHELL" "$WORK/p2.img" >/dev/null; }
fat "cp :$WORK/bsp-boot.scr myfs/boot.scr"
fat "mkdir myfs/airos"
fat "cp :$WORK/chain.scr myfs/airos/chain.scr"
fat "cp :$UBOOT_BIN myfs/airos/u-boot.bin"
fat "cp :$WORK/cubie-a7s.dtb myfs/airos/cubie-a7s.dtb"
fat "mkdir myfs/EFI"
fat "mkdir myfs/EFI/airos"
if [[ -n $LOADER ]]; then
	fat "cp :$LOADER myfs/EFI/airos/haiku_loader.efi"
fi

echo "Assembling $OUTPUT"
read -r p1start p1count < <(sector_of "$RADXA_IMAGE" 1)
read -r p2start p2count < <(sector_of "$RADXA_IMAGE" 2)
p3start=$((p2start + p2count))
p3count=$((ROOTFS_MIB * 2048))
p4start=$((p3start + p3count))
p4count=$((BFS_MIB * 2048))
total=$(((p4start + p4count + 33 + 2047) / 2048 * 2048))
rm -f "$OUTPUT"
truncate -s $((total * 512)) "$OUTPUT"
dd if="$WORK/boot.bin" of="$OUTPUT" bs=1M conv=notrunc,sparse status=none
sgdisk -o "$OUTPUT" >/dev/null
sgdisk -n 1:$p1start:+$p1count -t 1:8300 -c 1:config \
	-n 2:$p2start:+$p2count -t 2:EF00 -c 2:efi -A 2:set:2 \
	-n 3:$p3start:+$p3count -t 3:8300 -c 3:rootfs -A 3:set:2 \
	-n 4:$p4start:+$p4count -t 4:42465331-3BA3-10F1-802A-4861696B7521 -c 4:haiku \
	"$OUTPUT" >/dev/null
# boot0 starts at 128 KiB, so the first 16 MiB stay Radxa's apart from the
# GPT itself.
for n in 1 2 3; do
	start=$(eval echo \$p${n}start)
	dd if="$WORK/p$n.img" of="$OUTPUT" bs=512 seek="$start" conv=notrunc,sparse status=none
done
if [[ -n $BFS_IMAGE ]]; then
	dd if="$BFS_IMAGE" of="$OUTPUT" bs=512 seek="$p4start" conv=notrunc,sparse status=none
fi
sgdisk -v "$OUTPUT" | tail -1
sgdisk -p "$OUTPUT" | tail -5
