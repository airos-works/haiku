#!/bin/bash
# Cross-build Mesa's Turnip (freedreno Vulkan, "msm" kernel interface) for
# air/OS arm64, on the native "adreno" kernel driver of the Radxa Dragon Q8B
# (headers/private/graphics/adreno/adreno_haiku.h). Like
# tools/cubie-a7s/mesa/build.sh, whose pinned Mesa, sysroot and cross file it
# uses.
#
#   tools/dragon-q8b/mesa/build.sh [fetch|patch|configure|driver|tests|all]
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
CROSS=${CUBIE_CROSS_PREFIX:-$WORK/build/arm64/cross-tools-arm64/bin/aarch64-unknown-haiku}
JOBS=${HAIKU_JOBS:-16}

SRC=$ROOT/mesa-$MESA_VERSION
BUILD=$ROOT/build
OUT=$ROOT/out
PATCHES=(mesa-haiku-turnip.patch)
TESTS=(tu_vkprobe tu_vkfill)

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
	for p in "${PATCHES[@]}"; do
		log "applying $p"
		patch -d "$SRC" -p1 --dry-run -s --no-backup-if-mismatch < "$TOOLS/$p"
		patch -d "$SRC" -p1 -s --no-backup-if-mismatch < "$TOOLS/$p"
		cp "$TOOLS/$p" "$applied/$p"
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
c_args = ['--sysroot=$sysroot', '-I$ADRENO_HEADERS', '-ffile-prefix-map=$ROOT/=', '-Wa,--debug-prefix-map=$ROOT/=']
cpp_args = ['--sysroot=$sysroot', '-I$ADRENO_HEADERS', '-ffile-prefix-map=$ROOT/=', '-Wa,--debug-prefix-map=$ROOT/=']
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

collect() {
	mkdir -p "$OUT"
	cp "$BUILD/src/freedreno/vulkan/libvulkan_freedreno.so" "$OUT/"
	"$CROSS-strip" "$OUT/libvulkan_freedreno.so"
	for t in "${TESTS[@]}"; do
		cp "$ROOT/tests/$t" "$OUT/"
	done
}

case ${1:-all} in
fetch) fetch ;;
patch) patch_source ;;
configure) configure ;;
driver) driver ;;
tests) tests ;;
all) fetch; patch_source; driver; tests; collect ;;
*) echo "usage: $0 [fetch|patch|configure|driver|tests|all]" >&2; exit 2 ;;
esac
