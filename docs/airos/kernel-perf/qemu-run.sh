#!/bin/bash
# run.sh <name>: boots a copy of the image from an emulated NVMe disk with two
# scratch NVMe disks, 8 vCPUs, 2 GiB; serial log to serial-<name>.log
set -euo pipefail
Q=/data1/scratch/kperf/out
N=$1
# disk images in RAM: no host disk in the measurements
D=/dev/shm/kperf
mkdir -p $D
cp --sparse=always ${IMAGE:-/data1/scratch/io-build/haiku-nightly.image} $D/boot-$N.img
rm -f $D/raw-$N.img $D/fs-$N.img
truncate -s 1G $D/raw-$N.img
truncate -s 2G $D/fs-$N.img
# SCRIPT=file: a 1 MiB disk with that script, which iotest.sh runs instead
SCRIPT_ARGS=""
if [ -n "${SCRIPT:-}" ]; then
	rm -f $D/script-$N.img
	truncate -s 1M $D/script-$N.img
	dd if="$SCRIPT" of=$D/script-$N.img conv=notrunc status=none
	SCRIPT_ARGS="-drive file=$D/script-$N.img,format=raw,if=none,id=script -device nvme,serial=script01,drive=script"
fi
timeout ${TIMEOUT:-900} qemu-system-x86_64 -enable-kvm -cpu host -smp ${SMP:-8} -m ${MEM:-2048} \
	-drive file=$D/boot-$N.img,format=raw,if=none,id=boot \
	-device nvme,serial=boot0001,drive=boot,bootindex=0 \
	-drive file=$D/raw-$N.img,format=raw,if=none,id=raw \
	-device nvme,serial=raw00001,drive=raw \
	-drive file=$D/fs-$N.img,format=raw,if=none,id=fs \
	-device nvme,serial=fs000001,drive=fs \
	$SCRIPT_ARGS \
	-display none -serial file:$Q/serial-$N.log -monitor ${MONITOR:-none} ${EXTRA:-} \
	-net none -no-reboot || true
grep -a "KP\|IOT_BEGIN\|IOT_END\|PANIC\|panic" $Q/serial-$N.log | sed "s/.*\(KP.*\|IOT_.*\)/\1/" > $Q/result-$N.txt || true
rm -f $D/boot-$N.img $D/raw-$N.img $D/fs-$N.img $D/script-$N.img
cat $Q/result-$N.txt
