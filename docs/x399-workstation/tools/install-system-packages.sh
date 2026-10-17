#!/bin/bash
# Installs a build's system packages on the X399 in place of the installed
# ones, and its EFI boot loader on the ESP; takes effect at the next (cold)
# boot. The packages keep the installed file names: the package daemon leaves
# a same-name swap alone, and the running system keeps the files it has open.
#
#   install-system-packages.sh [BUILD_DIR]
#
# BUILD_DIR is a configured x86_64 build directory whose packages are built
# (default /mnt/HaikuWork/build/master-x86_64):
#   jam -q haiku.hpkg haiku_loader.hpkg haiku_datatranslators.hpkg \
#       haiku_devel.hpkg smbfs.hpkg userland_fs.hpkg
# WebPositive is no longer built (2026-10-17, Summit replaces it): remove an
# installed webpositive package with `pkgman uninstall webpositive` once
# haiku_devel matches the haiku package again (before that, pkgman's solver
# also takes away the mismatched haiku_devel).
# The replaced packages and BOOTX64.EFI are kept in
# /boot/home/x399-backup/system-<date>; the boot menu's "last state" goes
# back to them too. Power cycle afterwards (never a warm reboot):
#   flock /mnt/HaikuWork/x399/state/lab.lock /mnt/HaikuWork/x399/tools/power-cycle.sh
set -euo pipefail
X399=${X399:-/mnt/HaikuWork/x399}
BUILD=${1:-/mnt/HaikuWork/build/master-x86_64}
SSH="ssh -F $X399/ssh/config -o ConnectTimeout=10 ws-haiku"
PACKAGES="haiku haiku_loader haiku_datatranslators haiku_devel smbfs userland_fs"
DIR=$BUILD/objects/haiku/x86_64/packaging/packages
LOADER=$BUILD/objects/haiku/x86_64/release/system/boot/efi/haiku_loader.efi
ESP_DEVICE=/dev/disk/nvme/0/0
stamp=$(date +%Y%m%d-%H%M%S)
STAGE=/boot/home/x399-stage/system-$stamp
BACKUP=/boot/home/x399-backup/system-$stamp

[ -f "$LOADER" ] || LOADER=$(find "$BUILD/objects/haiku/x86_64" -name haiku_loader.efi | head -1)
[ -f "$LOADER" ] || { echo "no haiku_loader.efi in $BUILD" >&2; exit 1; }
for p in $PACKAGES; do
	[ -f "$DIR/$p.hpkg" ] || { echo "$DIR/$p.hpkg is not built" >&2; exit 1; }
done

upload() {
	$SSH "cat > $STAGE/$2" < "$1"
	local remote
	remote=$($SSH "sha256sum $STAGE/$2" | cut -d' ' -f1)
	[ "$remote" = "$(sha256sum "$1" | cut -d' ' -f1)" ] \
		|| { echo "$2 was damaged on the way" >&2; exit 1; }
}

$SSH "mkdir -p $STAGE $BACKUP"
for p in $PACKAGES; do
	upload "$DIR/$p.hpkg" $p.hpkg
done
upload "$LOADER" haiku_loader.efi
echo "staged in $STAGE"

$SSH "set -e
	cd /boot/system/packages
	for p in $PACKAGES; do
		old=\$(ls | grep \"^\$p-[r0-9]\" | head -1)
		[ -n \"\$old\" ] || { echo \"no installed \$p package\" >&2; exit 1; }
		cp \$old $BACKUP/
		cp $STAGE/\$p.hpkg \$old.new
	done
	sync
	for p in $PACKAGES; do
		old=\$(ls | grep \"^\$p-[r0-9]\" | grep -v '\\.new\$' | head -1)
		mv -f \$old.new \$old
		echo \"\$p -> \$old\"
	done
	sync

	# The loader on the ESP. Its FAT read-only attribute lets cp truncate the
	# file and then fails the write: make it writable first, and read the
	# result back after mounting again.
	M=/boot/home/esp-mnt
	mkdir -p \$M
	[ -d \$M/EFI ] || mount -t fat $ESP_DEVICE \$M
	E=\$M/EFI/BOOT/BOOTX64.EFI
	[ -f \$E ]
	cp \$E $BACKUP/BOOTX64.EFI
	chmod u+w \$E
	cp $STAGE/haiku_loader.efi \$E
	sync
	unmount \$M
	mount -t fat $ESP_DEVICE \$M
	a=\$(sha256sum \$E | cut -d' ' -f1)
	b=\$(sha256sum $STAGE/haiku_loader.efi | cut -d' ' -f1)
	unmount \$M
	[ \"\$a\" = \"\$b\" ] || { echo \"BOOTX64.EFI does not match; the old one is $BACKUP/BOOTX64.EFI\" >&2; exit 1; }
	echo \"BOOTX64.EFI updated\"
	sync"
echo "installed; previous packages and loader in $BACKUP"
