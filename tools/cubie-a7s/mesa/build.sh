#!/usr/bin/env bash
# Build Mesa's PowerVR Vulkan driver (libvulkan_powervr_mesa.so) and its
# test programs for air/OS arm64 (Cubie A7S, PowerVR BXM-4-64 MC1).
#
#   build.sh [all|fetch|host|patch|configure|driver|tests|gl|shim]
#   (default: all)
#
# From a clean $ROOT, "all" downloads and verifies the pinned Mesa source,
# builds Mesa's host shader tools (mesa_clc, vtn_bindgen2, pco_clc) at the
# same version, applies mesa-haiku-pvr.patch, cross-builds the driver and
# the test programs and copies the results to $ROOT/out. Every step
# is incremental; rerunning it is cheap.
#
# The driver talks to the air/OS kernel driver "powervr" through
# /dev/graphics/powervr/0; its userland contract is pvr_haiku.h in this
# tree (headers/private/graphics/powervr), used by path, not copied.
set -euo pipefail

MESA_VERSION=26.2.4
MESA_ARCHIVE=mesa-$MESA_VERSION.tar.xz
MESA_URL=https://archive.mesa3d.org/$MESA_ARCHIVE
# docs/relnotes/26.2.4.rst ("SHA checksums"); the archive's .sig is by Eric
# Engestrom, 57551DE15B968F6341C248F68D8E31AFC32428A6
MESA_SHA256=bce5f7fbebb934373b86c999a064d52fb5065878dc57f287f95346648ec832e9

WORK=/mnt/HaikuWork
ROOT=${CUBIE_MESA_ROOT:-$WORK/cubie/mesa}
# the ROCK 5's pinned arm64 Haiku sysroot and cross file (also used by rpi4)
BASE=${CUBIE_MESA_BASE:-$WORK/artifacts/mali-system-opengl-build/20260918T125722Z}
TOOLS=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TREE=$(cd "$TOOLS/../../.." && pwd)
PVR_HEADERS=${PVR_HAIKU_HEADERS:-$TREE/headers/private/graphics/powervr}
# the display driver's contract, for the frame buffer import (direct present)
SUNXI_HEADERS=$TREE/headers/private/graphics/sunxi_display
CROSS=$WORK/build/arm64/cross-tools-arm64/bin/aarch64-unknown-haiku
NATIVE_DEPS=$WORK/toolchains/mesa-native-deps
JOBS=${HAIKU_JOBS:-16}

SRC=$ROOT/mesa-$MESA_VERSION
HOST_BUILD=$ROOT/build-host
HOST_TOOLS=$ROOT/host-tools
BUILD=$ROOT/build
OUT=$ROOT/out
BUILD_GL=$ROOT/build-gl
# applied in this order; mesa-haiku-gl.patch only touches files the first
# one does not
PATCHES=(mesa-haiku-pvr.patch mesa-haiku-gl.patch)
TESTS=(pvr_vkprobe pvr_vkfill pvr_vkfence pvr_vktriangle pvr_vkhang
	pvr_vkbench pvr_present pvr_copybench)
# OpenGL ES programs (through libglvnd's libEGL/libGLESv2)
GL_TESTS=(pvr_glprobe pvr_glbench pvr_glreset pvr_glpresent pvr_glcomposite)
# the runtime loader's thread-local storage for dlopen()ed libraries: a
# program and the library it loads, built from one file
TLS_CHECK=(tls_generation_check libtls_generation_check.so)

export PATH=$HOST_TOOLS/bin:$WORK/toolchains/mesa-python/bin:$WORK/toolchains/host/usr/bin:$PATH
export LD_LIBRARY_PATH=$NATIVE_DEPS/usr/lib/x86_64-linux-gnu:$NATIVE_DEPS/usr/lib/llvm-18/lib
export TMPDIR=$WORK/tmp
unset PKG_CONFIG_PATH
mkdir -p "$ROOT" "$TMPDIR"

log() { printf '== %s\n' "$*"; }

# -- source -------------------------------------------------------------------
fetch() {
	mkdir -p "$ROOT/downloads"
	local archive=$ROOT/downloads/$MESA_ARCHIVE
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
	fi
	[ "$(cat "$SRC/VERSION")" = "$MESA_VERSION" ]
}

