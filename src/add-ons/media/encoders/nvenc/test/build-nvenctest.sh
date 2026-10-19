#!/bin/bash
# Cross-build nvenctest (nvenctest.c), which encodes test pictures with the
# nvenc add-on on the card and writes the stream, for x86_64.
# usage: build-nvenctest.sh <haiku build dir> <output file>
set -euo pipefail
BUILD=$(realpath "$1")
OUT=$(realpath -m "$2")
SRC=$(dirname "$(realpath "$0")")
HAIKU=$(realpath "$SRC/../../../../../..")
TOOLS=$BUILD/cross-tools-x86_64/bin/x86_64-unknown-haiku
if [ ! -x "$TOOLS-gcc" ]; then
	TOOLS=$(sed -n 's/^HAIKU_CC_x86_64 ?= \(.*\)-gcc ;$/\1/p' "$BUILD/build/BuildConfig")
fi
OBJ=$BUILD/objects/haiku/x86_64/release
GCC_SYSLIBS=$(ls -d "$BUILD"/build_packages/gcc_syslibs_devel-*)
GCCLIB=$(dirname "$($TOOLS-gcc -print-libgcc-file-name)")
mkdir -p "$(dirname "$OUT")"
$TOOLS-gcc -O2 -std=gnu11 -Wall -nostdinc -nostdlib \
	-I"$GCC_SYSLIBS/develop/headers/gcc/include" \
	-I"$GCC_SYSLIBS/develop/headers/gcc/include-fixed" \
	-I"$HAIKU/headers/private/media" -I"$HAIKU/headers/glibc" -I"$HAIKU/headers/posix" \
	-I"$HAIKU/headers" -I"$HAIKU/headers/os" -I"$HAIKU/headers/os/support" \
	-I"$HAIKU/headers/os/kernel" -I"$HAIKU/headers/os/storage" \
	-o "$OUT" \
	"$OBJ/system/glue/arch/x86_64/crti.o" "$GCCLIB/crtbegin.o" \
	"$OBJ/system/glue/start_dyn.o" "$OBJ/system/glue/init_term_dyn.o" \
	"$SRC/nvenctest.c" "$OBJ/system/libroot/libroot.so" \
	"$GCCLIB/libgcc.a" "$GCCLIB/crtend.o" "$OBJ/system/glue/arch/x86_64/crtn.o" \
	-Wl,--no-undefined
echo "built $OUT"
