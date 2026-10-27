#!/bin/bash
# Cross-build nvidia_rm.accelerant for x86_64.
#
# usage: build-cross.sh <haiku build dir> <nvidia_rm work dir> <output dir>
#        (TOOLS=<cross compiler prefix> when the build dir has none of its own)
#
# The work dir is the one used by the kernel driver's build-cross.sh; it
# provides NVIDIA's open-gpu-kernel-modules headers (or OGKM_SRC does, as for
# the driver).
set -euo pipefail

BUILD=$(realpath "$1")
WORK=$(realpath "$2")
OUT=$(realpath -m "$3")
SRC=$(dirname "$(realpath "$0")")
HAIKU=$(realpath "$SRC/../../../..")
OGKM=${OGKM_SRC:-$WORK/open-gpu-kernel-modules}
[ -d "$OGKM/src" ] || { echo "run the nvidia_rm driver build first" >&2; exit 1; }

TOOLS=${TOOLS:-$BUILD/cross-tools-x86_64/bin/x86_64-unknown-haiku}
OBJ=$BUILD/objects/haiku/x86_64/release
GCC_SYSLIBS=$(ls -d "$BUILD"/build_packages/gcc_syslibs_devel-*)
GCC_SYSLIBS_RUNTIME=$(ls -d "$BUILD"/build_packages/gcc_syslibs-*)
GCCLIB=$(dirname "$($TOOLS-gcc -print-libgcc-file-name)")

INCLUDES=(-I"$SRC" -I"$SRC/sdk" -I"$HAIKU/src/add-ons/kernel/drivers/graphics/nvidia_rm/sdk"
	-I"$OGKM/src/common/sdk/nvidia/inc" -I"$OGKM/src/common/unix/common/inc"
	-I"$OGKM/src/common/inc" -I"$OGKM/src/nvidia/arch/nvalloc/common/inc"
	-I"$OGKM/src/nvidia/arch/nvalloc/unix/include" -I"$OGKM/src/nvidia/inc/kernel"
	-I"$OGKM/src/nvidia/interface" -I"$OGKM/src/nvidia-modeset/interface"
	-I"$OGKM/src/nvidia-modeset/os-interface/include"
	-I"$OGKM/src/nvidia-modeset/kapi/interface"
	-I"$GCC_SYSLIBS/develop/headers/c++"
	-I"$GCC_SYSLIBS/develop/headers/c++/x86_64-unknown-haiku"
	-I"$GCC_SYSLIBS/develop/headers/gcc/include"
	-I"$GCC_SYSLIBS/develop/headers/gcc/include-fixed"
	-I"$HAIKU/headers/glibc" -I"$HAIKU/headers/posix" -I"$HAIKU/headers"
	-I"$HAIKU/headers/os" -I"$HAIKU/headers/os/add-ons/graphics"
	-I"$HAIKU/headers/os/app" -I"$HAIKU/headers/os/drivers"
	-I"$HAIKU/headers/os/interface" -I"$HAIKU/headers/os/kernel"
	-I"$HAIKU/headers/os/storage" -I"$HAIKU/headers/os/support"
	-I"$HAIKU/headers/private/graphics/common"
	-I"$HAIKU/headers/private/shared")
FLAGS=(-O2 -fpic -nostdinc -D_DEFAULT_SOURCE -DNV_PLATFORM_MAX_IOCTL_SIZE=16384
	-Wno-missing-field-initializers "${INCLUDES[@]}")

OBJDIR=$WORK/accelerant-obj
mkdir -p "$OBJDIR" "$OUT/add-ons/accelerants"
pids=()
for f in Accelerant NvUtils NvKmsBitmap sdk/ErrorUtils sdk/NvRmApi sdk/NvRmDevice \
		sdk/NvKmsApi sdk/NvKmsDevice sdk/NvKmsSurface; do
	$TOOLS-g++ -std=c++20 "${FLAGS[@]}" -c "$SRC/$f.cpp" \
		-o "$OBJDIR/$(basename $f).o" & pids+=($!)
done
for f in "$SRC/common/decode_edid.c" "$OGKM/src/common/shared/nvstatus/nvstatus.c" \
		"$OGKM/src/nvidia-modeset/lib/nvkms-format.c" \
		"$OGKM/src/nvidia-modeset/lib/nvkms-sync.c"; do
	$TOOLS-gcc "${FLAGS[@]}" -c "$f" -o "$OBJDIR/$(basename "$f" .c).o" & pids+=($!)
done
for pid in "${pids[@]}"; do wait "$pid"; done

