#!/bin/bash
# Cross-build Mesa's Turnip (freedreno Vulkan, "msm" kernel interface) for
# air/OS arm64, on the native "adreno" kernel driver of the Radxa Dragon Q8B
# (headers/private/graphics/adreno/adreno_haiku.h), and OpenGL/GLES on it
# through zink. Like tools/cubie-a7s/mesa/build.sh, whose pinned Mesa,
# sysroot, cross file, OpenGL patch (EGL, hgl, zink) and GL tests it uses.
#
#   tools/dragon-q8b/mesa/build.sh [fetch|patch|configure|driver|tests|gl|collect|userland|all]
#
# Everything lives under /mnt/HaikuWork/dragon/mesa (DRAGON_MESA_ROOT).
set -euo pipefail

MESA_VERSION=26.2.4
MESA_ARCHIVE=mesa-$MESA_VERSION.tar.xz
MESA_URL=https://archive.mesa3d.org/$MESA_ARCHIVE
MESA_SHA256=bce5f7fbebb934373b86c999a064d52fb5065878dc57f287f95346648ec832e9

WORK=${HAIKU_WORK_ROOT:-/mnt/HaikuWork}
ROOT=${DRAGON_MESA_ROOT:-$WORK/dragon/mesa}
BASE=${CUBIE_MESA_BASE:-$WORK/artifacts/mali-system-opengl-build/20260918T125722Z}
TOOLS=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TREE=$(cd "$TOOLS/../../.." && pwd)
ADRENO_HEADERS=$TREE/headers/private/graphics/adreno
# zink's direct present asks the Cubie's display driver for its frame buffer
# generation; elsewhere the device is missing and it does without
SUNXI_HEADERS=$TREE/headers/private/graphics/sunxi_display
CUBIE_MESA=$TREE/tools/cubie-a7s/mesa
CROSS=${CUBIE_CROSS_PREFIX:-$WORK/build/arm64/cross-tools-arm64/bin/aarch64-unknown-haiku}
JOBS=${HAIKU_JOBS:-16}

SRC=$ROOT/mesa-$MESA_VERSION
BUILD=$ROOT/build
BUILD_GL=$ROOT/build-gl
OUT=$ROOT/out
# in order, relative to this directory: Turnip, the Cubie's OpenGL patch,
# what OpenGL needs to find this GPU, then glthread for EGL contexts
# (mesa_glthread=true)
PATCHES=(mesa-haiku-turnip.patch ../../cubie-a7s/mesa/mesa-haiku-gl.patch
	mesa-haiku-turnip-gl.patch mesa-haiku-glthread.patch)
TESTS=(tu_vkprobe tu_vkfill)
# the Cubie's OpenGL ES checks, device neutral (through libglvnd's
# libEGL/libGLESv2)
GL_TESTS=(pvr_glprobe pvr_glbench pvr_glcomposite)

export PATH=$WORK/toolchains/mesa-python/bin:$WORK/toolchains/host/usr/bin:$PATH
export TMPDIR=${TMPDIR:-$WORK/tmp}
unset PKG_CONFIG_PATH
mkdir -p "$ROOT" "$TMPDIR"

log() { printf '== %s\n' "$*"; }

fetch() {
	mkdir -p "$ROOT/downloads"
	local archive=$ROOT/downloads/$MESA_ARCHIVE
	local cubie=$WORK/cubie/mesa/downloads/$MESA_ARCHIVE
	if [ ! -f "$archive" ] && [ -f "$cubie" ]; then
		cp "$cubie" "$archive"
	fi
	if [ ! -f "$archive" ]; then
		log "downloading $MESA_URL"
		curl -fL --retry 3 -o "$archive.part" "$MESA_URL"
		mv "$archive.part" "$archive"
	fi
	echo "$MESA_SHA256  $archive" | sha256sum -c --quiet - || {
		echo "$archive: sha256 mismatch" >&2; exit 1; }
	if [ ! -f "$SRC/VERSION" ]; then
		log "unpacking $MESA_ARCHIVE"
		rm -rf "$SRC.tmp" && mkdir -p "$SRC.tmp"
		tar -xf "$archive" -C "$SRC.tmp"
		mv "$SRC.tmp/mesa-$MESA_VERSION" "$SRC" && rmdir "$SRC.tmp"
		# the pristine tree, for regenerating the patch
		rm -rf "$ROOT/pristine" && cp -a "$SRC" "$ROOT/pristine"
	fi
	[ "$(cat "$SRC/VERSION")" = "$MESA_VERSION" ]
}