# -- the patches --------------------------------------------------------------
# Applied once; $SRC/.haiku-patches keeps copies of what was applied and the
# hash of the series. A changed series is applied after reverting the old
# one, last patch first.
patch_source() {
	local applied=$SRC/.haiku-patches want have= p n=1
	want=$(cd "$TOOLS" && cat "${PATCHES[@]}" | sha256sum | cut -d' ' -f1)
	[ ! -f "$applied/sha256" ] || have=$(cat "$applied/sha256")
	# a tree patched when there was only mesa-haiku-pvr.patch
	if [ -z "$have" ] && [ -f "$SRC/.haiku-pvr.patch" ]; then
		mkdir -p "$applied"
		mv "$SRC/.haiku-pvr.patch" "$applied/1-mesa-haiku-pvr.patch"
		rm -f "$SRC/.haiku-pvr.sha256"
		have=old
	fi
	[ "$want" != "$have" ] || { log "patches already applied"; return 0; }
	if [ -d "$applied" ]; then
		log "reverting the patches applied before"
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
		cp "$TOOLS/$p" "$applied/$n-$p"
		n=$((n + 1))
	done
	echo "$want" > "$applied/sha256"
}

# -- host tools ---------------------------------------------------------------
# Mesa's precompiled shaders are serialized NIR and USC code, so the build
# tools must be this exact Mesa version: mesa_clc (OpenCL C -> SPIR-V),
# vtn_bindgen2 (SPIR-V -> NIR builder functions) and pco_clc (the PowerVR
# compiler run on the USC library). They are built natively against the
# host's LLVM 18 with clang-cpp, LLVMSPIRVLib and SPIRV-Tools from
# toolchains/mesa-native-deps; toolchains/mesa-host (Mesa 25.3.6) is not
# touched.
host_tools() {
	local stamp=$HOST_TOOLS/.mesa-version
	if [ -x "$HOST_TOOLS/bin/mesa_clc" ] && [ -x "$HOST_TOOLS/bin/vtn_bindgen2" ] &&
	   [ -x "$HOST_TOOLS/bin/pco_clc" ] && [ "$(cat "$stamp" 2>/dev/null)" = "$MESA_VERSION" ]; then
		log "host tools present ($HOST_TOOLS/bin)"
		return 0
	fi
	log "building host tools"
	local pc=$ROOT/host-pkgconfig nd=$NATIVE_DEPS/usr
	mkdir -p "$pc"
	# the extracted .deb pkg-config files say prefix=/usr
	cat > "$pc/SPIRV-Tools.pc" <<PC
libdir=$nd/lib/x86_64-linux-gnu
includedir=$nd/include
Name: SPIRV-Tools
Description: Tools for SPIR-V (toolchains/mesa-native-deps)
Version: 2025.1.1
Libs: -L\${libdir} -lSPIRV-Tools-opt -lSPIRV-Tools -lSPIRV-Tools-link
Cflags: -I\${includedir}
PC
	cat > "$pc/LLVMSPIRVLib.pc" <<PC
libdir=$nd/lib/x86_64-linux-gnu
includedir=$nd/include
Name: LLVMSPIRVLib
Description: LLVM/SPIR-V bi-directional translator (toolchains/mesa-native-deps)
Version: 18.1.0.0
Libs: -L\${libdir} -lLLVMSPIRVLib
Cflags: -I\${includedir}
PC
	# clang-cpp's development link lives only in mesa-native-deps; the
	# run-time library is the host's (identical) one
	cat > "$ROOT/host-native.ini" <<INI
[binaries]
llvm-config = '/usr/bin/llvm-config-18'

[properties]
pkg_config_libdir = ['$pc', '/usr/lib/x86_64-linux-gnu/pkgconfig', '/usr/share/pkgconfig']

[built-in options]
c_link_args = ['-L$nd/lib/llvm-18/lib', '-Wl,-rpath,$nd/lib/x86_64-linux-gnu']
cpp_link_args = ['-L$nd/lib/llvm-18/lib', '-Wl,-rpath,$nd/lib/x86_64-linux-gnu']
INI
	if [ ! -f "$HOST_BUILD/build.ninja" ]; then
		LIBRARY_PATH=$nd/lib/llvm-18/lib meson setup "$HOST_BUILD" "$SRC" \
			--native-file="$ROOT/host-native.ini" --prefix="$HOST_TOOLS" \
			--buildtype=release --wrap-mode=nofallback \
			-Dplatforms= -Dgallium-drivers= -Dvulkan-drivers=imagination \
			-Dopengl=false -Dgles1=disabled -Dgles2=disabled -Degl=disabled \
			-Dglx=disabled -Dgbm=disabled -Dglvnd=disabled -Dllvm=enabled \
			-Dshared-llvm=enabled -Dmesa-clc=enabled -Dinstall-mesa-clc=true \
			-Dprecomp-compiler=enabled -Dinstall-precomp-compiler=true \
			-Dspirv-tools=enabled -Dbuild-tests=false '-Dtools=[]' \
			-Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=disabled \
			-Dexpat=disabled -Dxmlconfig=disabled -Dshader-cache=disabled \
			-Dvulkan-layers=[] -Dintel-rt=disabled
	fi
	LIBRARY_PATH=$nd/lib/llvm-18/lib ninja -C "$HOST_BUILD" -j"$JOBS" \
		src/compiler/clc/mesa_clc src/compiler/spirv/vtn_bindgen2 \
		src/imagination/pco/uscgen/pco_clc
	mkdir -p "$HOST_TOOLS/bin"
	install -m755 "$HOST_BUILD/src/compiler/clc/mesa_clc" \
		"$HOST_BUILD/src/compiler/spirv/vtn_bindgen2" \
		"$HOST_BUILD/src/imagination/pco/uscgen/pco_clc" "$HOST_TOOLS/bin/"
	echo "$MESA_VERSION" > "$stamp"
}

