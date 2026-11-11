#!/bin/bash
# Cross-build the nvidia_rm kernel add-ons for x86_64 from a configured Haiku
# build directory.
#
# usage: build-cross.sh <haiku build dir> <work dir> <output dir>
#        (OGKM_SRC=<open-gpu-kernel-modules checkout at OGKM_COMMIT>)
#
# The RM core of NVIDIA's proprietary driver is non-PIC code for GCC's kernel
# code model, so nvidia_rm is linked as an ET_EXEC image at a fixed address in
# the top 2 GiB (see KERNEL_FIXED_ADD_ON_BASE). nvidia_rm_modeset is an
# ordinary position-independent add-on built from NVIDIA's open sources.
set -euo pipefail

NV_VERSION=570.86.16
NV_RUN=NVIDIA-Linux-x86_64-$NV_VERSION.run
NV_RUN_URL=https://us.download.nvidia.com/XFree86/Linux-x86_64/$NV_VERSION/$NV_RUN
NV_RUN_SHA256=4563ea4bb654247f491005a57c72c676aacd95abe1691d71332cecf25261cbd5
OGKM_URL=https://github.com/X547/open-gpu-kernel-modules.git
OGKM_COMMIT=2badeb8166550238ff2f3f57de61adb89a14642c
LOAD_ADDRESS=0xfffffffff0000000

BUILD=$(realpath "$1")
WORK=$(realpath -m "$2")
OUT=$(realpath -m "$3")
SRC=$(dirname "$(realpath "$0")")
HAIKU=$(realpath "$SRC/../../../../../..")
JOBS=${JOBS:-$(nproc)}

TOOLS=$BUILD/cross-tools-x86_64/bin/x86_64-unknown-haiku
OBJ=$BUILD/objects/haiku/x86_64/release
GCC_SYSLIBS=$(ls -d "$BUILD"/build_packages/gcc_syslibs_devel-*)
GCCLIB=$(dirname "$($TOOLS-gcc -print-libgcc-file-name)")

mkdir -p "$WORK" "$OUT"

# NVIDIA proprietary driver: RM core object
if [ ! -f "$WORK/$NV_RUN" ]; then
	curl -fL -o "$WORK/$NV_RUN.part" "$NV_RUN_URL"
	mv "$WORK/$NV_RUN.part" "$WORK/$NV_RUN"
fi
echo "$NV_RUN_SHA256  $WORK/$NV_RUN" | sha256sum -c --quiet
NV_DIR=$WORK/NVIDIA-Linux-x86_64-$NV_VERSION
[ -d "$NV_DIR" ] || (cd "$WORK" && sh "$NV_RUN" -x >/dev/null)
NV_KERNEL=$NV_DIR/kernel/nvidia/nv-kernel.o_binary

# Open GPU kernel modules with Haiku build support: headers and NVKMS.
# OGKM_SRC names a checkout of that commit made elsewhere (the air/OS CI
# uses jmgasper's fork); otherwise it is cloned into the work dir.
if [ -n "${OGKM_SRC:-}" ]; then
	OGKM=$(realpath "$OGKM_SRC")
else
	OGKM=$WORK/open-gpu-kernel-modules
	if [ ! -d "$OGKM/.git" ]; then
		git clone -q "$OGKM_URL" "$OGKM"
	fi
	git -C "$OGKM" checkout -q "$OGKM_COMMIT"
fi
[ "$(git -C "$OGKM" rev-parse HEAD)" = "$OGKM_COMMIT" ] \
	|| { echo "$OGKM is not at $OGKM_COMMIT" >&2; exit 1; }

KCC=$WORK/haiku-kcc
cat > "$KCC" <<KCCEOF
#!/bin/sh
exec $TOOLS-gcc -isystem $HAIKU/headers/posix -isystem $HAIKU/headers/glibc \\
	-isystem $HAIKU/headers/os -isystem $HAIKU/headers/os/support \\
	-isystem $HAIKU/headers/os/kernel -isystem $HAIKU/headers/os/drivers \\
	-isystem $HAIKU/headers -D_KERNEL_MODE -mno-red-zone "\$@"
KCCEOF
chmod +x "$KCC"
make -s -C "$OGKM/src/nvidia-modeset" -j"$JOBS" TARGET_OS=Haiku TARGET_ARCH=x86_64 \
	CC="$KCC" LD=$TOOLS-ld AR=$TOOLS-ar OBJCOPY=$TOOLS-objcopy HOST_CC=gcc
NVKMS_KERNEL=$OGKM/src/nvidia-modeset/_out/Haiku_x86_64/nv-modeset-kernel.o

