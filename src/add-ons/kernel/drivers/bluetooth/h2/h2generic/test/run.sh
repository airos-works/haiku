#!/bin/bash
# Host tests of h2generic (the Bluetooth USB driver): its device_control()
# (the ioctl) and its receive completions are cut out of h2generic.cpp and
# h2transactions.cpp and built against stand-ins for the kernel, the USB
# stack and the rest of the driver; no Haiku needed.
#
# usage: run.sh [git revision]
#   With a revision, the code of that revision is tested instead (to see
#   what the test catches).
set -e
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../../../../../../../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

source=$here/../h2generic.cpp
transactions=$here/../h2transactions.cpp
if [ -n "$1" ]; then
	(cd "$top" && git show \
		"$1:src/add-ons/kernel/drivers/bluetooth/h2/h2generic/h2generic.cpp") \
		> "$work/h2generic.cpp"
	(cd "$top" && git show \
		"$1:src/add-ons/kernel/drivers/bluetooth/h2/h2generic/h2transactions.cpp") \
		> "$work/h2transactions.cpp"
	source=$work/h2generic.cpp
	transactions=$work/h2transactions.cpp
fi

# from the comment in front of device_control() to the one in front of
# device_read()
sed -n '/^\/\/ implements the POSIX ioctl()/,/^\/\/ implements the POSIX read()/p' \
	"$source" > "$work/device_control.inc"
grep -q '^device_control(' "$work/device_control.inc"

# the receive completions: from "RX Complete" to sco_rx_complete()
sed -n '/^#pragma mark --- RX Complete ---/,/^sco_rx_complete(/p' \
	"$transactions" | sed "1,2d" | sed "\$d" | sed "\$d" > "$work/receive.inc"
grep -q '^event_complete(' "$work/receive.inc"

flags="-std=c++17 -g -O0 -Wall -Wno-unused-function -Wno-unused-variable
	-fsanitize=address,undefined"
c++ $flags -DDEVICE_CONTROL_SOURCE="\"$work/device_control.inc\"" \
	-o "$work/test" "$here/device_control_test.cpp"
c++ $flags -DRECEIVE_SOURCE="\"$work/receive.inc\"" \
	-o "$work/receive_test" "$here/receive_test.cpp"
status=0
"$work/test" || status=1
"$work/receive_test" || status=1
exit $status