# How to run "meson setup" on build directory $1 with machine files $2...:
# --reconfigure, or --wipe when the machine files changed since it was set
# up (Meson reads their options only then).
setup_mode() {
	local dir=$1 hash
	shift
	hash=$(cat "$@" | sha256sum | cut -d' ' -f1)
	if [ ! -f "$dir/build.ninja" ]; then
		:
	elif [ "$(cat "$dir/.haiku-machine-files" 2>/dev/null)" = "$hash" ]; then
		echo --reconfigure
	else
		echo --wipe
	fi
}

remember_machine_files() {
	local dir=$1
	shift
	cat "$@" | sha256sum | cut -d' ' -f1 > "$dir/.haiku-machine-files"
}

# -- the driver ---------------------------------------------------------------
# Only the PowerVR Vulkan driver: no GL, no EGL, no LLVM (the shader
# compilers that need it ran on the host), no window system integration.
configure() {
	[ -f "$PVR_HEADERS/pvr_haiku.h" ] || {
		echo "pvr_haiku.h not found in $PVR_HEADERS" >&2; exit 1; }
	[ -f "$SUNXI_HEADERS/sunxi_display.h" ] || {
		echo "sunxi_display.h not found in $SUNXI_HEADERS" >&2; exit 1; }
	[ -f "$BASE/haiku-aarch64.ini" ] || {
		echo "no Haiku arm64 sysroot/cross file at $BASE" >&2; exit 1; }
	local sysroot=$BASE/sysroot
	# BASE's cross file names the compilers and the sysroot; this one adds
	# the kernel driver's userland header directory to every compile, and
	# keeps $ROOT out of the debug information, the compiler's and the
	# assembler's line tables (and so out of the build-id: the same output
	# from any root)
	cat > "$ROOT/haiku-aarch64-pvr.ini" <<INI
[built-in options]
c_args = ['--sysroot=$sysroot', '-I$PVR_HEADERS', '-I$SUNXI_HEADERS', '-ffile-prefix-map=$ROOT/=', '-Wa,--debug-prefix-map=$ROOT/=']
cpp_args = ['--sysroot=$sysroot', '-I$PVR_HEADERS', '-I$SUNXI_HEADERS', '-ffile-prefix-map=$ROOT/=', '-Wa,--debug-prefix-map=$ROOT/=']
c_link_args = ['--sysroot=$sysroot']
cpp_link_args = ['--sysroot=$sysroot']
INI
	local files=("$BASE/haiku-aarch64.ini" "$ROOT/haiku-aarch64-pvr.ini")
	meson setup $(setup_mode "$BUILD" "${files[@]}") "$BUILD" "$SRC" \
		--cross-file="$BASE/haiku-aarch64.ini" \
		--cross-file="$ROOT/haiku-aarch64-pvr.ini" \
		--prefix=/boot/system/non-packaged --libdir=lib \
		--buildtype=debugoptimized --wrap-mode=nofallback \
		-Dplatforms=haiku -Dvulkan-drivers=imagination '-Dgallium-drivers=[]' \
		-Dimagination-srv=false -Dopengl=false -Dgles1=disabled \
		-Dgles2=disabled -Degl=disabled -Dglx=disabled -Dgbm=disabled \
		-Dglvnd=disabled -Dllvm=disabled -Dmesa-clc=system \
		-Dprecomp-compiler=system -Dspirv-tools=disabled \
		-Dshader-cache=disabled -Dvalgrind=disabled -Dlibunwind=disabled \
		-Dzstd=disabled -Dzlib=disabled -Dexpat=disabled -Dxmlconfig=disabled \
		-Dbuild-tests=false '-Dtools=[]' '-Dvulkan-layers=[]'
	remember_machine_files "$BUILD" "${files[@]}"
}