FLAGS=(-O2 -fpic -fno-exceptions -fno-rtti -fno-use-cxa-atexit -mno-red-zone
	-fno-omit-frame-pointer -nostdinc -ffreestanding
	-D_KERNEL_MODE -D_DEFAULT_SOURCE -DNV_PLATFORM_MAX_IOCTL_SIZE=16384
	-Wno-missing-field-initializers
	-I"$SRC" -I"$SRC/common" -I"$SRC/rm" -I"$SRC/sdk"
	-I"$OGKM/src/common" -I"$OGKM/src/common/sdk/nvidia/inc"
	-I"$OGKM/src/common/unix/common/inc" -I"$OGKM/src/common/inc"
	-I"$OGKM/src/nvidia/arch/nvalloc/common/inc"
	-I"$OGKM/src/nvidia/arch/nvalloc/unix/include"
	-I"$OGKM/src/nvidia/inc/kernel" -I"$OGKM/src/nvidia/interface"
	-I"$OGKM/src/nvidia-modeset/interface"
	-I"$OGKM/src/nvidia-modeset/os-interface/include"
	-I"$OGKM/src/nvidia-modeset/kapi/interface"
	-I"$HAIKU/build/config_headers"
	-I"$HAIKU/headers/private" -I"$HAIKU/headers/private/shared"
	-I"$HAIKU/headers/private/kernel" -I"$HAIKU/headers/private/kernel/arch/x86"
	-I"$HAIKU/headers/private/kernel/arch/x86/64"
	-I"$HAIKU/headers/private/system" -I"$HAIKU/headers/private/system/arch/x86_64"
	-I"$HAIKU/headers/private/kernel/boot/platform/efi"
	-I"$HAIKU/headers/private/libroot"
	-I"$OBJ/../common/system/kernel"
	-I"$GCC_SYSLIBS/develop/headers/c++"
	-I"$GCC_SYSLIBS/develop/headers/c++/x86_64-unknown-haiku"
	-I"$GCC_SYSLIBS/develop/headers/gcc/include"
	-I"$GCC_SYSLIBS/develop/headers/gcc/include-fixed"
	-I"$HAIKU/headers/compatibility/bsd" -I"$HAIKU/headers/glibc"
	-I"$HAIKU/headers/posix" -I"$HAIKU/headers" -I"$HAIKU/headers/os"
	-I"$HAIKU/headers/os/drivers" -I"$HAIKU/headers/os/kernel"
	-I"$HAIKU/headers/os/support" -I"$HAIKU/headers/os/device"
	-I"$HAIKU/headers/os/storage" -I"$HAIKU/headers/os/app")

CFLAGS=()
for flag in "${FLAGS[@]}"; do
	case "$flag" in -fno-rtti|-fno-use-cxa-atexit) ;; *) CFLAGS+=("$flag") ;; esac
done

compile() {
	local out=$1 src=$2
	case "$src" in
		*.cpp) $TOOLS-g++ -std=c++20 "${FLAGS[@]}" -c "$src" -o "$out" ;;
		*.c) $TOOLS-gcc "${CFLAGS[@]}" -c "$src" -o "$out" ;;
		*.S) $TOOLS-gcc -c "$src" -o "$out" ;;
	esac
}

mkdir -p "$WORK/obj/common" "$WORK/obj/rm" "$WORK/obj/modeset"
for f in CppUtils DeviceNamesArray; do
	compile "$WORK/obj/common/$f.o" "$SRC/common/$f.cpp"
done
for f in BaseDevice ControlDevice Device Driver IntrSafePool NvidiaModule \
		os-haiku os-haiku-rm proprietary_rm; do
	compile "$WORK/obj/rm/$f.o" "$SRC/rm/$f.cpp" &
done
compile "$WORK/obj/rm/stubs.o" "$SRC/rm/stubs.c" &
compile "$WORK/obj/rm/retpoline_thunks.o" "$SRC/rm/retpoline_thunks.S" &
for f in nvkms-haiku KmsDriver KmsDevice TimerQueue; do
	compile "$WORK/obj/modeset/$f.o" "$SRC/modeset/$f.cpp" &
done
failed=0
for job in $(jobs -p); do
	wait "$job" || failed=1
done
[ $failed = 0 ] || { echo "compilation failed" >&2; exit 1; }

LINK_COMMON=(-z max-page-size=0x1000 -rpath-link "$BUILD")
KERNEL_LIBS=("$OBJ/system/kernel/kernel.so" "$OBJ/system/glue/haiku_version_glue.o"
	"$GCCLIB/libgcc.a")

mkdir -p "$OUT/add-ons/kernel/drivers/bin" "$OUT/add-ons/kernel/drivers/dev/graphics"
$TOOLS-ld -o "$OUT/add-ons/kernel/drivers/bin/nvidia_rm" "${LINK_COMMON[@]}" \
	-z noseparate-code -E --entry=0 --no-dynamic-linker \
	-Ttext-segment=$LOAD_ADDRESS \
	"$GCCLIB/crtbegin.o" "$WORK"/obj/common/*.o "$WORK"/obj/rm/*.o \
	"$NV_KERNEL" "${KERNEL_LIBS[@]}" "$GCCLIB/crtend.o"
$TOOLS-ld -o "$OUT/add-ons/kernel/drivers/bin/nvidia_rm_modeset" "${LINK_COMMON[@]}" \
	-shared --no-undefined \
	"$GCCLIB/crtbeginS.o" "$WORK"/obj/common/*.o "$WORK"/obj/modeset/*.o \
	"$NVKMS_KERNEL" "${KERNEL_LIBS[@]}" "$GCCLIB/crtendS.o"

ln -sf ../../bin/nvidia_rm "$OUT/add-ons/kernel/drivers/dev/graphics/nvidia_rm"
ln -sf ../bin/nvidia_rm_modeset "$OUT/add-ons/kernel/drivers/dev/nvidia_rm_modeset"
echo "built nvidia_rm $NV_VERSION into $OUT"
