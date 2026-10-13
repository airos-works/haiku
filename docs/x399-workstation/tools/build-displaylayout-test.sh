#!/bin/bash
# Cross-build tests/displaylayouttest.cpp with app_server's DisplayLayout.cpp
# and run it on the workstation.
#
# usage: build-displaylayout-test.sh [--no-run]
#        (BUILD, HAIKU and TOOLS pick another build dir, source tree and
#        cross compiler prefix)
set -euo pipefail
X399=${X399:-/mnt/HaikuWork/x399}
BUILD=${BUILD:-$X399/build/x86_64}
HAIKU=${HAIKU:-$X399/haiku}
TESTS=$HAIKU/docs/x399-workstation/tests
OUT=$BUILD/displaylayouttest
SSH="ssh -F $X399/ssh/config -o ConnectTimeout=10 ws-haiku"

TOOLS=${TOOLS:-$BUILD/cross-tools-x86_64/bin/x86_64-unknown-haiku}
OBJ=$BUILD/objects/haiku/x86_64/release
GCC_SYSLIBS=$(ls -d "$BUILD"/build_packages/gcc_syslibs_devel-*)
GCC_SYSLIBS_RUNTIME=$(ls -d "$BUILD"/build_packages/gcc_syslibs-*)
GCCLIB=$(dirname "$($TOOLS-gcc -print-libgcc-file-name)")

# The stand-in HWInterface.h comes first; DisplayLayout.cpp finds nothing of
# that name beside itself.
INCLUDES=(-I"$TESTS/displaylayout" -I"$HAIKU/src/servers/app"
	-I"$GCC_SYSLIBS/develop/headers/c++"
	-I"$GCC_SYSLIBS/develop/headers/c++/x86_64-unknown-haiku"
	-I"$GCC_SYSLIBS/develop/headers/gcc/include"
	-I"$GCC_SYSLIBS/develop/headers/gcc/include-fixed"
	-I"$HAIKU/headers/glibc" -I"$HAIKU/headers/posix" -I"$HAIKU/headers"
	-I"$HAIKU/headers/os" -I"$HAIKU/headers/os/add-ons/graphics"
	-I"$HAIKU/headers/os/app" -I"$HAIKU/headers/os/interface"
	-I"$HAIKU/headers/os/kernel" -I"$HAIKU/headers/os/storage"
	-I"$HAIKU/headers/os/support" -I"$HAIKU/headers/private/graphics/common")

mkdir -p "$OUT"
for f in "$HAIKU/src/servers/app/DisplayLayout.cpp" "$TESTS/displaylayouttest.cpp"; do
	$TOOLS-g++ -O1 -g -nostdinc -Wall "${INCLUDES[@]}" -c "$f" \
		-o "$OUT/$(basename "$f" .cpp).o"
done
$TOOLS-g++ -nostdlib -o "$OUT/displaylayouttest" \
	"$OBJ/system/glue/arch/x86_64/crti.o" "$GCCLIB/crtbegin.o" \
	"$OBJ/system/glue/start_dyn.o" "$OBJ/system/glue/init_term_dyn.o" \
	"$OUT"/*.o "$OBJ/kits/libbe.so" "$OBJ/system/libroot/libroot.so" \
	"$GCC_SYSLIBS_RUNTIME/lib/libstdc++.so" "$GCC_SYSLIBS_RUNTIME/lib/libgcc_s.so" \
	"$GCCLIB/libgcc.a" "$GCCLIB/crtend.o" "$OBJ/system/glue/arch/x86_64/crtn.o" \
	-Wl,--no-undefined -Wl,-rpath-link,"$BUILD"
echo "built $OUT/displaylayouttest"

[ "${1:-}" = "--no-run" ] && exit 0
$SSH 'mkdir -p /boot/home/x399-tests'
$SSH 'cat > /boot/home/x399-tests/displaylayouttest && chmod +x /boot/home/x399-tests/displaylayouttest' \
	< "$OUT/displaylayouttest"
$SSH /boot/home/x399-tests/displaylayouttest