driver() {
	[ -f "$BUILD/build.ninja" ] || configure
	ninja -C "$BUILD" -j"$JOBS" src/imagination/vulkan/libvulkan_powervr_mesa.so \
		src/imagination/vulkan/powervr_mesa_icd.aarch64.json
}

# -- tests and results --------------------------------------------------------
# The test programs link the driver directly: there is no Vulkan loader on
# arm64 Haiku. They start from vk_icdGetInstanceProcAddr.
# Linking them also checks that every symbol the driver needs is in the
# sysroot's libroot/libstdc++ (--no-allow-shlib-undefined).
tests() {
	local lib=$BUILD/src/imagination/vulkan
	mkdir -p "$ROOT/tests"
	for t in "${TESTS[@]}"; do
		"$CROSS-gcc" --sysroot="$BASE/sysroot" -std=gnu11 -O2 -g -Wall \
			-Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
			-I"$SRC/include" -o "$ROOT/tests/$t" "$TOOLS/$t.c" \
			-L"$lib" -lvulkan_powervr_mesa -Wl,--no-allow-shlib-undefined \
			-Wl,-rpath-link="$BASE/sysroot/boot/system/lib"
	done
	"$CROSS-gcc" --sysroot="$BASE/sysroot" -std=gnu11 -O2 -g -Wall -Wextra \
		-fPIC -shared -DTLS_GENERATION_LIBRARY \
		-o "$ROOT/tests/${TLS_CHECK[1]}" "$TOOLS/tls_generation_check.c"
	"$CROSS-gcc" --sysroot="$BASE/sysroot" -std=gnu11 -O2 -g -Wall -Wextra \
		-o "$ROOT/tests/${TLS_CHECK[0]}" "$TOOLS/tls_generation_check.c"
}

# -- OpenGL: EGL with zink and softpipe -----------------------------------------
# A second build of the same tree: Mesa's EGL vendor library for libglvnd
# (libEGL_mesa.so.0, which carries GL and GLES too) with the Gallium drivers
# zink (GL on the PowerVR Vulkan driver) and softpipe (the fallback), and
# the on-disk shader cache. llvmpipe would need LLVM for arm64 Haiku, which
# the sysroot does not have. Zink finds the Vulkan driver through
# libvulkan.so.1, vulkan_shim.c.
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
	local files=("$BASE/haiku-aarch64.ini" "$ROOT/haiku-aarch64-pvr.ini"
		"$ROOT/haiku-aarch64-gl.ini")
	meson setup $(setup_mode "$BUILD_GL" "${files[@]}") "$BUILD_GL" "$SRC" \
		--cross-file="$BASE/haiku-aarch64.ini" \
		--cross-file="$ROOT/haiku-aarch64-pvr.ini" \
		--cross-file="$ROOT/haiku-aarch64-gl.ini" \
		--prefix=/boot/system/non-packaged --libdir=lib \
		--buildtype=debugoptimized --wrap-mode=nofallback \
		-Dplatforms=haiku -Dgallium-drivers=zink,softpipe '-Dvulkan-drivers=[]' \
		-Dopengl=true -Dgles1=disabled -Dgles2=enabled -Degl=enabled \
		-Dglvnd=enabled -Dglx=disabled -Dgbm=disabled -Dllvm=disabled \
		-Dshader-cache=enabled -Dshader-cache-max-size=128M -Dzlib=enabled \
		-Dzstd=disabled -Dexpat=disabled -Dxmlconfig=disabled \
		-Dvalgrind=disabled -Dlibunwind=disabled -Dmesa-clc=system \
		-Dprecomp-compiler=system -Dspirv-tools=disabled \
		-Dbuild-tests=false '-Dtools=[]' '-Dvulkan-layers=[]'
	remember_machine_files "$BUILD_GL" "${files[@]}"
}

