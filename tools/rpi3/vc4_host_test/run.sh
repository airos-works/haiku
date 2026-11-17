#!/bin/sh
# Builds and runs the host test of the vc4 driver's job and shader
# validation (src/add-ons/kernel/drivers/graphics/vc4) with the host's g++,
# AddressSanitizer and UBSan. See docs/rpi3/GPU.md.
set -e
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../../.." && pwd)
driver=$top/src/add-ons/kernel/drivers/graphics/vc4
out=${TMPDIR:-/tmp}/vc4_host_test.$$
mkdir -p "$out/graphics/vc4"
# the interface header, seen without the Haiku headers it includes
cp "$top/headers/private/graphics/vc4/vc4_drm.h" "$out/graphics/vc4/"
g++ -std=gnu++17 -g -O1 -Wall -Wno-unused-variable \
	-fsanitize=address,undefined -fno-sanitize-recover=undefined \
	-I "$here/stub" -I "$out" -I "$driver" \
	"$here/harness.cpp" "$driver/vc4_validate.cpp" \
	"$driver/vc4_validate_shaders.cpp" "$driver/vc4_render_cl.cpp" \
	"$driver/vc4_gem.cpp" -o "$out/harness"
ASAN_OPTIONS=detect_leaks=0 "$out/harness"
status=$?
rm -rf "$out"
exit $status
