#!/usr/bin/env bash
# QEMU smoke test of a Cubie A7S build before a hardware iteration: packs the
# BFS image into an SD image (build-sd-image.sh) and boots it on QEMU's virt
# machine through Debian's u-boot-qemu, as the board's U-Boot would (EFI).
#
#   qemu-smoke.sh [bfs image] [seconds]
#
# Passes when the serial log shows userland starting.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BUILD=${CUBIE_BUILD:-/mnt/HaikuWork/cubie/build}
IMAGE=${1:-$BUILD/haiku-cubie-minimum.image}
SECONDS_TO_RUN=${2:-120}
QEMU=${QEMU:-/mnt/HaikuWork/build/qemu-10.2.0/qemu-system-aarch64}
UBOOT=${QEMU_UBOOT:-/mnt/HaikuWork/cubie/downloads/u-boot-qemu/x/usr/lib/u-boot/qemu_arm64/u-boot.bin}
MARKER=${MARKER:-'launch_daemon|app_server|net_server'}

WORK=$(mktemp -d "${TMPDIR:-/mnt/HaikuWork/tmp}/cubie-qemu.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
"$HERE/build-sd-image.sh" "$IMAGE" "$WORK/sd.img" >/dev/null
LOG=$BUILD/qemu-smoke.log
"$QEMU" -M virt,gic-version=3 -cpu cortex-a76 -smp 4 -m 4096 -bios "$UBOOT" \
	-drive if=none,file="$WORK/sd.img",format=raw,id=hd0 \
	-device virtio-blk-device,drive=hd0 -nographic -serial mon:stdio \
	-device virtio-net-device,netdev=n0 -netdev user,id=n0 \
	</dev/null > "$LOG" 2>&1 &
pid=$!
deadline=$((SECONDS + SECONDS_TO_RUN))
result=1
while (( SECONDS < deadline )) && kill -0 $pid 2>/dev/null; do
	if grep -a -q -E "$MARKER" "$LOG"; then
		result=0
		break
	fi
	if grep -a -q -E "PANIC|Kernel Debugging Land" "$LOG"; then
		break
	fi
	sleep 2
done
kill $pid 2>/dev/null || true
wait $pid 2>/dev/null || true
if (( result == 0 )); then
	echo "QEMU smoke test passed ($(grep -a -m1 -o -E "$MARKER" "$LOG"))"
else
	echo "QEMU smoke test FAILED, see $LOG" >&2
	grep -a -E "PANIC|panic" "$LOG" | head -5 >&2 || true
fi
exit $result
