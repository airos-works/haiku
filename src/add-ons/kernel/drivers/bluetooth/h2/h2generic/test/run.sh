#!/bin/bash
# Host test of h2generic's device_control() (the Bluetooth USB driver's
# ioctl): the function is cut out of h2generic.cpp and built against
# stand-ins for the kernel and the rest of the driver; no Haiku needed.
#
# usage: run.sh [git revision]
#   With a revision, the function of that revision is tested instead (to see
#   what the test catches).
set -e
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../../../../../../../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

source=$here/../h2generic.cpp
if [ -n "$1" ]; then
	(cd "$top" && git show \
		"$1:src/add-ons/kernel/drivers/bluetooth/h2/h2generic/h2generic.cpp") \
		> "$work/h2generic.cpp"
	source=$work/h2generic.cpp
fi

# from the comment in front of device_control() to the one in front of
# device_read()
sed -n '/^\/\/ implements the POSIX ioctl()/,/^\/\/ implements the POSIX read()/p' \
	"$source" > "$work/device_control.inc"
grep -q '^device_control(' "$work/device_control.inc"

c++ -std=c++17 -g -O0 -Wall -Wno-unused-function -Wno-unused-variable \
	-fsanitize=address,undefined \
	-DDEVICE_CONTROL_SOURCE="\"$work/device_control.inc\"" \
	-o "$work/test" "$here/device_control_test.cpp"
"$work/test"
