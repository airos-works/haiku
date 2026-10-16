#!/bin/bash
# Cross-build a small test program from this directory against a build dir's
# libraries:  build-test.sh <name> [extra libs...]  ->  $OUT/<name>
# BUILD (default /mnt/HaikuWork/build/fracscale-x86_64) and OUT can be set.
set -euo pipefail
NAME=$1; shift
HERE=$(cd "$(dirname "$0")" && pwd)
HAIKU=$(cd "$HERE/../../.." && pwd)
BUILD=${BUILD:-/mnt/HaikuWork/build/fracscale-x86_64}
OUT=${OUT:-$BUILD/scaling-tests}
TOOLS=/mnt/HaikuWork/x399/build/x86_64/cross-tools-x86_64/bin/x86_64-unknown-haiku
OBJ=$BUILD/objects/haiku/x86_64/release
GCC_SYSLIBS=$(ls -d "$BUILD"/build_packages/gcc_syslibs_devel-*)
GCC_SYSLIBS_RUNTIME=$(ls -d "$BUILD"/build_packages/gcc_syslibs-*)
GCCLIB=$(dirname "$($TOOLS-gcc -print-libgcc-file-name)")
INCLUDES=(-I"$GCC_SYSLIBS/develop/headers/c++"
	-I"$GCC_SYSLIBS/develop/headers/c++/x86_64-unknown-haiku"
	-I"$GCC_SYSLIBS/develop/headers/gcc/include"
	-I"$GCC_SYSLIBS/develop/headers/gcc/include-fixed"
	-I"$HAIKU/headers/glibc" -I"$HAIKU/headers/posix" -I"$HAIKU/headers"
	-I"$HAIKU/headers/os" -I"$HAIKU/headers/os/game" -I"$HAIKU/headers/os/add-ons/graphics"
	-I"$HAIKU/headers/os/app" -I"$HAIKU/headers/os/interface"
	-I"$HAIKU/headers/os/kernel" -I"$HAIKU/headers/os/storage"
	-I"$HAIKU/headers/os/support" -I"$HAIKU/headers/os/translation"
	-I"$HAIKU/headers/private/interface" -I"$HAIKU/headers/private/shared")
LIBS=("$OBJ/kits/libbe.so")
for l in "$@"; do
	case $l in
		game) LIBS+=("$OBJ/kits/game/libgame.so") ;;
		translation) LIBS+=("$OBJ/kits/translation/libtranslation.so") ;;
		*) LIBS+=("$l") ;;
	esac
done
mkdir -p "$OUT"
$TOOLS-g++ -O2 -g -nostdinc -Wall "${INCLUDES[@]}" -c "$HERE/$NAME.cpp" -o "$OUT/$NAME.o"
$TOOLS-g++ -nostdlib -o "$OUT/$NAME" \
	"$OBJ/system/glue/arch/x86_64/crti.o" "$GCCLIB/crtbegin.o" \
	"$OBJ/system/glue/start_dyn.o" "$OBJ/system/glue/init_term_dyn.o" \
	"$OUT/$NAME.o" "${LIBS[@]}" "$OBJ/system/libroot/libroot.so" \
	"$GCC_SYSLIBS_RUNTIME/lib/libstdc++.so" "$GCC_SYSLIBS_RUNTIME/lib/libgcc_s.so" \
	"$GCCLIB/libgcc.a" "$GCCLIB/crtend.o" "$OBJ/system/glue/arch/x86_64/crtn.o" \
	-Wl,--no-undefined -Wl,-rpath-link,"$BUILD"
echo "$OUT/$NAME"
