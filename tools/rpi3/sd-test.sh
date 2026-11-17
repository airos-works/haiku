#!/usr/bin/env bash
# The SD card driver's test in QEMU's raspi3b (docs/rpi3/SD.md): boots the
# minimum image from a copy of its SD card, with a file of random data and a
# UserBootscript on the BFS partition. The script checks the file, copies it
# and reads the raw card; afterwards the results, the copy and the syslog
# are read back from the card's image and compared on the host.
#   sd-test.sh [seconds] [size in MiB]
#   RPI3_BUILD=<dir>    build directory (default /mnt/HaikuWork/rpi3/build)
#   RPI3_QEMU_TB=multi  QEMU's normal translation blocks (faster)
#   RPI3_SD_TEST_KEEP=<dir>  keep the serial log, the results and the syslog
set -euo pipefail
SECONDS_TO_RUN=${1:-600}
SIZE_MIB=${2:-16}
BUILD=${RPI3_BUILD:-/mnt/HaikuWork/rpi3/build}
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)
IMAGE=$BUILD/airos-rpi4-minimum.img
BFS_SHELL=$BUILD/objects/linux/x86_64/release/tools/bfs_shell/bfs_shell
WORK=$(mktemp -d "${TMPDIR:-/mnt/HaikuWork/tmp}/rpi3-sd-test.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

cp "$IMAGE" "$WORK/sd.img"
# QEMU wants a power of two
size=$(stat -c %s "$WORK/sd.img")
padded=1
while [ "$padded" -lt "$size" ]; do padded=$((padded * 2)); done
truncate -s "$padded" "$WORK/sd.img"

# the second partition is BFS
read -r start sectors < <(python3 -I -c '
import struct, sys
mbr = open(sys.argv[1], "rb").read(512)
entry = mbr[446 + 16:446 + 32]
print(*struct.unpack("<II", entry[8:16]))' "$WORK/sd.img")
bfs() {
	"$BFS_SHELL" --start-offset $((start * 512)) \
		--end-offset $(((start + sectors) * 512)) "$WORK/sd.img"
}

head -c $((SIZE_MIB * 1024 * 1024)) /dev/urandom > "$WORK/sdtest.bin"
expected=$(sha256sum "$WORK/sdtest.bin" | cut -d' ' -f1)
cat > "$WORK/UserBootscript" <<'EOF'
cd /boot/home
{
	echo "sdtest: start"
	sha256sum sdtest.bin
	echo "sdtest: raw read"
	dd if=/dev/disk/mmc/0/raw of=/dev/null bs=1M count=64
	echo "sdtest: copy"
	dd if=sdtest.bin of=sdtest.copy bs=1M
	sync
	echo "sdtest: done"
} > sdtest.result 2>&1
sync
EOF
{
	echo "cp :$WORK/sdtest.bin myfs/home/sdtest.bin"
	# the image's own is the empty template; bfs_shell does not overwrite
	echo "rm myfs/home/config/settings/boot/UserBootscript"
	echo "cp :$WORK/UserBootscript myfs/home/config/settings/boot/UserBootscript"
} | bfs >/dev/null

RPI3_BUILD=$BUILD RPI3_SD=$WORK/sd.img "$HERE/qemu-run.sh" "$SECONDS_TO_RUN" \
	> "$WORK/serial.log" 2>&1 || true
grep -a -E "sdhost|mmc_disk|PANIC|Welcome to syslog" "$WORK/serial.log" || true

echo "cat myfs/home/sdtest.result" | bfs > "$WORK/result.txt" 2>&1 || true
echo "--- sdtest.result"
cat "$WORK/result.txt"
echo "cp myfs/home/sdtest.copy :$WORK/sdtest.copy" | bfs >/dev/null 2>&1 || true
echo "cp myfs/system/var/log/syslog :$WORK/syslog" | bfs >/dev/null 2>&1 || true

status=0
if grep -q "$expected" "$WORK/result.txt"; then
	echo "read: sdtest.bin checksum matches ($expected)"
else
	echo "read: sdtest.bin checksum does not match ($expected expected)"
	status=1
fi
if [ -f "$WORK/sdtest.copy" ] && cmp -s "$WORK/sdtest.bin" "$WORK/sdtest.copy"
then
	echo "write: sdtest.copy is identical"
else
	echo "write: sdtest.copy is missing or differs"
	status=1
fi
if [ -s "$WORK/syslog" ]; then
	echo "syslog on the card: $(wc -l < "$WORK/syslog") lines, last:"
	tail -3 "$WORK/syslog"
fi
if [ -n "${RPI3_SD_TEST_KEEP:-}" ]; then
	mkdir -p "$RPI3_SD_TEST_KEEP"
	cp "$WORK/serial.log" "$WORK/result.txt" "$RPI3_SD_TEST_KEEP/"
	[ -f "$WORK/syslog" ] && cp "$WORK/syslog" "$RPI3_SD_TEST_KEEP/"
fi
exit $status
