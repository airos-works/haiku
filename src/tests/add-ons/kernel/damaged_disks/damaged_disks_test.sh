#!/bin/bash
# Damaged and crafted disk images against the file system code, on the build
# host (airos-works/haiku#165). Every image has one field that sends the
# driver outside the volume or outside a record; each used to end in a panic
# of the block cache, a division by zero, a wild copy or an endless loop.
#
#   damaged_disks_test.sh <build directory>
#
# needs in <build directory>/objects/linux/<arch>/release:
#   tools/bfs_shell/bfs_shell, tools/fat_shell/fat_shell
#       (jam "<build>bfs_shell" "<build>fat_shell")
#   tests/add-ons/kernel/file_systems/iso9660/iso9660_shell/iso9660_shell
#       (jam "<build>iso9660_shell"; left out when it is not there)
# and mkfs.vfat (dosfstools) and python3. The shells are the kernel add-ons'
# code with the FS shell's block cache, so this covers the file systems; the
# kernel's block cache and the GPT images (gpt-*.img) need a machine: plug an
# image in as a USB disk, e.g. in QEMU
#   (qemu) drive_add 0 if=none,id=d1,file=bfs-log-outside.img,format=raw
#   (qemu) device_add usb-storage,bus=xhci.0,drive=d1,id=s1,removable=on
# and look at the syslog: mount_server mounts what it can, and what it cannot
# gives a line there, not the kernel debugger.
set -u
BUILD=${1:?usage: damaged_disks_test.sh <build directory>}
HERE=$(dirname "$(readlink -f "$0")")
R=$(echo "$BUILD"/objects/linux/*/release)
BFS=$R/tools/bfs_shell/bfs_shell
FAT=$R/tools/fat_shell/fat_shell
ISO=$R/tests/add-ons/kernel/file_systems/iso9660/iso9660_shell/iso9660_shell
export LD_LIBRARY_PATH=$BUILD/objects/linux/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
W=$(mktemp -d "${TMPDIR:-/tmp}/damaged-disks.XXXXXX"); trap 'rm -rf "$W"' EXIT
failed=0

python3 "$HERE/make_damaged_disks.py" "$W" "$BFS" > "$W/list" || exit 1

# run SHELL IMAGE WANT COMMANDS...: WANT is "refused" (the mount must fail) or
# "mounted" (it may work, the commands must come back). Either way the shell
# must end by itself: no signal (a panic is abort() there), no timeout.
run() {
	local shell=$1 image=$2 want=$3; shift 3
	cp "$W/$image" "$W/work.img"
	local out status
	out=$({ printf '%s\n' "$@"; echo quit; } | timeout 60 "$shell" "$W/work.img" 2>&1)
	status=$?
	local what
	what=$(grep -F "$image" "$W/list" | sed 's/^[^ ]* *//')
	if [[ $status -ge 124 ]]; then
		echo "FAIL $image ($what): the shell ended with status $status"
		echo "$out" | tail -3 | cut -c1-200 | sed 's/^/       /'
		failed=1
	elif [[ $want == refused ]] && ! grep -q "Mounting FS failed" <<<"$out"; then
		echo "FAIL $image ($what): was mounted"
		failed=1
	else
		echo "ok   $image ($what)"
	fi
}

for image in bfs-log-outside bfs-log-empty bfs-log-past-end bfs-root-outside \
		bfs-dir-run-outside; do
	run "$BFS" $image.img refused "ls /myfs"
done
run "$BFS" bfs-good.img mounted "ls /myfs/dir"

run "$FAT" fat-root-cluster.img refused "ls /myfs"
run "$FAT" fat-entry-cluster.img mounted "ls /myfs" "ls /myfs/BADDIR" \
	"cat /myfs/BADFILE.TXT" "rm /myfs/BADFILE.TXT"
run "$FAT" fat-chain-cluster.img mounted "ls /myfs/CHAINDIR" \
	"cat /myfs/CHAIN.TXT" "touch /myfs/CHAINDIR/new" "mkdir /myfs/CHAINDIR/sub" \
	"cp /myfs/CHAIN.TXT /myfs/COPY.TXT" "rm /myfs/CHAIN.TXT"
run "$FAT" fat-good.img mounted "ls /myfs"

if [[ -x $ISO ]]; then
	run "$ISO" iso-name-short.img refused "ls /myfs"
	for image in iso-root-outside iso-dir-long iso-file-outside \
			iso-relocation-loop iso-relocation-outside iso-relocation-short \
			iso-link-long iso-attributes-short iso-name-twice \
			iso-record-past-block iso-good; do
		run "$ISO" $image.img mounted "ls /myfs" "cat /myfs/hello.txt" \
			"ls /myfs/deep" "cat /myfs/link"
	done
else
	echo "     (no iso9660_shell: the ISO9660 images were not tried)"
fi

[[ $failed == 0 ]] && echo PASS || echo FAILED
exit $failed