# The patch carries new files too: applied once, reverted before a changed
# one is applied.
patch_source() {
	local applied=$SRC/.haiku-patches want have= p
	want=$(cd "$TOOLS" && cat "${PATCHES[@]}" | sha256sum | cut -d' ' -f1)
	[ ! -f "$applied/sha256" ] || have=$(cat "$applied/sha256")
	[ "$want" != "$have" ] || { log "patch already applied"; return 0; }
	if [ -d "$applied" ]; then
		log "reverting the patch applied before"
		for p in $(ls -r "$applied"/*.patch 2>/dev/null); do
			patch -d "$SRC" -p1 -R -s --no-backup-if-mismatch < "$p"
		done
		rm -rf "$applied"
	fi
	mkdir -p "$applied"
	local i=1
	for p in "${PATCHES[@]}"; do
		log "applying $p"
		patch -d "$SRC" -p1 --dry-run -s --no-backup-if-mismatch < "$TOOLS/$p"
		patch -d "$SRC" -p1 -s --no-backup-if-mismatch < "$TOOLS/$p"
		cp "$TOOLS/$p" "$applied/$i-$(basename "$p")"
		i=$((i + 1))
	done
	echo "$want" > "$applied/sha256"
}

configure() {
	[ -f "$ADRENO_HEADERS/adreno_haiku.h" ] || {
		echo "adreno_haiku.h not found in $ADRENO_HEADERS" >&2; exit 1; }
	[ -f "$BASE/haiku-aarch64.ini" ] || {
		echo "no Haiku arm64 sysroot/cross file at $BASE" >&2; exit 1; }
	local sysroot=$BASE/sysroot
	cat > "$ROOT/haiku-aarch64-turnip.ini" <<INI
[built-in options]
c_args = ['--sysroot=$sysroot', '-I$ADRENO_HEADERS', '-I$SUNXI_HEADERS', '-ffile-prefix-map=$ROOT/=', '-Wa,--debug-prefix-map=$ROOT/=']
cpp_args = ['--sysroot=$sysroot', '-I$ADRENO_HEADERS', '-I$SUNXI_HEADERS', '-ffile-prefix-map=$ROOT/=', '-Wa,--debug-prefix-map=$ROOT/=']
c_link_args = ['--sysroot=$sysroot']
cpp_link_args = ['--sysroot=$sysroot']
INI
	local mode=
	[ ! -f "$BUILD/build.ninja" ] || mode=--wipe
	meson setup $mode "$BUILD" "$SRC" \
		--cross-file="$BASE/haiku-aarch64.ini" \
		--cross-file="$ROOT/haiku-aarch64-turnip.ini" \
		--prefix=/boot/system/non-packaged --libdir=lib \
		--buildtype=debugoptimized -Db_ndebug=true --wrap-mode=nofallback \
		-Dplatforms=haiku -Dvulkan-drivers=freedreno -Dfreedreno-kmds=msm \
		'-Dgallium-drivers=[]' -Dopengl=false -Dgles1=disabled \
		-Dgles2=disabled -Degl=disabled -Dglx=disabled -Dgbm=disabled \
		-Dglvnd=disabled -Dllvm=disabled -Dmesa-clc=auto \
		-Dprecomp-compiler=auto -Dspirv-tools=disabled \
		-Dshader-cache=disabled -Dvalgrind=disabled -Dlibunwind=disabled \
		-Dzstd=disabled -Dzlib=enabled -Dexpat=disabled -Dxmlconfig=disabled \
		-Dbuild-tests=false '-Dtools=[]' '-Dvulkan-layers=[]' \
		-Dperfetto=false
}

driver() {
	[ -f "$BUILD/build.ninja" ] || configure
	ninja -C "$BUILD" -j"$JOBS" src/freedreno/vulkan/libvulkan_freedreno.so \
		src/freedreno/vulkan/freedreno_icd.aarch64.json
}

tests() {
	local lib=$BUILD/src/freedreno/vulkan
	mkdir -p "$ROOT/tests"
	for t in "${TESTS[@]}"; do
		"$CROSS-gcc" --sysroot="$BASE/sysroot" -std=gnu11 -O2 -g -Wall \
			-Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
			-I"$SRC/include" -o "$ROOT/tests/$t" "$TOOLS/$t.c" \
			-L"$lib" -lvulkan_freedreno -Wl,--no-allow-shlib-undefined \
			-Wl,-rpath-link="$BASE/sysroot/boot/system/lib"
	done
}

# -- OpenGL: EGL with zink and softpipe ---------------------------------------
# As on the Cubie: Mesa's EGL vendor library for libglvnd (libEGL_mesa.so.0,
# GL and GLES too) with zink, on Turnip through libvulkan.so.1
# (vulkan_shim.c), and softpipe as the fallback; the on-disk shader cache.
gl_configure() {
	local pc=$ROOT/gl-pkgconfig
	mkdir -p "$pc"
	# the sysroot's zlib.pc names its original /packages location
	cat > "$pc/zlib.pc" <<'PC'
prefix=/boot/system/develop
libdir=${prefix}/lib
includedir=${prefix}/headers
Name: zlib
Description: zlib in the pinned Haiku ARM64 sysroot
Version: 1.2.13
Libs: -L${libdir} -lz
Cflags: -I${includedir}
PC
	cat > "$ROOT/haiku-aarch64-gl.ini" <<INI
[properties]
pkg_config_libdir = ['$pc', '$BASE/sysroot/boot/system/develop/lib/pkgconfig', '$BASE/sysroot/boot/system/lib/pkgconfig']
INI
	local mode=
	[ ! -f "$BUILD_GL/build.ninja" ] || mode=--wipe
	meson setup $mode "$BUILD_GL" "$SRC" \
		--cross-file="$BASE/haiku-aarch64.ini" \
		--cross-file="$ROOT/haiku-aarch64-turnip.ini" \
		--cross-file="$ROOT/haiku-aarch64-gl.ini" \
		--prefix=/boot/system/non-packaged --libdir=lib \
		--buildtype=debugoptimized -Db_ndebug=true --wrap-mode=nofallback \
		-Dplatforms=haiku -Dgallium-drivers=zink,softpipe '-Dvulkan-drivers=[]' \
		-Dopengl=true -Dgles1=disabled -Dgles2=enabled -Degl=enabled \
		-Dglvnd=enabled -Dglx=disabled -Dgbm=disabled -Dllvm=disabled \
		-Dshader-cache=enabled -Dshader-cache-max-size=128M -Dzlib=enabled \
		-Dzstd=disabled -Dexpat=disabled -Dxmlconfig=disabled \
		-Dvalgrind=disabled -Dlibunwind=disabled -Dmesa-clc=auto \
		-Dprecomp-compiler=auto -Dspirv-tools=disabled \
		-Dbuild-tests=false '-Dtools=[]' '-Dvulkan-layers=[]'
}

gl() {
	local lib=$BUILD/src/freedreno/vulkan
	[ -f "$lib/libvulkan_freedreno.so" ] || driver
	[ -f "$BUILD_GL/build.ninja" ] || gl_configure
	ninja -C "$BUILD_GL" -j"$JOBS" src/egl/libEGL_mesa.so.0.0.0
	local gl=$ROOT/gl
	local opengl=$BASE/sysroot/boot/system/develop/headers/os/opengl
	mkdir -p "$gl"
	"$CROSS-gcc" --sysroot="$BASE/sysroot" -std=gnu11 -O2 -g -Wall -Wextra \
		-fPIC -shared -fvisibility=hidden -Wl,-soname,libvulkan.so.1 \
		-I"$SRC/include" -o "$gl/libvulkan.so.1" "$CUBIE_MESA/vulkan_shim.c" \
		-L"$lib" -lvulkan_freedreno -Wl,--no-allow-shlib-undefined \
		-Wl,-rpath-link="$BASE/sysroot/boot/system/lib"
	local t
	for t in "${GL_TESTS[@]}"; do
		"$CROSS-gcc" --sysroot="$BASE/sysroot" -std=gnu11 -O2 -g -Wall \
			-Wextra -Wno-unused-parameter -I"$opengl" -I"$CUBIE_MESA" \
			-o "$gl/$t" "$CUBIE_MESA/$t.c" \
			-L"$BASE/sysroot/boot/system/develop/lib" -lEGL -lGLESv2 \
			-Wl,--no-allow-shlib-undefined \
			-Wl,-rpath-link="$BASE/sysroot/boot/system/lib"
	done
}

collect() {
	mkdir -p "$OUT/debug"
	local lib=$BUILD/src/freedreno/vulkan f
	cp "$lib/libvulkan_freedreno.so" "$OUT/debug/"
	"$CROSS-strip" -o "$OUT/libvulkan_freedreno.so" "$lib/libvulkan_freedreno.so"
	for t in "${TESTS[@]}"; do
		cp "$ROOT/tests/$t" "$OUT/debug/"
		"$CROSS-strip" -o "$OUT/$t" "$ROOT/tests/$t"
	done
	if [ -f "$BUILD_GL/src/egl/libEGL_mesa.so.0.0.0" ]; then
		cp "$BUILD_GL/src/egl/libEGL_mesa.so.0.0.0" "$OUT/debug/libEGL_mesa.so.0"
		"$CROSS-strip" -o "$OUT/libEGL_mesa.so.0" \
			"$BUILD_GL/src/egl/libEGL_mesa.so.0.0.0"
		for f in libvulkan.so.1 "${GL_TESTS[@]}"; do
			cp "$ROOT/gl/$f" "$OUT/debug/"
			"$CROSS-strip" -o "$OUT/$f" "$ROOT/gl/$f"
		done
		# libglvnd's vendor file, for .../non-packaged/add-ons/opengl/egl_vendor.d
		cat > "$OUT/10_mesa.json" <<'JSON'
{
  "file_format_version": "1.0.0",
  "ICD": {
    "library_path": "/boot/system/non-packaged/lib/libEGL_mesa.so.0"
  }
}
JSON
	fi
	{
		echo "mesa $MESA_VERSION sha256 $MESA_SHA256"
		for p in "${PATCHES[@]}"; do
			echo "patch $(sha256sum "$TOOLS/$p" | cut -d' ' -f1) $(basename "$p")"
		done
		echo "adreno_haiku.h $(sha256sum "$ADRENO_HEADERS/adreno_haiku.h" | cut -d' ' -f1)"
		echo "sysroot $BASE"
		# the files only: the user land tree (adreno) from an earlier run
		# and the debug copies are not results of this one
		(cd "$OUT" && find . -maxdepth 1 -type f ! -name 'MANIFEST' \
			! -name '*.MANIFEST' -printf '%P\n' | sort | xargs sha256sum)
	} > "$OUT/MANIFEST"
	log "results in $OUT"
}

# The tree the arm64 image carries as system/non-packaged/adreno
# (AIROS_CI_ADRENO in tools/airos/ci/UserBuildConfig), which its boot script
# links into system/non-packaged where the adreno driver publishes the GPU:
# Turnip and libvulkan.so.1 in lib, and the EGL vendor file, sorting before
# the ROCK 5's Panfrost one, that names this EGL vendor library in place.
# The GPU firmware is not in it: the driver needs it before the boot script
# runs, so it goes in the image's firmware tree (AIROS_CI_FIRMWARE, from
# tools/dragon-q8b/fetch-gpu-firmware.sh).
userland() {
	local tree=$OUT/adreno
	[ -f "$OUT/libEGL_mesa.so.0" ] || { echo "run collect first" >&2; exit 1; }
	rm -rf "$tree"
	mkdir -p "$tree/lib" "$tree/add-ons/opengl/egl_vendor.d"
	cp "$OUT/libvulkan_freedreno.so" "$OUT/libvulkan.so.1" \
		"$OUT/libEGL_mesa.so.0" "$tree/lib/"
	cat > "$tree/add-ons/opengl/egl_vendor.d/05_mesa_adreno.json" <<'JSON'
{
  "file_format_version": "1.0.0",
  "ICD": {
    "library_path": "/boot/system/non-packaged/adreno/lib/libEGL_mesa.so.0"
  }
}
JSON
	(cd "$tree" && find . -type f | sort | xargs sha256sum) > "$OUT/adreno.MANIFEST"
	log "user land in $tree"
}

case ${1:-all} in
fetch) fetch ;;
patch) patch_source ;;
configure) configure ;;
driver) driver ;;
tests) tests ;;
gl) gl_configure; gl ;;
collect) collect ;;
userland) userland ;;
all) fetch; patch_source; driver; tests; gl_configure; gl; collect; userland ;;
*) echo "usage: $0 [fetch|patch|configure|driver|tests|gl|collect|userland|all]" >&2; exit 2 ;;
esac