gl() {
	[ -f "$BUILD/src/imagination/vulkan/libvulkan_powervr_mesa.so" ] || driver
	[ -f "$BUILD_GL/build.ninja" ] || gl_configure
	ninja -C "$BUILD_GL" -j"$JOBS" src/egl/libEGL_mesa.so.0.0.0
	local gl=$ROOT/gl lib=$BUILD/src/imagination/vulkan
	local opengl=$BASE/sysroot/boot/system/develop/headers/os/opengl
	mkdir -p "$gl"
	"$CROSS-gcc" --sysroot="$BASE/sysroot" -std=gnu11 -O2 -g -Wall -Wextra \
		-fPIC -shared -fvisibility=hidden -Wl,-soname,libvulkan.so.1 \
		-I"$SRC/include" -o "$gl/libvulkan.so.1" "$TOOLS/vulkan_shim.c" \
		-L"$lib" -lvulkan_powervr_mesa -Wl,--no-allow-shlib-undefined \
		-Wl,-rpath-link="$BASE/sysroot/boot/system/lib"
	local t
	for t in "${GL_TESTS[@]}"; do
		"$CROSS-gcc" --sysroot="$BASE/sysroot" -std=gnu11 -O2 -g -Wall \
			-Wextra -Wno-unused-parameter -I"$opengl" -o "$gl/$t" \
			"$TOOLS/$t.c" -L"$BASE/sysroot/boot/system/develop/lib" -lEGL \
			-lGLESv2 -Wl,--no-allow-shlib-undefined \
			-Wl,-rpath-link="$BASE/sysroot/boot/system/lib"
	done
	check_symbols "$BUILD_GL/src/egl/libEGL_mesa.so.0.0.0"
}

# Every symbol a library needs is defined by one of the libraries it names
# (NEEDED), found in the sysroot. (ld --no-allow-shlib-undefined would also
# follow libbe's own dependencies, which the sysroot does not all have.)
check_symbols() {
	local lib=$1 n d defined missing
	local dirs=("$BASE/sysroot/boot/system/lib"
		"$BASE/sysroot/boot/system/develop/lib")
	defined=$(for n in $("$CROSS-readelf" -d "$lib" \
			| sed -n 's/.*Shared library: \[\(.*\)\]/\1/p'); do
		for d in "${dirs[@]}"; do
			[ -f "$d/$n" ] || continue
			"$CROSS-readelf" --dyn-syms -W "$d/$n" \
				| awk '$7 != "UND" && $8 != "" { sub(/@.*/, "", $8); print $8 }'
			continue 2
		done
		echo "$lib: $n is not in the sysroot" >&2
		exit 1
	done | sort -u)
	missing=$("$CROSS-readelf" --dyn-syms -W "$lib" \
		| awk '$7 == "UND" && $5 != "WEAK" && $8 != "" \
			{ sub(/@.*/, "", $8); print $8 }' \
		| sort -u | comm -23 - <(echo "$defined"))
	if [ -n "$missing" ]; then
		echo "$lib: undefined:" $missing >&2
		exit 1
	fi
	log "$(basename "$lib"): every symbol it needs is in the sysroot"
}

results() {
	local lib=$BUILD/src/imagination/vulkan
	mkdir -p "$OUT/debug"
	cp "$lib/libvulkan_powervr_mesa.so" "$OUT/debug/"
	"$CROSS-strip" -o "$OUT/libvulkan_powervr_mesa.so" \
		"$lib/libvulkan_powervr_mesa.so"
	for t in "${TESTS[@]}" "${TLS_CHECK[@]}"; do
		cp "$ROOT/tests/$t" "$OUT/debug/"
		"$CROSS-strip" -o "$OUT/$t" "$ROOT/tests/$t"
	done
	# for a Vulkan loader later (library in /boot/system/non-packaged/lib);
	# the tests do not read it
	cp "$lib/powervr_mesa_icd.aarch64.json" "$OUT/"
	# OpenGL: the EGL vendor library and its libglvnd vendor file, the
	# libvulkan.so.1 zink loads, and the EGL/GLES2 check
	cp "$BUILD_GL/src/egl/libEGL_mesa.so.0.0.0" "$OUT/debug/libEGL_mesa.so.0"
	"$CROSS-strip" -o "$OUT/libEGL_mesa.so.0" \
		"$BUILD_GL/src/egl/libEGL_mesa.so.0.0.0"
	cp "$ROOT/gl/libvulkan.so.1" "$OUT/debug/"
	"$CROSS-strip" -o "$OUT/libvulkan.so.1" "$ROOT/gl/libvulkan.so.1"
	for t in "${GL_TESTS[@]}"; do
		cp "$ROOT/gl/$t" "$OUT/debug/"
		"$CROSS-strip" -o "$OUT/$t" "$ROOT/gl/$t"
	done
	cat > "$OUT/10_mesa.json" <<'JSON'
{
  "file_format_version": "1.0.0",
  "ICD": {
    "library_path": "/boot/system/non-packaged/lib/libEGL_mesa.so.0"
  }
}
JSON
	{
		echo "mesa $MESA_VERSION sha256 $MESA_SHA256"
		for p in "${PATCHES[@]}"; do
			echo "patch $(sha256sum "$TOOLS/$p" | cut -d' ' -f1) $p"
		done
		echo "pvr_haiku.h $(sha256sum "$PVR_HEADERS/pvr_haiku.h" | cut -d' ' -f1) $PVR_HEADERS/pvr_haiku.h"
		echo "sunxi_display.h $(sha256sum "$SUNXI_HEADERS/sunxi_display.h" | cut -d' ' -f1) $SUNXI_HEADERS/sunxi_display.h"
		echo "sysroot $BASE"
		echo "host tools $(cat "$HOST_TOOLS/.mesa-version") $HOST_TOOLS/bin"
		(cd "$OUT" && sha256sum libvulkan_powervr_mesa.so "${TESTS[@]}" \
			"${TLS_CHECK[@]}" \
			libEGL_mesa.so.0 libvulkan.so.1 "${GL_TESTS[@]}" 10_mesa.json)
	} > "$OUT/MANIFEST"
	log "results in $OUT"
	ls -l "$OUT"
}

