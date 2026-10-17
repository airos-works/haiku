#!/bin/bash
# Cross-build the stand-in NVENC add-on (fake_nvenc.cpp) for x86_64.
# usage: build-fake.sh <haiku build dir> <output file>
set -euo pipefail
BUILD=$(realpath "$1")
OUT=$(realpath -m "$2")
SRC=$(dirname "$(realpath "$0")")
HAIKU=$(realpath "$SRC/../../../../../..")
TOOLS=$BUILD/cross-tools-x86_64/bin/x86_64-unknown-haiku
if [ ! -x "$TOOLS-g++" ]; then
	TOOLS=$(sed -n 's/^HAIKU_CC_x86_64 ?= \(.*\)-gcc ;$/\1/p' "$BUILD/build/BuildConfig")
fi
OBJ=$BUILD/objects/haiku/x86_64/release
GCC_SYSLIBS=$(ls -d "$BUILD"/build_packages/gcc_syslibs_devel-*)
GCC_SYSLIBS_RUNTIME=$(ls -d "$BUILD"/build_packages/gcc_syslibs-*)
GCCLIB=$(dirname "$($TOOLS-gcc -print-libgcc-file-name)")
mkdir -p "$(dirname "$OUT")"
$TOOLS-g++ -O2 -fPIC -fvisibility=hidden -shared -nostdinc -nostdlib \
	-I"$GCC_SYSLIBS/develop/headers/c++" \
	-I"$GCC_SYSLIBS/develop/headers/c++/x86_64-unknown-haiku" \
	-I"$GCC_SYSLIBS/develop/headers/gcc/include" \
	-I"$GCC_SYSLIBS/develop/headers/gcc/include-fixed" \
	-I"$HAIKU/headers/private/media" -I"$HAIKU/headers/glibc" -I"$HAIKU/headers/posix" \
	-I"$HAIKU/headers" -I"$HAIKU/headers/os" -I"$HAIKU/headers/os/support" \
	-I"$HAIKU/headers/os/kernel" \
	-o "$OUT" \
	"$OBJ/system/glue/arch/x86_64/crti.o" "$GCCLIB/crtbeginS.o" \
	"$OBJ/system/glue/init_term_dyn.o" "$SRC/fake_nvenc.cpp" \
	"$OBJ/system/libroot/libroot.so" \
	"$GCC_SYSLIBS_RUNTIME/lib/libstdc++.so" "$GCC_SYSLIBS_RUNTIME/lib/libgcc_s.so" \
	"$GCCLIB/libgcc.a" "$GCCLIB/crtendS.o" "$OBJ/system/glue/arch/x86_64/crtn.o" \
	-Wl,--no-undefined
echo "built $OUT"