$TOOLS-g++ -shared -nostdlib -o "$OUT/add-ons/accelerants/nvidia_rm.accelerant" \
	"$OBJ/system/glue/arch/x86_64/crti.o" "$GCCLIB/crtbeginS.o" \
	"$OBJ/system/glue/init_term_dyn.o" "$OBJDIR"/*.o \
	"$OBJ/kits/libbe.so" "$OBJ/system/libroot/libroot.so" \
	"$GCC_SYSLIBS_RUNTIME/lib/libstdc++.so" "$GCC_SYSLIBS_RUNTIME/lib/libgcc_s.so" \
	"$GCCLIB/libgcc.a" \
	"$GCCLIB/crtendS.o" "$OBJ/system/glue/arch/x86_64/crtn.o" \
	-Wl,--no-undefined -Wl,-rpath-link,"$BUILD"
echo "built nvidia_rm.accelerant into $OUT"

# Diagnostic tool: dumps NVKMS's view of every dpy (see DpyInfoTest.cpp).
TOOLDIR=$OBJDIR/nvdpyinfo
mkdir -p "$TOOLDIR" "$OUT/bin"
for f in DpyInfoTest sdk/ErrorUtils sdk/NvKmsApi sdk/NvKmsDevice sdk/NvRmApi sdk/NvRmDevice; do
	$TOOLS-g++ -std=c++20 "${FLAGS[@]}" -c "$SRC/$f.cpp" -o "$TOOLDIR/$(basename $f).o"
done
$TOOLS-gcc "${FLAGS[@]}" -c "$OGKM/src/common/shared/nvstatus/nvstatus.c" \
	-o "$TOOLDIR/nvstatus.o"
$TOOLS-g++ -nostdlib -o "$OUT/bin/nvdpyinfo" \
	"$OBJ/system/glue/arch/x86_64/crti.o" "$GCCLIB/crtbegin.o" \
	"$OBJ/system/glue/start_dyn.o" "$OBJ/system/glue/init_term_dyn.o" \
	"$TOOLDIR"/*.o \
	"$OBJ/system/libroot/libroot.so" "$GCC_SYSLIBS_RUNTIME/lib/libstdc++.so" \
	"$GCC_SYSLIBS_RUNTIME/lib/libgcc_s.so" "$GCCLIB/libgcc.a" \
	"$GCCLIB/crtend.o" "$OBJ/system/glue/arch/x86_64/crtn.o" \
	-Wl,--no-undefined -Wl,-rpath-link,"$BUILD"
echo "built nvdpyinfo into $OUT/bin"

# Diagnostic tool: draws into the screen's frame buffer (see ScanoutTest.cpp).
TOOLDIR=$OBJDIR/nvscanout
mkdir -p "$TOOLDIR"
for f in ScanoutTest sdk/ErrorUtils sdk/NvRmApi sdk/NvRmDevice; do
	$TOOLS-g++ -std=c++20 "${FLAGS[@]}" -c "$SRC/$f.cpp" -o "$TOOLDIR/$(basename $f).o"
done
$TOOLS-gcc "${FLAGS[@]}" -c "$OGKM/src/common/shared/nvstatus/nvstatus.c" \
	-o "$TOOLDIR/nvstatus.o"
$TOOLS-g++ -nostdlib -o "$OUT/bin/nvscanout" \
	"$OBJ/system/glue/arch/x86_64/crti.o" "$GCCLIB/crtbegin.o" \
	"$OBJ/system/glue/start_dyn.o" "$OBJ/system/glue/init_term_dyn.o" \
	"$TOOLDIR"/*.o \
	"$OBJ/system/libroot/libroot.so" "$GCC_SYSLIBS_RUNTIME/lib/libstdc++.so" \
	"$GCC_SYSLIBS_RUNTIME/lib/libgcc_s.so" "$GCCLIB/libgcc.a" \
	"$GCCLIB/crtend.o" "$OBJ/system/glue/arch/x86_64/crtn.o" \
	-Wl,--no-undefined -Wl,-rpath-link,"$BUILD"
echo "built nvscanout into $OUT/bin"

# Diagnostic tool: counts the display's vertical blanks (see VblankTest.cpp).
TOOLDIR=$OBJDIR/nvvblank
mkdir -p "$TOOLDIR"
for f in VblankTest NvUtils sdk/ErrorUtils sdk/NvRmApi sdk/NvRmDevice sdk/NvKmsApi sdk/NvKmsDevice sdk/NvKmsSurface; do
	$TOOLS-g++ -std=c++20 "${FLAGS[@]}" -c "$SRC/$f.cpp" -o "$TOOLDIR/$(basename $f).o"
done
$TOOLS-gcc "${FLAGS[@]}" -c "$OGKM/src/common/shared/nvstatus/nvstatus.c" \
	-o "$TOOLDIR/nvstatus.o"
$TOOLS-g++ -nostdlib -o "$OUT/bin/nvvblank" \
	"$OBJ/system/glue/arch/x86_64/crti.o" "$GCCLIB/crtbegin.o" \
	"$OBJ/system/glue/start_dyn.o" "$OBJ/system/glue/init_term_dyn.o" \
	"$TOOLDIR"/*.o \
	"$OBJ/system/libroot/libroot.so" "$GCC_SYSLIBS_RUNTIME/lib/libstdc++.so" \
	"$GCC_SYSLIBS_RUNTIME/lib/libgcc_s.so" "$GCCLIB/libgcc.a" \
	"$GCCLIB/crtend.o" "$OBJ/system/glue/arch/x86_64/crtn.o" \
	-Wl,--no-undefined -Wl,-rpath-link,"$BUILD"
echo "built nvvblank into $OUT/bin"

# Diagnostic tool: shows a square on the hardware cursor plane and reads the
# display's CRCs (see CursorTest.cpp).
TOOLDIR=$OBJDIR/nvcursortest
mkdir -p "$TOOLDIR"
for f in CursorTest NvKmsBitmap NvUtils sdk/ErrorUtils sdk/NvRmApi sdk/NvRmDevice sdk/NvKmsApi sdk/NvKmsDevice sdk/NvKmsSurface; do
	$TOOLS-g++ -std=c++20 "${FLAGS[@]}" -c "$SRC/$f.cpp" -o "$TOOLDIR/$(basename $f).o"
done
$TOOLS-gcc "${FLAGS[@]}" -c "$OGKM/src/common/shared/nvstatus/nvstatus.c" \
	-o "$TOOLDIR/nvstatus.o"
$TOOLS-g++ -nostdlib -o "$OUT/bin/nvcursortest" \
	"$OBJ/system/glue/arch/x86_64/crti.o" "$GCCLIB/crtbegin.o" \
	"$OBJ/system/glue/start_dyn.o" "$OBJ/system/glue/init_term_dyn.o" \
	"$TOOLDIR"/*.o \
	"$OBJ/system/libroot/libroot.so" "$OBJ/kits/libbe.so" \
	"$GCC_SYSLIBS_RUNTIME/lib/libstdc++.so" \
	"$GCC_SYSLIBS_RUNTIME/lib/libgcc_s.so" "$GCCLIB/libgcc.a" \
	"$GCCLIB/crtend.o" "$OBJ/system/glue/arch/x86_64/crtn.o" \
	-Wl,--no-undefined -Wl,-rpath-link,"$BUILD"
echo "built nvcursortest into $OUT/bin"

# Diagnostic tool: asks NVKMS whether a layout is possible without applying it
# (see LayoutCheck.cpp).
TOOLDIR=$OBJDIR/nvlayoutcheck
mkdir -p "$TOOLDIR"
for f in LayoutCheck NvKmsBitmap NvUtils sdk/ErrorUtils sdk/NvRmApi sdk/NvRmDevice sdk/NvKmsApi sdk/NvKmsDevice sdk/NvKmsSurface; do
	$TOOLS-g++ -std=c++20 "${FLAGS[@]}" -c "$SRC/$f.cpp" -o "$TOOLDIR/$(basename $f).o"
done
$TOOLS-gcc "${FLAGS[@]}" -c "$OGKM/src/common/shared/nvstatus/nvstatus.c" \
	-o "$TOOLDIR/nvstatus.o"
$TOOLS-g++ -nostdlib -o "$OUT/bin/nvlayoutcheck" \
	"$OBJ/system/glue/arch/x86_64/crti.o" "$GCCLIB/crtbegin.o" \
	"$OBJ/system/glue/start_dyn.o" "$OBJ/system/glue/init_term_dyn.o" \
	"$TOOLDIR"/*.o \
	"$OBJ/system/libroot/libroot.so" "$OBJ/kits/libbe.so" \
	"$GCC_SYSLIBS_RUNTIME/lib/libstdc++.so" \
	"$GCC_SYSLIBS_RUNTIME/lib/libgcc_s.so" "$GCCLIB/libgcc.a" \
	"$GCCLIB/crtend.o" "$OBJ/system/glue/arch/x86_64/crtn.o" \
	-Wl,--no-undefined -Wl,-rpath-link,"$BUILD"
echo "built nvlayoutcheck into $OUT/bin"