# -- host smoke test (optional, not part of "all") ----------------------------
# The same patched source built for Linux (the Haiku hunks compile out) with
# Mesa's pvr drm-shim, which answers DEV_QUERY like the Linux kernel driver
# for any BVNC. The tests run on the build host against BXM-4-64
# 36.56.104.183 under an ioctl tracer: the driver's own code (enumeration,
# device creation, the PowerVR shader compiler, command streams, null jobs)
# runs, and every ioctl it makes is logged to $ROOT/shim/<test>.log, in
# order, with its flags. Nothing executes: the shim ignores SUBMIT_JOBS and
# its syncobj ioctls succeed at once, so pvr_vkfill finds every word
# unwritten and pvr_vkfence's waits that must time out do not.
SHIM_BVNC=36.56.104.183

# From a trace with "== frame N" or "== dispatch N" marks: what each frame
# from 10 up to 60 (or the end of the loop) creates and destroys, per kind
# of object, and the other requests per frame. A kind whose net count grows
# in both halves of the window piles up.
trace_balance() {
	awk '
	BEGIN {
		pairs["PVR_CREATE_BO"] = "GEM_CLOSE"
		pairs["PVR_VM_MAP"] = "PVR_VM_UNMAP"
		pairs["CPU_MAP"] = "CPU_UNMAP"
		pairs["SYNCOBJ_CREATE"] = "SYNCOBJ_DESTROY"
		pairs["PVR_CREATE_FREE_LIST"] = "PVR_DESTROY_FREE_LIST"
		pairs["PVR_CREATE_HWRT_DATASET"] = "PVR_DESTROY_HWRT_DATASET"
		pairs["PVR_CREATE_CONTEXT"] = "PVR_DESTROY_CONTEXT"
		pairs["PVR_CREATE_VM_CONTEXT"] = "PVR_DESTROY_VM_CONTEXT"
		for (c in pairs)
			destroys[pairs[c]] = c
		first = 10; last = 60; half = (first + last) / 2
	}
	/^== (frame|dispatch) [0-9]+/ {
		frame = $3 + 0
		if (frame == first)
			counting = 1
		if (frame >= last)
			counting = 0
		next
	}
	/ (frames|dispatches) in / { counting = 0 }
	!counting || !/^(ioctl|mmap|munmap) / || / = -[0-9]+/ { next }
	{
		name = $3
		part = frame < half ? 1 : 2
		count[name]++
		if (name in pairs)
			net[name, part]++
		else if (name in destroys)
			net[destroys[name], part]--
		if (name == "PVR_SUBMIT_JOBS")
			jobs += $4
	}
	END {
		frames = last - first
		printf "%-26s %9s %9s %14s\n", "object (create/destroy)",
			"made/fr", "freed/fr", "net 1st/2nd"
		for (c in pairs) {
			made = count[c] + 0; freed = count[pairs[c]] + 0
			if (made + freed == 0)
				continue
			flag = net[c, 1] > 0 && net[c, 2] > 0 ? "  PILES UP" : ""
			printf "%-26s %9.2f %9.2f %6d / %-6d%s\n", c, made / frames,
				freed / frames, net[c, 1], net[c, 2], flag
		}
		printf "other requests per frame:"
		for (n in count) {
			if (!(n in pairs) && !(n in destroys))
				printf " %s %.2f", n, count[n] / frames
		}
		printf "; jobs %.2f\n", jobs / frames
	}' "$1"
}

