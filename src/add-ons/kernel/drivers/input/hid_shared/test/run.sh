#!/bin/bash
# Host test of the shared HID report descriptor parser (HIDParser, HIDReport,
# HIDReportItem, HIDCollection): crafted and random descriptors and reports
# under AddressSanitizer; no Haiku needed.
#
# usage: run.sh [git revision]
#   With a revision, the same cases also run against the parser of that
#   revision, to show which of them it fails.
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../../../../../../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
files="HIDParser.cpp HIDParser.h HIDReport.cpp HIDReport.h HIDReportItem.cpp
	HIDReportItem.h HIDCollection.cpp HIDCollection.h HIDDataTypes.h"

build() {	# build <source dir> <output> <sanitizers>
	c++ -std=c++17 -g -O1 -Wall -Wno-unused-variable -Wno-unused-value \
		-Wno-unused-but-set-variable -fsanitize=$3 -fno-sanitize-recover=all -fno-omit-frame-pointer \
		-DUSERLAND_HID \
		-I"$here/stub" -I"$1" -I"$top/headers/private/drivers" \
		-I"$top/headers/private/kernel" -o "$2" "$here/hid_parser_test.cpp" \
		"$1/HIDParser.cpp" "$1/HIDReport.cpp" "$1/HIDReportItem.cpp" \
		"$1/HIDCollection.cpp"
}

run() {		# run <binary> <label>: every case in a process of its own
	failed=0
	for c in $("$1" list); do
		# the leaks of the other cases would hide what they are about
		leaks=0; [ "$c" = leak ] && leaks=1
		# (the memory limits are for the parser without limits: its
		# allocations fail at 1 GiB, and it is killed at 3)
		if ASAN_OPTIONS=detect_leaks=$leaks:allocator_may_return_null=1:soft_rss_limit_mb=1024:hard_rss_limit_mb=3072 \
			timeout 60 "$1" "$c" > "$work/out" 2>&1; then
			echo "$2: $c: ok"
			grep '^INFO' "$work/out" | sed -e 's/^INFO/   /'
		else
			failed=$((failed + 1))
			echo "$2: $c: FAILED ($(grep -m1 -E \
				'^FAIL|ERROR: [A-Za-z]*Sanitizer|runtime error' "$work/out" \
				| sed -e 's/^==[0-9]*==//' -e 's/ on address.*//' \
					-e 's/ (pc 0x.*//' | grep . || echo "no answer in 60 s"))"
		fi
	done
	return $failed
}

if [ -n "$1" ]; then
	mkdir "$work/old"
	for f in $files; do
		(cd "$top" && git show \
			"$1:src/add-ons/kernel/drivers/input/hid_shared/$f") \
			> "$work/old/$f" || exit 2
	done
	# (without the checks for undefined behaviour, which would stop it at
	# the shift in HIDReportItem's constructor, before the memory errors)
	build "$work/old" "$work/old-test" address || exit 2
	run "$work/old-test" "$1"
	echo "$1: $? cases failed"
fi

# (the scaling of values subtracts 32 bit signed ones that the device chose,
# and wraps)
build "$here/.." "$work/test" \
	"address,undefined -fno-sanitize=signed-integer-overflow" || exit 2
run "$work/test" "this tree"
result=$?
echo "this tree: $result cases failed"
exit $((result != 0))
