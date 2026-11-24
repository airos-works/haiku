#!/bin/bash
# Host test of xhci's copy_descriptor_data() under AddressSanitizer; no Haiku
# needed.
#
# usage: run.sh [git revision]
#   With a revision, the cases also run against the function of that
#   revision (one from before the copy took the received length), to show
#   which of them it fails.
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

extract() {	# the function, from "static size_t" to its closing brace
	awk '/^copy_descriptor_data\(/ { p = 1; print "static size_t" }
		p { print } p && /^}/ { exit }'
}

run() {		# run <label> [-DOLD], with xhci.cpp on stdin
	dir=$work/$(echo "$1" | tr -c 'A-Za-z0-9\n' _)
	mkdir -p "$dir" && extract > "$dir/copy_function.h"
	c++ -std=c++17 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
		$2 -I"$dir" -o "$dir/test" "$here/xhci_copy_test.cpp" || exit 2
	echo "== $1"
	ASAN_OPTIONS=detect_leaks=0 "$dir/test"
}

if [ -n "$1" ]; then
	(cd "$here" && git show "$1:./../xhci.cpp") | run "$1" -DOLD
fi
run "this tree" < "$here/../xhci.cpp"