shim() {
	local sb=$ROOT/build-shim reconfigure=()
	[ ! -f "$sb/build.ninja" ] || reconfigure=(--reconfigure)
	meson setup "${reconfigure[@]}" "$sb" "$SRC" --buildtype=debugoptimized \
		--wrap-mode=nofallback -Dplatforms= -Dgallium-drivers=zink,softpipe \
		-Dvulkan-drivers=imagination -Dtools=drm-shim -Dopengl=true \
		-Dgles1=disabled -Dgles2=enabled -Degl=enabled -Dglx=disabled \
		-Dgbm=disabled -Dglvnd=disabled -Dllvm=disabled -Dmesa-clc=system \
		-Dprecomp-compiler=system -Dspirv-tools=disabled \
		-Dshader-cache=disabled -Dvalgrind=disabled -Dlibunwind=disabled \
		-Dzstd=disabled -Dexpat=disabled -Dxmlconfig=disabled \
		-Dbuild-tests=false '-Dvulkan-layers=[]'
	ninja -C "$sb" -j"$JOBS"
	local lib=$sb/src/imagination/vulkan s=$ROOT/shim
	mkdir -p "$s/lib"
	cc -shared -fPIC -O2 -Wall -I"$SRC/include" -o "$s/libpvr_ioctl_trace.so" \
		"$TOOLS/pvr_ioctl_trace.c" -ldl -lpthread
	local preload=$s/libpvr_ioctl_trace.so:$sb/src/imagination/drm-shim/libpowervr_noop_drm_shim.so
	for t in "${TESTS[@]}"; do
		cc -std=gnu11 -O2 -g -Wall -I"$SRC/include" -o "$s/$t" "$TOOLS/$t.c" \
			-L"$lib" -lvulkan_powervr_mesa -Wl,-rpath,"$lib"
	done
	# OpenGL ES through zink: EGL without a window system (surfaceless) on
	# the shim's "powervr" render node, which Mesa gives to zink (forced:
	# the shim's renderD128 may be a real GPU on the host as well); zink
	# loads the Vulkan driver through this tree's libvulkan.so.1. (Haiku
	# makes zink without a DRM device; on Linux that path asserts in
	# driconf, so this is the closest.)
	cc -std=gnu11 -O2 -Wall -shared -fPIC -fvisibility=hidden \
		-Wl,-soname,libvulkan.so.1 -I"$SRC/include" -o "$s/lib/libvulkan.so.1" \
		"$TOOLS/vulkan_shim.c" -L"$lib" -lvulkan_powervr_mesa -Wl,-rpath,"$lib"
	for t in "${GL_TESTS[@]}"; do
		cc -std=gnu11 -O2 -g -Wall -I"$SRC/include" -o "$s/$t" "$TOOLS/$t.c" \
			"$sb/src/egl/libEGL.so" "$sb/src/mesa/glapi/es2api/libGLESv2.so" \
			-Wl,-rpath,"$sb/src/egl:$sb/src/mesa/glapi/es2api" -lm -ldl
	done
	cc -std=gnu11 -O2 -Wall -fPIC -shared -DTLS_GENERATION_LIBRARY \
		-o "$s/${TLS_CHECK[1]}" "$TOOLS/tls_generation_check.c"
	cc -std=gnu11 -O2 -Wall -o "$s/${TLS_CHECK[0]}" \
		"$TOOLS/tls_generation_check.c" -ldl -lpthread
	# Every thread gets Haiku's default stack, 256 KiB (glibc sizes thread
	# stacks from RLIMIT_STACK; the Haiku-only 8 MiB for Mesa's own threads
	# does not apply here): a frame too large for an application's thread
	# crashes here as on the board.
	# The memory types are the board's: the host-cached one (on by default
	# only on Haiku) and zink's staging in it; host memory is coherent, so
	# the cache maintenance is a no-op here.
	local run name scanout
	for run in tls_generation_check \
		pvr_vkprobe pvr_vkfill "pvr_vkfill 1 5000 --cached" pvr_vkfence \
		pvr_vktriangle pvr_vkhang "pvr_vktriangle --linear" \
		"pvr_vkbench --seconds 3 --interval 1" \
		"pvr_vkbench --seconds 3 --interval 1 --timeline --rerecord" \
		"pvr_glprobe --expect zink" \
		"pvr_glprobe --expect zink --repeat 3" "pvr_glreset --frames 100" \
		"pvr_present --expect-none" "SCANOUT=640x480 pvr_present --shim" \
		"SCANOUT=1920x1080 pvr_copybench --runs 2" \
		"pvr_glpresent --shim --front-bpr 4096 --front-refused" \
		"SCANOUT=640x480 pvr_glpresent --shim --front-bpr 2560" \
		"pvr_glbench --seconds 3 --interval 1 --expect zink" \
		"pvr_glcomposite --frames 2 --shim --expect zink" \
		"pvr_vkbench --dispatches 60 --timeline --rerecord --mark" \
		"pvr_glbench --frames 60 --mark" \
		"pvr_glbench --frames 60 --resize 5 --mark"; do
		# SCANOUT=WxH: the driver's stand-in frame buffer for the direct
		# present (PVR_SHIM_SCANOUT)
		scanout=
		case "$run" in
		SCANOUT=*) scanout=${run%% *}; scanout=${scanout#SCANOUT=}
			run=${run#* } ;;
		esac
		name=${run%% *}
		case "$run" in
		*--resize*--mark) name=balance-resize-$name ;;
		*--mark) name=balance-$name ;;
		*--timeline*) name=$name-timeline-rerecord ;;
		*--repeat*) name=$name-repeat ;;
		*--linear) name=$name-linear ;;
		*--cached) name=$name-cached ;;
		*--expect-none) name=$name-none ;;
		*--expect*) name=$name-expect-zink ;;
		esac
		[ -z "$scanout" ] || name=$name-scanout
		(ulimit -s 256
		[ -z "$scanout" ] || export PVR_SHIM_SCANOUT=$scanout
		PVR_SHIM_DEVICE_BVNC=$SHIM_BVNC PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 \
			PVR_CACHED_MEMORY_TYPE=1 ZINK_NONCOHERENT_CACHED_STAGING=1 \
			EGL_PLATFORM=surfaceless MESA_LOADER_DRIVER_OVERRIDE=zink \
			LD_LIBRARY_PATH="$s/lib:$LD_LIBRARY_PATH" \
			LD_PRELOAD=$preload timeout 120 "$s/"$run > "$s/$name.log" 2>&1) \
			|| true
		printf '%-30s %5s ioctls, last line: %s\n' "$name" \
			"$(grep -c '^ioctl ' "$s/$name.log")" "$(tail -1 "$s/$name.log")"
	done
	# objects made per frame (dispatch) that are not destroyed again, in the
	# steady state: frames 10 to 59
	for name in balance-pvr_glbench balance-resize-pvr_glbench \
		balance-pvr_vkbench; do
		trace_balance "$s/$name.log" > "$s/$name.txt"
		log "$name: per frame, frames 10-59 ($s/$name.txt)"
		cat "$s/$name.txt"
	done
	# A GPU reset as Mesa sees it: from the N-th SUBMIT_JOBS on, every one
	# fails with EIO (the tracer injects it). The driver must report the
	# device lost and each program end instead of waiting forever (timeout's
	# 124): pvr_vkfill and pvr_glprobe at the first submit, pvr_glreset (a
	# read of what an ended batch drew, as the BGLView present does) in the
	# middle of its frames.
	local rc fail
	for run in "1 pvr_vkfill" "1 pvr_glprobe --expect zink" \
		"10 pvr_glreset --frames 300" \
		"6 SCANOUT=640x480 pvr_glpresent --shim --lost --front-bpr 2560"; do
		fail=${run%% *}
		run=${run#* }
		scanout=
		case "$run" in
		SCANOUT=*) scanout=${run%% *}; scanout=${scanout#SCANOUT=}
			run=${run#* } ;;
		esac
		name=lost-${run%% *}
		if (ulimit -s 256
			[ -z "$scanout" ] || export PVR_SHIM_SCANOUT=$scanout
			PVR_TRACE_FAIL_SUBMIT=$fail PVR_SHIM_DEVICE_BVNC=$SHIM_BVNC \
			PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 EGL_PLATFORM=surfaceless \
			PVR_CACHED_MEMORY_TYPE=1 ZINK_NONCOHERENT_CACHED_STAGING=1 \
			MESA_LOADER_DRIVER_OVERRIDE=zink \
			LD_LIBRARY_PATH="$s/lib:$LD_LIBRARY_PATH" \
			LD_PRELOAD=$preload timeout 60 "$s/"$run > "$s/$name.log" 2>&1)
		then
			rc=0
		else
			rc=$?
		fi
		printf '%-30s exit %3s%s, last line: %s\n' "$name" "$rc" \
			"$([ "$rc" = 124 ] && echo ' (HUNG)')" "$(tail -1 "$s/$name.log")"
	done
	log "logs in $s"
}

case "${1:-all}" in
shim) fetch; patch_source; host_tools; shim ;;
fetch) fetch ;;
host) fetch; patch_source; host_tools ;;
patch) fetch; patch_source ;;
configure) fetch; patch_source; host_tools; configure ;;
driver) fetch; patch_source; host_tools; driver ;;
tests) tests ;;
gl) fetch; patch_source; host_tools; driver; gl_configure; gl ;;
all) fetch; patch_source; host_tools; configure; driver; tests; gl_configure
	gl; results ;;
*) echo "usage: $0 [all|fetch|host|patch|configure|driver|tests|gl|shim]" >&2
	exit 2 ;;
esac
