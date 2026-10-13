#!/bin/sh
# usbdiskcheck.sh <raw device> <MiB offset>: write 256 MiB of random data at
# the offset (destroys what is there), read it back with three block sizes
# and compare checksums, then time 1 GiB sequential reads and writes.
D=$1
OFF=${2:-32768}
F=/boot/home/x399-tests/rand256.bin
[ -f $F ] || dd if=/dev/urandom of=$F bs=1048576 count=256 2>/dev/null
echo "source:   $(sha256sum < $F | cut -c1-16)"
dd if=$F of=$D bs=1048576 seek=$OFF conv=fsync 2>&1 | tail -1
echo "read 1M:  $(dd if=$D bs=1048576 skip=$OFF count=256 2>/dev/null | sha256sum | cut -c1-16)"
echo "read 64K: $(dd if=$D bs=65536 skip=$((OFF * 16)) count=4096 2>/dev/null | sha256sum | cut -c1-16)"
echo "read 4K:  $(dd if=$D bs=4096 skip=$((OFF * 256)) count=65536 2>/dev/null | sha256sum | cut -c1-16)"
echo "sequential read 1 GiB:"
dd if=$D of=/dev/null bs=1048576 count=1024 skip=4096 2>&1 | tail -1
echo "sequential write 1 GiB:"
dd if=/dev/zero of=$D bs=1048576 count=1024 seek=40960 conv=fsync 2>&1 | tail -1
