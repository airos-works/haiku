#!/bin/bash
# Cross-build the NVENC encoder add-on for x86_64.
#
# usage: build-cross.sh <haiku build dir> <NVK tree> <output dir>
#        (OGKM_SRC=<open-gpu-kernel-modules checkout>)
#
# Like the NVDEC add-on (src/add-ons/media/plugins/nvdec/build-cross.sh):
# the NVK tree (airos-works/mesa-nvk) provides the resource manager API
# (src/nouveau/vulkan/nvkmd/nvrm) and NVIDIA's class and nvenc_drv.h
# headers; the RM headers come from NVIDIA's open-gpu-kernel-modules, which
# NVK expects in nvrm/open-gpu-kernel-modules unless OGKM_SRC names it.
# The add-on goes to <output dir>/add-ons/media/encoders/nvenc, where
# MediaConverter looks for it (it is no Media Kit add-on).
set -euo pipefail

BUILD=$(realpath "$1")
NVK=$(realpath "$2")
OUT=$(realpath -m "$3")
SRC=$(dirname "$(realpath "$0")")
HAIKU=$(realpath "$SRC/../../../../..")
NVRM=$NVK/src/nouveau/vulkan/nvkmd/nvrm
[ -f "$NVRM/nvRmApi.c" ] || { echo "no nvRmApi.c in $NVRM" >&2; exit 1; }
[ -f "$NVK/src/nouveau/headers/nvidia/video/nvenc_drv.h" ] \
	|| { echo "no nvenc_drv.h in $NVK" >&2; exit 1; }
O=$(realpath "${OGKM_SRC:-$NVRM/open-gpu-kernel-modules}")
[ -f "$O/src/nvidia/arch/nvalloc/unix/include/nv.h" ] || { echo "no RM headers in $O" >&2; exit 1; }

TOOLS=$BUILD/cross-tools-x86_64/bin/x86_64-unknown-haiku
if [ ! -x "$TOOLS-gcc" ]; then
	# a build directory configured with --cross-tools-prefix
	TOOLS=$(sed -n 's/^HAIKU_CC_x86_64 ?= \(.*\)-gcc ;$/\1/p' "$BUILD/build/BuildConfig")
fi
OBJ=$BUILD/objects/haiku/x86_64/release
GCC_SYSLIBS=$(ls -d "$BUILD"/build_packages/gcc_syslibs_devel-*)
GCCLIB=$(dirname "$($TOOLS-gcc -print-libgcc-file-name)")

INCLUDES=(-I"$SRC" -I"$NVRM" -I"$O/src/common/sdk/nvidia/inc"
	-I"$O/src/nvidia/arch/nvalloc/common/inc" -I"$O/src/nvidia/arch/nvalloc/unix/include"
	-I"$O/src/nvidia/inc/kernel" -I"$NVK/src/nouveau/headers/nvidia"
	-I"$GCC_SYSLIBS/develop/headers/gcc/include"
	-I"$GCC_SYSLIBS/develop/headers/gcc/include-fixed"
	-I"$HAIKU/headers/private/media" -I"$HAIKU/headers/private/shared"
	-I"$HAIKU/headers/compatibility/bsd" -I"$HAIKU/headers/glibc" -I"$HAIKU/headers/posix"
	-I"$HAIKU/headers" -I"$HAIKU/headers/os" -I"$HAIKU/headers/os/app"
	-I"$HAIKU/headers/os/drivers" -I"$HAIKU/headers/os/kernel"
	-I"$HAIKU/headers/os/storage" -I"$HAIKU/headers/os/support")
FLAGS=(-O2 -g -fPIC -fvisibility=hidden -nostdinc -std=gnu11 -D_DEFAULT_SOURCE
	-Wall -Wno-missing-braces "${INCLUDES[@]}")

OBJDIR=$OUT/.obj-nvenc
mkdir -p "$OBJDIR" "$OUT/add-ons/media/encoders"
pids=()
for f in nvenc_engine nvenc_common nvenc_h264 nvenc_hevc nvenc_addon; do
	$TOOLS-gcc "${FLAGS[@]}" -c "$SRC/$f.c" -o "$OBJDIR/$f.o" & pids+=($!)
done
$TOOLS-gcc "${FLAGS[@]}" -w -c "$NVRM/nvRmApi.c" -o "$OBJDIR/nvRmApi.o" & pids+=($!)
failed=0
for pid in "${pids[@]}"; do wait "$pid" || failed=1; done
[ $failed = 0 ] || { echo "compilation failed" >&2; exit 1; }

$TOOLS-gcc -shared -nostdlib -o "$OUT/add-ons/media/encoders/nvenc" \
	"$OBJ/system/glue/arch/x86_64/crti.o" "$GCCLIB/crtbeginS.o" \
	"$OBJ/system/glue/init_term_dyn.o" "$OBJDIR"/*.o \
	"$OBJ/system/libroot/libroot.so" \
	"$GCCLIB/libgcc.a" "$GCCLIB/crtendS.o" "$OBJ/system/glue/arch/x86_64/crtn.o" \
	-Wl,--no-undefined -Wl,-rpath-link,"$BUILD"
rm -rf "$OBJDIR"
echo "built nvenc into $OUT/add-ons/media/encoders"
