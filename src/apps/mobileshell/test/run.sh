#!/bin/sh
# Builds and runs the swipe decoder's test on the build host: the decoder is
# plain C++ and knows nothing of the system it types on.
#
#	src/apps/mobileshell/test/run.sh
set -e
dir=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/swipe-test.XXXXXX")
trap 'rm -rf "$out"' EXIT
${CXX:-c++} -std=c++17 -O2 -Wall -Wextra -Werror -o "$out/swipe-test" \
	"$dir/swipe-test.cpp" "$dir/../SwipeDecoder.cpp"
"$out/swipe-test"
