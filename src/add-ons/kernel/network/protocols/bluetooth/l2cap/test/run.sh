#!/bin/bash
# Host test of L2CAP's l2cap_receive_data(): the function is cut out of
# l2cap.cpp and built against stand-ins for the network stack, the HCI layer
# and the endpoints; no Haiku and no Bluetooth controller needed.
#
# usage: run.sh [git revision]
#   With a revision, the function of that revision is tested instead (to see
#   what the test catches).
set -e
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../../../../../../../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

source=$here/../l2cap.cpp
if [ -n "$1" ]; then
	(cd "$top" && git show \
		"$1:src/add-ons/kernel/network/protocols/bluetooth/l2cap/l2cap.cpp") \
		> "$work/l2cap.cpp"
	source=$work/l2cap.cpp
fi

# connection_for() and l2cap_receive_data(): from the line in front of the
# first to the line in front of "status_t l2cap_error_received("
first=$(grep -n '^connection_for(' "$source" | cut -d: -f1)
next=$(grep -n '^l2cap_error_received(' "$source" | cut -d: -f1)
sed -n "$((first - 1)),$((next - 2))p" "$source" > "$work/receive_data.inc"
grep -q '^l2cap_receive_data(' "$work/receive_data.inc"

c++ -std=c++17 -g -O0 -Wall -Wno-unused-function -Wno-unused-variable \
	-fsanitize=address,undefined \
	-DRECEIVE_DATA_SOURCE="\"$work/receive_data.inc\"" \
	-o "$work/test" "$here/receive_data_test.cpp"
ASAN_OPTIONS=detect_leaks=0 "$work/test"
