#!/usr/bin/env bash
# build-arm64-app-packages.sh - cross-build the owner's applications into
# arm64 Haiku packages (.hpkg) for the air/OS image of the ROCK 5 ITX.
#
# Usage:
#   tools/airos/build-arm64-app-packages.sh [OUTPUT_DIR] [APP ...]
#
#   OUTPUT_DIR  where the packages go (default /mnt/HaikuWork/airos/packages-arm64)
#   APP         any of: airshot clipper burrow kiri lcdmonitor amp turbochook natter
#               airtop summit_webkit summit airpins rpi_installer
#               (default: all of them but airpins and rpi_installer; these
#               board utilities (Raspberry Pi, Cubie A7S) are built only
#               when named)
#
# Environment overrides (all optional):
#   JOBS=8                       parallel compile jobs
#   APPS=/mnt/HaikuWork/apps     where the application repositories live
#   APPBUILD=/mnt/HaikuWork/airos/appbuild
#                                work tree: snapshots, objects, staging
#   SYSROOT=...                  arm64 Haiku sysroot (default: Summit's arm64
#                                sysroot, the most complete one: libmedia, libgame,
#                                private headers, libshared.a)
#   EXTRA_DEPS=...               cross-built TagLib 2, SQLite, PCRE2, Scintilla,
#                                Lexilla (same binaries as rock5-image-extras/lib)
#   TLS_DEPS=...                 Summit's arm64 deps: curl 8.10.1 built with
#                                OpenSSL 3.3.2 and nghttp2, the SOCK_NONBLOCK and
#                                armcap SIGBUS fixes; libcurl/libssl/libcrypto
#                                come from here because the bootstrap libcurl in
#                                rock5-image-extras has no TLS at all
#   NATTER_WEB_SIGNIN=auto|1|0   Natter's "Sign in with Slack's web page" needs
#                                Summit's WebKit engine. auto (default): use it
#                                when SUMMIT_ENGINE_LOG ends with "ninja exit 0";
#                                1: require it; 0: build without it
#   SUMMIT_ENGINE=...            engine build dir with lib/libWebKit.so
#   SUMMIT_ENGINE_LOG=...        its ninja log
#   SUMMIT_WEBKIT_SRC=...        Source/WebKit of the engine tree (API headers)
#   SUMMIT_ENGINE_EXTRA_DEPS=... a second dependency prefix, for an engine built
#                                with GL compositing and WebGL (see
#                                tools/rpi4/summit/build-gl-deps.sh)
#
# For every application this:
#   1. snapshots the working tree of the repository (tracked files plus
#      untracked, non-ignored ones: what `git status` calls the current source)
#      into $APPBUILD/<app>/src. Nothing is ever written into the repositories,
#      and other people may keep editing them while this runs;
#   2. cross-compiles it with the arm64 cross compiler. The sysroot goes on
#      CXX itself, because the Makefiles link with $(CXX) and no flags. rc and
#      xres are the host builds; mimeset in the Makefiles is skipped;
#   3. stages the package exactly as the app's own tools/package-haiku.sh does
#      (documentation, licences, data files, post-install scripts with mode
#      755, the data/deskbar/menu/Applications symlink), then strips, puts the
#      resources back with xres (GNU strip drops them), copies them into
#      attributes with `resattr -O` (Tracker and Deskbar read the icon and
#      signature from attributes) and runs the host mimeset --all with a
#      staged data/mime_db, as the native scripts do;
#   4. retargets .PackageInfo to arm64 and cuts `requires` down to
#      `haiku >= r1~beta6` (Natter keeps summit_webkit when it links the
#      engine). The board has no repository for lib:/cmd: providers and an
#      unresolvable package is deactivated at boot; the libraries go to
#      system/non-packaged/lib instead. Every dropped requirement is printed;
#   5. creates <name>-<version>-arm64.hpkg in OUTPUT_DIR (older versions of
#      the same package there are removed), checks it with `package list`
#      and prints the non-base shared libraries the binaries need with the
#      exact file each was linked against.
#
# The host tools keep Haiku attributes in $ATTRS keyed by inode number, so a
# new file can inherit attributes of a deleted one. Staging therefore removes
# such stale attributes, deletes stages with rm_attrs, and refuses a package
# with attributes anywhere but on applications and MIME DB entries.
#
# Re-running is safe: snapshots and stages are rebuilt, objects are kept and
# make rebuilds only what changed.
set -euo pipefail
umask 022

WORK=/mnt/HaikuWork
OUT=${1:-$WORK/airos/packages-arm64}
[[ $# -gt 0 ]] && shift
SELECTED=("$@")

APPS=${APPS:-$WORK/apps}
APPBUILD=${APPBUILD:-$WORK/airos/appbuild}
JOBS=${JOBS:-8}
CROSS=${CROSS:-$WORK/build/arm64/cross-tools-arm64/bin/aarch64-unknown-haiku-}
TOOLS=${TOOLS:-$WORK/build/arm64/objects/linux/x86_64/release/tools}
ATTRS=${ATTRS:-$WORK/build/arm64/attributes}
# The full system MIME DB of the arm64 build: supported types already known
# there must not get entries (or preferred apps) of their own in the package.
MIMEDB=${MIMEDB:-$WORK/build/arm64/objects/common/data/mime_db/mime_db}
SYSROOT=${SYSROOT:-$WORK/build/summit-arm64/sysroot}
EXTRA_DEPS=${EXTRA_DEPS:-$WORK/tmp/takeover/arm64-deps/install/boot/system}
TLS_DEPS=${TLS_DEPS:-$WORK/build/summit-arm64/deps}
SUMMIT_ENGINE=${SUMMIT_ENGINE:-/home/jmgasper/summit-arm64-build/WebKitBuild}
SUMMIT_ENGINE_LOG=${SUMMIT_ENGINE_LOG:-$WORK/build/summit-arm64/ninja-81dffc7.log}
SUMMIT_WEBKIT_SRC=${SUMMIT_WEBKIT_SRC:-$WORK/build/summit-arm64/WebKit/Source/WebKit}
NATTER_WEB_SIGNIN=${NATTER_WEB_SIGNIN:-auto}
# Where the summit_webkit package installs the engine (lib/ holds libWebKit).
ENGINE_DIR=/boot/system/lib/summit-webkit

export TMPDIR=$WORK/tmp
HOSTBIN=$APPBUILD/.hostbin
FARM=$APPBUILD/.deps-arm64
# The arm64 cross compiler links only libgcc.a, which carries a private copy
# of the unwinder: the program's frames are registered with that copy while
# libstdc++'s __cxa_throw unwinds with libgcc_s.so.1, finds no frames and
# calls std::terminate. Every C++ exception aborted (Summit died at start on a
# damaged profile.json). These specs link libgcc_s ahead of libgcc.
UNWIND_SPECS=$HOSTBIN/shared-unwinder.specs
CXX_ARM64="${CROSS}g++ --sysroot=$SYSROOT -specs=$UNWIND_SPECS -L$FARM/lib -Wl,-rpath-link,$TLS_DEPS/lib"
CC_ARM64="${CROSS}gcc --sysroot=$SYSROOT -specs=$UNWIND_SPECS -L$FARM/lib -Wl,-rpath-link,$TLS_DEPS/lib"
STRIP=${CROSS}strip

die() { echo "error: $*" >&2; exit 1; }
note() { printf '\n== %s\n' "$*"; }

check_prerequisites() {
	local required
	for required in "${CROSS}g++" "$TOOLS/rc/rc" "$TOOLS/xres" "$TOOLS/resattr/resattr" \
			"$TOOLS/mimeset" "$TOOLS/rm_attrs" "$TOOLS/package/package" "$MIMEDB" \
			"$SYSROOT/boot/system/develop/lib/libbe.so" "$EXTRA_DEPS/lib/libtag.so.2" \
			"$TLS_DEPS/lib/libcurl.so.4" "$ATTRS"; do
		[[ -e $required ]] || die "missing $required"
	done
	command -v rsync >/dev/null || die "rsync is needed"
}

# rc and xres by name are the host builds; mimeset in the Makefiles is skipped
# (it writes attributes on build outputs, and staging does it properly).
setup_host_tools() {
	mkdir -p "$HOSTBIN"
	printf '#!/bin/sh\nexec "%s" "$@"\n' "$TOOLS/rc/rc" > "$HOSTBIN/rc"
	printf '#!/bin/sh\nexec "%s" "$@"\n' "$TOOLS/xres" > "$HOSTBIN/xres"
	printf '#!/bin/sh\nexit 0\n' > "$HOSTBIN/mimeset"
	chmod 755 "$HOSTBIN/rc" "$HOSTBIN/xres" "$HOSTBIN/mimeset"
	printf '*libgcc:\n-lgcc_s -lgcc\n\n' > "$UNWIND_SPECS"
	export PATH="$HOSTBIN:$PATH"
}

# One directory with exactly the third-party libraries and headers the apps
# may link, so it is unambiguous which arm64 file each -l resolves to.
setup_dependency_farm() {
	rm -rf "$FARM"
	mkdir -p "$FARM/lib" "$FARM/include"
	local x=$EXTRA_DEPS t=$TLS_DEPS
	ln -s "$t/lib/libcurl.so.4" "$FARM/lib/libcurl.so"
	ln -s "$t/lib/libssl.so.3" "$FARM/lib/libssl.so"
	ln -s "$t/lib/libcrypto.so.3" "$FARM/lib/libcrypto.so"
	ln -s "$x/lib/libtag.so.2" "$FARM/lib/libtag.so"
	ln -s "$x/lib/libsqlite3.so.0" "$FARM/lib/libsqlite3.so"
	ln -s "$x/lib/libpcre2-8.so.0" "$FARM/lib/libpcre2-8.so"
	ln -s "$x/lib/libscintilla.so" "$FARM/lib/libscintilla.so"
	ln -s "$x/lib/liblexilla.so" "$FARM/lib/liblexilla.so"
	ln -s "$t/include/curl" "$FARM/include/curl"
	ln -s "$t/include/openssl" "$FARM/include/openssl"
	ln -s "$x/develop/headers/taglib" "$FARM/include/taglib"
	ln -s "$x/develop/headers/sqlite3.h" "$FARM/include/sqlite3.h"
	ln -s "$x/develop/headers/sqlite3ext.h" "$FARM/include/sqlite3ext.h"
	ln -s "$x/develop/headers/pcre2.h" "$FARM/include/pcre2.h"
	ln -s "$x/develop/headers/scintilla" "$FARM/include/scintilla"
	ln -s "$x/develop/headers/lexilla" "$FARM/include/lexilla"
}
FARM_CPATH() { echo "$FARM/include:$FARM/include/scintilla:$FARM/include/lexilla"; }

# snapshot <repo dir name> <work name>: sets SRC, BUILDDIR, STAGE.
snapshot() {
	local repo=$APPS/$1 work=$APPBUILD/$2
	[[ -d $repo ]] || die "no repository $repo"
	SRC=$work/src BUILDDIR=$work/build-arm64 STAGE=$work/stage
	rm -rf "$SRC"
	mkdir -p "$SRC" "$BUILDDIR"
	if git -C "$repo" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
		git -C "$repo" ls-files -z --cached --others --exclude-standard \
			| rsync -a --from0 --files-from=- --ignore-missing-args "$repo/" "$SRC/"
		# --no-optional-locks: never touch the index others may be using.
		echo "snapshot of $repo at $(git -C "$repo" rev-parse --short HEAD)$(
			[[ -n $(git --no-optional-locks -C "$repo" status --porcelain) ]] \
				&& echo ' plus uncommitted changes')"
	else
		rsync -a --exclude='/build-*' --exclude='/artifacts' "$repo/" "$SRC/"
		echo "snapshot of $repo (not a git repository)"
	fi
	# Objects live outside the snapshot so rebuilds stay incremental.
	rm -rf "$SRC/build-arm64"
	ln -s "$BUILDDIR" "$SRC/build-arm64"
}

stage_begin() {
	[[ -e $STAGE ]] && "$TOOLS/rm_attrs" -rf "$STAGE"
	mkdir -p "$STAGE"
}

stage_end() {
	"$TOOLS/rm_attrs" -rf "$STAGE"
}

# install_binary <built binary> <rsrc or -> <path in package>
install_binary() {
	local dest=$STAGE/$3
	mkdir -p "$(dirname "$dest")"
	cp "$1" "$dest"
	chmod 755 "$dest"
	"$STRIP" --strip-debug "$dest"
	# GNU strip drops the appended Haiku resources; put them back.
	[[ $2 == - ]] || "$TOOLS/xres" -o "$dest" "$2"
}

# install_file <mode> <source> <path in package>
install_file() {
	mkdir -p "$(dirname "$STAGE/$3")"
	cp -R "$2" "$STAGE/$3"
	chmod "$1" "$STAGE/$3"
}

docs() { # docs <package name> <source paths relative to SRC>...
	local name=$1 path
	shift
	mkdir -p "$STAGE/documentation/packages/$name"
	for path in "$@"; do
		mkdir -p "$STAGE/documentation/packages/$name/$(dirname "$path")"
		cp -R "$SRC/$path" "$STAGE/documentation/packages/$name/$(dirname "$path")/"
	done
}

deskbar_link() { # deskbar_link <path in package of the app> <menu name>
	mkdir -p "$STAGE/data/deskbar/menu/Applications"
	ln -s "../../../../$1" "$STAGE/data/deskbar/menu/Applications/$2"
}

# Stale attributes: any attribute file of a staged node older than $1.
purge_stale_attributes() {
	local marker=$1 entry inode
	while IFS= read -r -d '' entry; do
		inode=$(stat -c %i "$entry")
		[[ -d $ATTRS/$inode ]] || continue
		find "$ATTRS/$inode" -mindepth 1 ! -newer "$marker" -exec rm -rf {} +
	done < <(find "$STAGE" -print0)
}

# attribute_resources <rsrc> <output>: only the BEOS:* resources (signature,
# flags, icon, version, supported types). resattr copies every resource, and an
# app's own data (Amp's MiniDisc PNGs) does not belong in its attributes.
attribute_resources() {
	local type id
	cp "$1" "$2.in"
	while IFS=$'\t' read -r type id; do
		"$TOOLS/xres" -o "$2.out" -d "$type:$id" "$2.in"
		mv "$2.out" "$2.in"
	done < <("$TOOLS/xres" -l "$1" | awk '
		/^'"'"'/ {
			type = substr($0, 2, 4); split(substr($0, 7), f, " ")
			name = substr($0, 7); sub(/^ *[^ ]+ +[^ ]+ +/, "", name)
			if (name !~ /^BEOS:/) print type "\t" f[1]
		}')
	mv "$2.in" "$2"
}

# add_attributes <rsrc> <app path in package>...: resattr + mimeset for the
# applications in the stage, as mimeset does on Haiku.
add_attributes() {
	local rsrc=$APPBUILD/.attributes.rsrc marker=$APPBUILD/.attr-marker
	attribute_resources "$1" "$rsrc"
	shift
	touch "$marker"
	sleep 0.05
	purge_stale_attributes "$marker"
	local app
	for app in "$@"; do
		"$TOOLS/resattr/resattr" -O -o "$STAGE/$app" "$rsrc"
	done
	( cd "$STAGE" && "$TOOLS/mimeset" --all -f --mimedb data/mime_db --mimedb "$MIMEDB" "$@" )
	purge_stale_attributes "$marker"
}

# retarget_package_info <source> <destination> [<regex of requires to keep>]
retarget_package_info() {
	local dropped=$APPBUILD/.dropped
	: > "$dropped"
	awk -v keep="${3:-}" -v dropped="$dropped" '
		/^architecture[ \t]/   { print "architecture arm64"; next }
		/^requires[ \t]*\{/    { print; print "\thaiku >= r1~beta6"; inreq = 1; next }
		inreq && /^[ \t]*\}/   { print; inreq = 0; next }
		inreq {
			line = $0
			gsub(/^[ \t]+|[ \t]+$/, "", line)
			if (line == "") next
			if (keep != "" && line ~ keep) { print "\t" line; next }
			if (line !~ /^haiku([ \t<>=]|$)/) print line > dropped
			next
		}
		{ print }
	' "$1" > "$2"
	DROPPED=$(paste -sd ',' "$dropped" | sed 's/,/, /g')
	[[ -n $DROPPED ]] || DROPPED="(none)"
}

info_field() { awk -v k="$1" '$1 == k { print $2; exit }' "$2"; }

# Non-base shared libraries of a staged binary and the file each was linked
# against (base = anything in the sysroot's system/lib or develop/lib).
soname() {
	local name
	name=$(readelf -d "$1" | sed -n 's/.*(SONAME).*\[\(.*\)\]/\1/p')
	echo "${name:-$(basename "$1")}"
}

needed_libraries() {
	local binary=$1 needed path lib
	for needed in $(readelf -d "$binary" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
		[[ -e $SYSROOT/boot/system/lib/$needed || -e $SYSROOT/boot/system/develop/lib/$needed ]] \
			&& continue
		# The farm entry with that soname is what -l linked; else the engine.
		path=""
		for lib in "$FARM"/lib/*.so; do
			[[ $(soname "$lib") == "$needed" ]] && { path=$(readlink -f "$lib"); break; }
		done
		[[ -z $path && -e $SUMMIT_ENGINE/lib/$needed ]] \
			&& path="$(readlink -f "$SUMMIT_ENGINE/lib/$needed") (Summit engine)"
		printf '    %-20s %s\n' "$needed" "${path:-NOT FOUND}"
		# What libcurl itself pulls in.
		if [[ $needed == libcurl.so.4 && -n $path ]]; then
			for needed in $(readelf -d "$path" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
				[[ -e $SYSROOT/boot/system/lib/$needed ]] && continue
				printf '    %-20s %s (needed by libcurl)\n' "$needed" "$(readlink -f "$TLS_DEPS/lib/$needed")"
			done
		fi
	done
}

# finish_package <package name> <binaries in package for the library report>...
finish_package() {
	local name=$1 version file
	shift
	version=$(info_field version "$STAGE/.PackageInfo")
	file=$OUT/$name-$version-arm64.hpkg
	mkdir -p "$OUT"
	rm -f "$OUT/$name"-[0-9]*-arm64.hpkg
	"$TOOLS/package/package" create -q -C "$STAGE" "$file"
	verify_package "$file"
	echo "package:  $file"
	echo "version:  $version"
	echo "dropped:  $DROPPED"
	echo "libraries outside the base system:"
	local binary report=""
	for binary in "$@"; do
		report+=$(needed_libraries "$STAGE/$binary")$'\n'
	done
	report=$(printf '%s' "$report" | sed '/^$/d' | awk '!seen[$1]++' | sort)
	echo "${report:-    (none)}"
	printf '%s\t%s\t%s\t%s\n' "$name" "$file" "$version" "$DROPPED" >> "$APPBUILD/.summary"
	stage_end
}

# The package must be arm64, and attributes may only sit on applications,
# add-ons and MIME DB entries (anything else would be stale host state).
verify_package() {
	local file=$1 tool=$TOOLS/package/package
	"$tool" list -i "$file" | grep -q '^[[:space:]]*architecture: arm64$' \
		|| die "$file is not arm64"
	"$tool" list -a "$file" | awk -v file="$file" '
		/^[ \t]*</ {
			attr = $0; sub(/^[ \t]*</, "", attr); sub(/[ \t].*/, "", attr)
			ok = (path ~ /^(apps|add-ons)\// && attr ~ /^BEOS:/) \
				|| (path ~ /^data\/mime_db\// && attr ~ /^META:/)
			if (!ok) { print "unexpected attribute " attr " on " path " in " file > "/dev/stderr"; bad = 1 }
			next
		}
		/^[ \t]*[^ \t]/ && started {
			match($0, /^ */); depth = RLENGTH / 2
			entry = substr($0, RLENGTH + 1); sub(/[ \t][ \t]+.*/, "", entry)
			stack[depth] = entry; path = stack[0]
			for (i = 1; i <= depth; i++) path = path "/" stack[i]
			next
		}
		/^[^ \t]/ { started = 1; match($0, /^ */); stack[0] = $1; path = $1 }
		END { exit bad }
	' || die "$file carries unexpected attributes"
}

wanted() {
	[[ ${#SELECTED[@]} -eq 0 ]] && return 0
	local app
	for app in "${SELECTED[@]}"; do [[ ${app,,} == "$1" ]] && return 0; done
	return 1
}

# --- airShot ----------------------------------------------------------------
build_airshot() {
	note airShot
	snapshot airShot airshot
	local private=$SYSROOT/boot/system/develop/headers/private
	make -C "$SRC" -s -j"$JOBS" BUILD=build-arm64 CXX="$CXX_ARM64" \
		APP_CPPFLAGS="-I$private/interface -I$private/shared -I$private/app" \
		FILTER_CPPFLAGS="-I$private/storage" \
		RC="$TOOLS/rc/rc" XRES="$TOOLS/xres" MIMESET=true all
	stage_begin
	install_binary "$BUILDDIR/airShot" "$BUILDDIR/airShot.rsrc" apps/airShot
	install_binary "$BUILDDIR/airShot_filter" - add-ons/input_server/filters/airShot
	docs airshot README.md LICENSE docs
	# The Font Awesome tool icons (working tree since 0.1.0~beta-2).
	if [[ -d $SRC/resources/icons ]]; then
		cp -R "$SRC/resources/icons" "$STAGE/documentation/packages/airshot/"
		install_file 644 "$SRC/resources/icons/fontawesome/CC-BY-4.0.txt" "data/licenses/CC BY 4.0"
	fi
	deskbar_link apps/airShot airShot
	retarget_package_info "$SRC/resources/airShot.PackageInfo" "$STAGE/.PackageInfo"
	add_attributes "$BUILDDIR/airShot.rsrc" apps/airShot
	finish_package airshot apps/airShot add-ons/input_server/filters/airShot
}

# --- Clipper ----------------------------------------------------------------
# The clipboard manager: the application and its two input_server add-ons
# (global shortcuts, paste injection), staged as its tools/package-haiku.sh
# does.
build_clipper() {
	note Clipper
	snapshot clipper clipper
	local private=$SYSROOT/boot/system/develop/headers/private
	make -C "$SRC" -s -j"$JOBS" BUILD=build-arm64 CXX="$CXX_ARM64" \
		APP_CPPFLAGS="-I$private/interface" \
		RC="$TOOLS/rc/rc" XRES="$TOOLS/xres" MIMESET=true all
	stage_begin
	install_binary "$BUILDDIR/Clipper" "$BUILDDIR/Clipper.rsrc" apps/Clipper
	install_binary "$BUILDDIR/Clipper_filter" - add-ons/input_server/filters/Clipper_shortcuts
	install_binary "$BUILDDIR/Clipper_device" - add-ons/input_server/devices/Clipper_paste
	docs clipper README.md LICENSE docs
	install_file 644 "$SRC/LICENSE" data/licenses/MIT
	deskbar_link apps/Clipper Clipper
	# its requires list is on one line; the retargeting reads one per line
	sed 's/^requires { *\(.*\) *}$/requires {\n\t\1\n}/' \
		"$SRC/resources/Clipper.PackageInfo" > "$BUILDDIR/Clipper.PackageInfo"
	retarget_package_info "$BUILDDIR/Clipper.PackageInfo" "$STAGE/.PackageInfo"
	add_attributes "$BUILDDIR/Clipper.rsrc" apps/Clipper
	finish_package clipper apps/Clipper add-ons/input_server/filters/Clipper_shortcuts \
		add-ons/input_server/devices/Clipper_paste
}

# --- AirPins ----------------------------------------------------------------
# The Raspberry Pi's GPIO tool (a native version of pigg). It talks to the
# rpi_gpio driver of the rpi4 image and simulates the pins anywhere else, so
# it goes into the Raspberry Pi 4 packages (/mnt/HaikuWork/rpi4/packages-arm64).
build_airpins() {
	note AirPins
	snapshot airpins airpins
	local private=$SYSROOT/boot/system/develop/headers/private
	make -C "$SRC" -s -j"$JOBS" BUILD=build-arm64 CXX="$CXX_ARM64" \
		APP_CPPFLAGS="-I$private/interface -I$private/shared" \
		RC="$TOOLS/rc/rc" XRES="$TOOLS/xres" MIMESET=true all
	stage_begin
	install_binary "$BUILDDIR/AirPins" "$BUILDDIR/AirPins.rsrc" apps/AirPins
	docs airpins README.md LICENSE third_party/pigg/LICENSE \
		resources/icons/README.md resources/icons/fontawesome/LICENSE.txt \
		resources/icons/fontawesome/CC-BY-4.0.txt
	deskbar_link apps/AirPins AirPins
	retarget_package_info "$SRC/resources/AirPins.PackageInfo" "$STAGE/.PackageInfo"
	add_attributes "$BUILDDIR/AirPins.rsrc" apps/AirPins
	finish_package airpins apps/AirPins
}

build_airtop() {
	note AirTop
	snapshot AirTop airtop
	# BAuxDisplay (the auxiliary display panel) needs the sysroot's libdevice
	# and headers from 2026-10-08 or later
	make -C "$SRC" -s -j"$JOBS" BUILD=build-arm64 CXX="$CXX_ARM64" \
		RC="$TOOLS/rc/rc" XRES="$TOOLS/xres" MIMESET=true all
	stage_begin
	install_binary "$BUILDDIR/AirTop" "$BUILDDIR/AirTop.rsrc" apps/AirTop
	docs airtop README.md LICENSE docs/THERMALS.md
	deskbar_link apps/AirTop AirTop
	retarget_package_info "$SRC/resources/AirTop.PackageInfo" "$STAGE/.PackageInfo"
	add_attributes "$BUILDDIR/AirTop.rsrc" apps/AirTop
	finish_package airtop apps/AirTop
}

# --- First-boot setup (Raspberry Pi and Cubie A7S SD card images) ------------
build_rpi_installer() {
	note RPiInstaller
	snapshot rpi-installer rpi_installer
	make -C "$SRC" -s -j"$JOBS" BUILD=build-arm64 CXX="$CXX_ARM64" \
		HAIKU_HEADERS="$SYSROOT/boot/system/develop/headers" \
		RC="$TOOLS/rc/rc" XRES="$TOOLS/xres" all
	stage_begin
	install_binary "$BUILDDIR/RPiInstaller" "$BUILDDIR/RPiInstaller.rsrc" apps/RPiInstaller
	docs rpi_installer README.md LICENSE
	deskbar_link apps/RPiInstaller "First-Boot Setup"
	retarget_package_info "$SRC/resources/RPiInstaller.PackageInfo" "$STAGE/.PackageInfo"
	add_attributes "$BUILDDIR/RPiInstaller.rsrc" apps/RPiInstaller
	finish_package rpi_installer apps/RPiInstaller
}

# --- Burrow -----------------------------------------------------------------
# The repository's own tools/build-arm64.sh builds burrow-openvpn (OpenSSL
# linked statically, LZO/LZ4 left out) and Burrow; it runs in the snapshot.
build_burrow() {
	note Burrow
	snapshot burrow burrow
	JOBS=$JOBS bash "$SRC/tools/build-arm64.sh" >/dev/null
	stage_begin
	install_binary "$BUILDDIR/Burrow" "$BUILDDIR/Burrow.rsrc" apps/Burrow
	install_binary "$BUILDDIR/burrow-openvpn" - bin/burrow-openvpn
	docs burrow README.md LICENSE docs
	# OpenVPN is GPL 2: ship how burrow-openvpn was made next to the binary.
	mkdir -p "$STAGE/documentation/packages/burrow/openvpn"
	cp -R "$SRC/openvpn/patches" "$SRC/openvpn/build.sh" "$SRC/openvpn/SOURCE.md" \
		"$SRC/tools/build-arm64.sh" "$STAGE/documentation/packages/burrow/openvpn/"
	deskbar_link apps/Burrow Burrow
	retarget_package_info "$SRC/resources/Burrow.PackageInfo" "$STAGE/.PackageInfo"
	add_attributes "$BUILDDIR/Burrow.rsrc" apps/Burrow
	finish_package burrow apps/Burrow bin/burrow-openvpn
}

# --- Kiri -------------------------------------------------------------------
build_kiri() {
	note Kiri
	snapshot kiri kiri
	CPATH=$(FARM_CPATH) make -C "$SRC" -s -j"$JOBS" BUILD=build-arm64 \
		CXX="$CXX_ARM64" CC="$CC_ARM64" all
	stage_begin
	install_binary "$BUILDDIR/Kiri" "$BUILDDIR/Kiri.rsrc" apps/Kiri
	docs kiri README.md LICENSE docs vendor/libvterm/LICENSE vendor/libvterm/UPSTREAM.md \
		vendor/nlohmann/LICENSE.MIT vendor/nlohmann/UPSTREAM.md \
		vendor/md4c/LICENSE.md vendor/md4c/UPSTREAM.md tools/install-language-tools.sh
	install_file 755 "$SRC/resources/kiri-post-install.sh" boot/post-install/kiri.sh
	deskbar_link apps/Kiri Kiri
	retarget_package_info "$SRC/resources/Kiri.PackageInfo" "$STAGE/.PackageInfo"
	add_attributes "$BUILDDIR/Kiri.rsrc" apps/Kiri
	finish_package kiri apps/Kiri
}

# --- LCDMonitor -------------------------------------------------------------
# No .PackageInfo, README or LICENSE in the tree; the package info below is
# made from resources/LCDMonitor.rdef (signature, version 0.1.0 development).
build_lcdmonitor() {
	note LCDMonitor
	snapshot lcdmonitor lcdmonitor
	make -C "$SRC" -s -j"$JOBS" BUILD=build-arm64 CXX="$CXX_ARM64" \
		HOST_TOOLS="$TOOLS/" RC="$TOOLS/rc/rc" XRES="$TOOLS/xres" all
	stage_begin
	install_binary "$BUILDDIR/LCDMonitor" "$BUILDDIR/LCDMonitor.rsrc" apps/LCDMonitor
	mkdir -p "$STAGE/documentation/packages/lcdmonitor"
	head -n 20 "$SRC/vendor/stb/stb_image_write.h" > "$STAGE/documentation/packages/lcdmonitor/stb_image_write-header.txt"
	deskbar_link apps/LCDMonitor LCDMonitor
	cat > "$APPBUILD/lcdmonitor/LCDMonitor.PackageInfo" <<'INFO'
name lcdmonitor
version 0.1.0~dev-1
architecture x86_64
summary "System monitor for the Thermalright Trofeo Vision 9.16 USB LCD"
description "LCDMonitor draws a system monitor (clock, uptime, CPU, memory and network graphs) on the Thermalright Trofeo Vision 9.16 USB LCD (USB 0416:5408) once a second. It runs in the background, talks to the panel through the USB Kit (no kernel driver) and reconnects when the panel comes back. Frames are sent rotated 180 degrees; rotate=0 in ~/config/settings/LCDMonitor turns that off. To start it at login, link /boot/system/apps/LCDMonitor into ~/config/settings/boot/launch."
packager "air/OS contributors"
vendor "air/OS"
copyrights { "2026 air/OS contributors" "2010-2015 Sean Barrett (stb_image_write, public domain)" }
licenses { "MIT" }
provides {
	lcdmonitor = 0.1.0~dev
	app:LCDMonitor = 0.1.0~dev
}
requires {
	haiku >= r1~beta6
}
INFO
	retarget_package_info "$APPBUILD/lcdmonitor/LCDMonitor.PackageInfo" "$STAGE/.PackageInfo"
	add_attributes "$BUILDDIR/LCDMonitor.rsrc" apps/LCDMonitor
	finish_package lcdmonitor apps/LCDMonitor
}

# --- Amp (apps/tasamp) --------------------------------------------------------
build_amp() {
	note Amp
	snapshot tasamp amp
	CPATH=$(FARM_CPATH) make -C "$SRC" -s -j"$JOBS" BUILD=build-arm64 CXX="$CXX_ARM64" all
	stage_begin
	install_binary "$BUILDDIR/Amp" "$BUILDDIR/Amp.rsrc" apps/Amp
	docs amp README.md LICENSE docs vendor/nlohmann/LICENSE.MIT vendor/nlohmann/UPSTREAM.md \
		vendor/fontawesome/LICENSE.txt vendor/fontawesome/UPSTREAM.md
	# each licence named in .PackageInfo needs its text inside the package
	install_file 644 "$SRC/vendor/fontawesome/SIL-OFL-1.1.txt" "data/licenses/SIL OFL 1.1"
	install_file 644 "$SRC/vendor/fontawesome/CC-BY-4.0.txt" "data/licenses/CC BY 4.0"
	# icons::Init() reads the Font Awesome face from B_SYSTEM_DATA_DIRECTORY/Amp.
	install_file 644 "$SRC/vendor/fontawesome/FontAwesome6Free-Solid-900.otf" \
		data/Amp/FontAwesome6Free-Solid-900.otf
	install_file 755 "$SRC/resources/amp-post-install.sh" boot/post-install/amp.sh
	deskbar_link apps/Amp Amp
	retarget_package_info "$SRC/resources/Amp.PackageInfo" "$STAGE/.PackageInfo"
	add_attributes "$BUILDDIR/Amp.rsrc" apps/Amp
	finish_package amp apps/Amp
}

# --- Turbo Chook ----------------------------------------------------------------
build_turbochook() {
	note TurboChook
	snapshot turbochook turbochook
	make -C "$SRC" -s -j"$JOBS" BUILD=build-arm64 CXX="$CXX_ARM64" CC="$CC_ARM64" all
	stage_begin
	install_binary "$BUILDDIR/TurboChook" "$BUILDDIR/TurboChook.rsrc" apps/TurboChook
	docs turbochook README.md LICENSE docs vendor/nlohmann/LICENSE.MIT vendor/nlohmann/UPSTREAM.md
	install_file 755 "$SRC/resources/turbochook-post-install.sh" boot/post-install/turbochook.sh
	deskbar_link apps/TurboChook TurboChook
	retarget_package_info "$SRC/resources/TurboChook.PackageInfo" "$STAGE/.PackageInfo"
	add_attributes "$BUILDDIR/TurboChook.rsrc" apps/TurboChook
	finish_package turbochook apps/TurboChook
}

# --- Natter ---------------------------------------------------------------------
engine_ready() {
	[[ -f $SUMMIT_ENGINE_LOG ]] && [[ $(tail -n 1 "$SUMMIT_ENGINE_LOG") == "ninja exit 0" ]] \
		&& [[ -e $SUMMIT_ENGINE/lib/libWebKit.so ]]
}

build_natter() {
	note Natter
	local web=0
	case $NATTER_WEB_SIGNIN in
		1) engine_ready || die "Summit's arm64 engine is not built ($SUMMIT_ENGINE_LOG)"; web=1 ;;
		auto) if engine_ready; then web=1; else
			echo "warning: Summit's arm64 engine is not finished; Natter is built without web sign-in" >&2
		fi ;;
		0) ;;
		*) die "NATTER_WEB_SIGNIN must be auto, 1 or 0" ;;
	esac
	snapshot natter natter
	local work=$APPBUILD/natter engine=$APPBUILD/natter/engine
	cat > "$work/toolchain.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Haiku)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_SYSROOT $SYSROOT)
set(CMAKE_C_COMPILER ${CROSS}gcc)
set(CMAKE_CXX_COMPILER ${CROSS}g++)
set(CMAKE_EXE_LINKER_FLAGS_INIT -specs=$UNWIND_SPECS)
set(CMAKE_SHARED_LINKER_FLAGS_INIT -specs=$UNWIND_SPECS)
set(CMAKE_FIND_ROOT_PATH $SYSROOT/boot/system $SYSROOT/boot/system/develop)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOF
	local rpath_link="-Wl,-rpath-link,$TLS_DEPS/lib -Wl,-rpath-link,$SYSROOT/boot/system/lib"
	local args=(-DNATTER_WEBKIT=)
	# The package runs the build-tree binary, so set its RPATH by hand: CMake
	# would otherwise add the host directories of the libraries it linked.
	local rpath=""
	if [[ $web == 1 ]]; then
		# The engine's public headers, laid out as an installed engine has them
		# (the same set Summit's own arm64 build-app.sh uses).
		rm -rf "$engine"
		mkdir -p "$engine/include/WebKit"
		local header
		for header in UIProcess/API/haiku/WebKitView.h UIProcess/API/haiku/WebKitContext.h \
				UIProcess/API/haiku/WebKitEmbedding.h UIProcess/API/haiku/WebKitExtensionPermission.h \
				UIProcess/API/haiku/WebKitInfo.h Shared/API/c/WKBase.h \
				Shared/API/c/WKDeclarationSpecifiers.h Shared/API/c/haiku/WKBaseHaiku.h; do
			cp "$SUMMIT_WEBKIT_SRC/$header" "$engine/include/WebKit/"
		done
		ln -s "$SUMMIT_ENGINE/lib" "$engine/lib"
		args=(-DNATTER_WEBKIT="$engine")
		rpath_link+=" -Wl,-rpath-link,$SUMMIT_ENGINE/lib"
		# lib/ is the summit_webkit package layout; the flat directory is a fallback.
		rpath="-Wl,-rpath,$ENGINE_DIR/lib:$ENGINE_DIR:/boot/home/config/non-packaged/lib/summit-webkit/lib"
	fi
	# Configure afresh: CMake keeps cached values from the other variant.
	rm -rf "$BUILDDIR"
	mkdir -p "$BUILDDIR"
	# No host curl or OpenSSL: no package configs, no host pkg-config files.
	mkdir -p "$work/no-pkgconfig"
	PKG_CONFIG_LIBDIR=$work/no-pkgconfig PKG_CONFIG_PATH= \
	cmake -S "$SRC" -B "$BUILDDIR" -DCMAKE_TOOLCHAIN_FILE="$work/toolchain.cmake" \
		-DCURL_NO_CURL_CMAKE=ON \
		-DCMAKE_BUILD_TYPE=Release -DNATTER_BUILD_TESTS=OFF -DNATTER_BUILD_CLI=OFF \
		-DCMAKE_SKIP_BUILD_RPATH=ON -DCMAKE_EXE_LINKER_FLAGS="$rpath_link $rpath" \
		-DCURL_INCLUDE_DIR="$FARM/include" -DCURL_LIBRARY="$FARM/lib/libcurl.so" \
		-DOPENSSL_INCLUDE_DIR="$FARM/include" -DOPENSSL_SSL_LIBRARY="$FARM/lib/libssl.so" \
		-DOPENSSL_CRYPTO_LIBRARY="$FARM/lib/libcrypto.so" "${args[@]}" >/dev/null
	cmake --build "$BUILDDIR" --target Natter -j"$JOBS" >/dev/null

	stage_begin
	install_binary "$BUILDDIR/Natter" "$BUILDDIR/Natter.rsrc" apps/Natter/Natter
	deskbar_link apps/Natter/Natter Natter
	docs natter README.md LICENSE
	# The package info is the heredoc of the app's own tools/package-haiku.sh.
	local version webkit_require=""
	version=$(sed -n 's/^project(natter VERSION \([0-9.]*\).*/\1/p' "$SRC/CMakeLists.txt")
	[[ $web == 1 ]] && webkit_require="	summit_webkit >= 1.10.0"
	sed -n '/^cat > "\$STAGE\/.PackageInfo" <<INFO$/,/^INFO$/p' "$SRC/tools/package-haiku.sh" \
		| sed '1d;$d' > "$work/PackageInfo.template"
	[[ -s $work/PackageInfo.template ]] || die "no package info in natter's tools/package-haiku.sh"
	VERSION=$version ARCH=x86_64 WEBKIT=$webkit_require \
		bash -c 'eval "cat <<INFO
$(cat "$1")
INFO"' _ "$work/PackageInfo.template" > "$work/Natter.PackageInfo"
	retarget_package_info "$work/Natter.PackageInfo" "$STAGE/.PackageInfo" '^summit_webkit'
	add_attributes "$BUILDDIR/Natter.rsrc" apps/Natter/Natter
	echo "web sign-in: $([[ $web == 1 ]] && echo "yes (engine from $ENGINE_DIR/lib)" || echo no)"
	echo "RPATH: $(readelf -d "$STAGE/apps/Natter/Natter" | sed -n 's/.*(R\(UN\)\{0,1\}PATH).*\[\(.*\)\]/\2/p')"
	finish_package natter apps/Natter/Natter
}


# --- Summit and its engine ----------------------------------------------------
# Summit is built from SUMMIT_SRC, the Summit checkout whose engine patch the
# arm64 engine in SUMMIT_ENGINE was built with (build/summit-arm64/summit), so
# that the browser and the engine always match. The engine goes into its own
# package, summit_webkit, installed in /boot/system/lib/summit-webkit like the
# x86_64 one: WebProcess and NetworkProcess there, the libraries in lib/.
SUMMIT_SRC=${SUMMIT_SRC:-$WORK/build/summit-arm64/summit}
SUMMIT_WEBKIT_VERSION=${SUMMIT_WEBKIT_VERSION:-1.10.0-1}
# Raise to replace an installed package built from the same Summit commit.
SUMMIT_REVISION=${SUMMIT_REVISION:-1}

# set_rpath <ELF file> <new rpath>: in place. CMake's file(RPATH_CHANGE)
# keeps appended resources but cannot make the RPATH longer or add one;
# patchelf (PATCHELF) does both. Haiku's runtime loader resolves a library's
# dependencies with that library's own RPATH only, so every library of the
# engine needs one.
PATCHELF=${PATCHELF:-$WORK/toolchains/patchelf/bin/patchelf}
set_rpath() {
	local old
	old=$(readelf -d "$1" | sed -n 's/.*(R\(UN\)\{0,1\}PATH).*\[\(.*\)\]/\2/p')
	[[ $old == "$2" ]] && return 0
	if [[ -n $old ]]; then
		local script=$APPBUILD/.relocate.cmake status=0
		printf 'file(RPATH_CHANGE FILE [==[%s]==] OLD_RPATH [==[%s]==] NEW_RPATH [==[%s]==])\n' \
			"$1" "$old" "$2" > "$script"
		cmake -P "$script" >/dev/null 2>&1 || status=$?
		rm -f "$script"
		[[ $status == 0 ]] && return 0
	fi
	[[ -x $PATCHELF ]] || die "cannot set the RPATH of $1 without patchelf ($PATCHELF)"
	"$PATCHELF" --force-rpath --set-rpath "$2" "$1"
}

# The engine's public API: every header of its Haiku API and the C base
# headers they include.
engine_api_headers() {
	ls "$SUMMIT_WEBKIT_SRC"/UIProcess/API/haiku/*.h
	echo "$SUMMIT_WEBKIT_SRC/Shared/API/c/WKBase.h" \
		"$SUMMIT_WEBKIT_SRC/Shared/API/c/WKDeclarationSpecifiers.h" \
		"$SUMMIT_WEBKIT_SRC/Shared/API/c/haiku/WKBaseHaiku.h"
}

build_summit_webkit() {
	note "summit_webkit (Summit's engine)"
	engine_ready || die "Summit's arm64 engine is not built ($SUMMIT_ENGINE_LOG)"
	local work=$APPBUILD/summit_webkit
	mkdir -p "$work"
	STAGE=$work/stage
	stage_begin
	local engine=lib/summit-webkit
	mkdir -p "$STAGE/$engine/lib"
	local name
	for name in WebProcess NetworkProcess; do
		cp "$SUMMIT_ENGINE/bin/$name" "$STAGE/$engine/$name"
		chmod 755 "$STAGE/$engine/$name"
		"$STRIP" --strip-debug "$STAGE/$engine/$name"
		set_rpath "$STAGE/$engine/$name" '$ORIGIN/lib'
	done
	"$TOOLS/xres" -o "$STAGE/$engine/WebProcess" "$SUMMIT_ENGINE/bin/WebProcess.rsrc"
	# Every library the processes need that Haiku does not have, transitively,
	# under its soname: libWebKit, JavaScriptCore, ICU 78 and the third-party
	# libraries of the engine (curl with OpenSSL, libxml2, image codecs...).
	local pending=("$engine/WebProcess" "$engine/NetworkProcess") item needed source
	local -A seen=()
	while [[ ${#pending[@]} -gt 0 ]]; do
		item=${pending[-1]}
		unset 'pending[-1]'
		for needed in $(readelf -d "$STAGE/$item" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
			[[ -n ${seen[$needed]:-} ]] && continue
			seen[$needed]=1
			[[ -e $SYSROOT/boot/system/lib/$needed || -e $SYSROOT/boot/system/develop/lib/$needed ]] \
				&& continue
			source=""
			[[ -e $SUMMIT_ENGINE/lib/$needed ]] && source=$SUMMIT_ENGINE/lib/$needed
			[[ -z $source && -e $TLS_DEPS/lib/$needed ]] && source=$TLS_DEPS/lib/$needed
			# what a GL engine build needs on top (FreeType, Fontconfig,
			# HarfBuzz, libepoxy: tools/rpi4/summit/build-gl-deps.sh)
			[[ -z $source && -n ${SUMMIT_ENGINE_EXTRA_DEPS:-} \
				&& -e $SUMMIT_ENGINE_EXTRA_DEPS/lib/$needed ]] \
				&& source=$SUMMIT_ENGINE_EXTRA_DEPS/lib/$needed
			if [[ -z $source ]]; then
				case $needed in
					libtracker.so|libtranslation.so|libmedia.so|libtextencoding.so) continue ;;
				esac
				die "summit_webkit: no $needed"
			fi
			cp "$(readlink -f "$source")" "$STAGE/$engine/lib/$needed"
			chmod 644 "$STAGE/$engine/lib/$needed"
			"$STRIP" --strip-unneeded "$STAGE/$engine/lib/$needed"
			set_rpath "$STAGE/$engine/lib/$needed" '$ORIGIN'
			pending+=("$engine/lib/$needed")
		done
	done
	# Fontconfig's configuration, where that build of it looks for it
	if [[ -n ${SUMMIT_ENGINE_EXTRA_DEPS:-} && -d $SUMMIT_ENGINE_EXTRA_DEPS/etc/fonts ]]; then
		mkdir -p "$STAGE/$engine/etc"
		cp -RL "$SUMMIT_ENGINE_EXTRA_DEPS/etc/fonts" "$STAGE/$engine/etc/"
	fi
	# libWebKit.so for programs that link the engine (Natter, Summit)
	ln -s libWebKit.so.1 "$STAGE/$engine/lib/libWebKit.so"
	# the public headers, as the x86_64 package has them
	local header
	mkdir -p "$STAGE/develop/headers/summit-webkit/WebKit"
	for header in $(engine_api_headers); do
		cp "$header" "$STAGE/develop/headers/summit-webkit/WebKit/"
	done
	mkdir -p "$STAGE/documentation/packages/summit_webkit"
	local engine_source=${SUMMIT_WEBKIT_SRC%/Source/WebKit}
	git -C "$engine_source" diff > "$STAGE/documentation/packages/summit_webkit/arm64-changes.diff"
	git -C "$engine_source" rev-parse HEAD > "$STAGE/documentation/packages/summit_webkit/engine-revision.txt"
	cat > "$STAGE/.PackageInfo" <<INFO
name			summit_webkit
version			$SUMMIT_WEBKIT_VERSION
architecture	arm64
summary			"The WebKit engine of the Summit browser"
description		"The Summit browser's WebKit build for arm64 (libWebKit with its web and network processes, JavaScriptCore, ICU 78 and the libraries they need), installed in /boot/system/lib/summit-webkit. Programs link lib/libWebKit.so.1 from there and include develop/headers/summit-webkit. Built from upstream WebKit 00991b6c with Summit's Haiku port patch (Summit $(git -C "$SUMMIT_SRC" rev-parse --short HEAD)), drawing through app_server."
packager		"air/OS"
vendor			"Summit project"
copyrights {
	"Apple Inc. and the WebKit contributors"
	"Summit contributors"
}
licenses {
	"GNU LGPL v2"
	"GNU LGPL v2.1"
	"BSD (2-clause)"
	"BSD (3-clause)"
	"MIT"
}
provides {
	summit_webkit = ${SUMMIT_WEBKIT_VERSION%-*}
}
requires {
	haiku >= r1~beta6
}
INFO
	DROPPED="(none)"
	finish_package summit_webkit "$engine/WebProcess" "$engine/NetworkProcess"
}

build_summit() {
	note Summit
	engine_ready || die "Summit's arm64 engine is not built ($SUMMIT_ENGINE_LOG)"
	local saved_apps=$APPS
	APPS=$(dirname "$SUMMIT_SRC")
	snapshot "$(basename "$SUMMIT_SRC")" summit
	APPS=$saved_apps
	local work=$APPBUILD/summit
	# The engine's public headers, as an installed engine has them.
	rm -rf "$work/engine"
	mkdir -p "$work/engine/include/WebKit" "$BUILDDIR/obj"
	local header
	for header in $(engine_api_headers); do
		cp "$header" "$work/engine/include/WebKit/"
	done
	# The browser's sources: the list its own native build uses.
	local sources
	sources=$(python3 - "$SRC/tools/build-modern-browser.py" <<'PY'
import ast, re, sys
text = open(sys.argv[1]).read()
match = re.search(r"\n(\s+)sources = (\['src/main\.cpp'.*?\])\n", text, re.S)
print(' '.join(ast.literal_eval(match.group(2))))
PY
)
	[[ -n $sources ]] || die "no source list in Summit's tools/build-modern-browser.py"
	local private=$SYSROOT/boot/system/develop/headers/private
	local flags=(-std=c++23 -O2 -Wall -Wextra -Wno-multichar -DBUILDING_HAIKU__=1
		-I"$work/engine/include" -DSUMMIT_MODERN_WEBKIT=1 -I"$SRC/src" -I"$SRC/vendor"
		-I"$private/netservices" -I"$private" -I"$FARM/include"
		-I"$FARM/include/scintilla" -I"$FARM/include/lexilla")
	local source objects=() object
	for source in $sources; do
		object=$BUILDDIR/obj/$(echo "${source%.cpp}" | tr / _).o
		objects+=("$object")
		[[ -e $object && ! $SRC/$source -nt $object ]] && continue
		${CROSS}g++ --sysroot="$SYSROOT" "${flags[@]}" -c "$SRC/$source" -o "$object" &
		while [[ $(jobs -rp | wc -l) -ge $JOBS ]]; do wait -n; done
	done
	wait
	for object in "${objects[@]}"; do [[ -e $object ]] || die "Summit: $object did not compile"; done
	${CROSS}g++ --sysroot="$SYSROOT" -specs="$UNWIND_SPECS" "${objects[@]}" -L"$SUMMIT_ENGINE/lib" -L"$FARM/lib" \
		-Wl,-rpath-link,"$SUMMIT_ENGINE/lib" -Wl,-rpath-link,"$TLS_DEPS/lib" \
		-Wl,-rpath-link,"$SYSROOT/boot/system/lib" -Wl,-rpath,"$ENGINE_DIR/lib" \
		-lWebKit -lbe -lnetwork -lcrypto -lbnetapi -ltranslation -ltracker -lgame \
		-lscintilla -llexilla -lcurl -lcolumnlistview -o "$BUILDDIR/Summit"
	"$TOOLS/rc/rc" -I "$SRC/resources" -o "$BUILDDIR/Summit.rsrc" "$SRC/resources/Summit.rdef"

	stage_begin
	install_binary "$BUILDDIR/Summit" "$BUILDDIR/Summit.rsrc" apps/Summit
	install_file 644 "$SRC/resources/start.html" data/Summit/start.html
	deskbar_link apps/Summit Summit
	docs summit README.md LICENSE
	local version
	version="0.1.0~git$(git -C "$SUMMIT_SRC" log -1 --format=%cd --date=format:%Y%m%d)-$SUMMIT_REVISION"
	cat > "$STAGE/.PackageInfo" <<INFO
name			summit
version			$version
architecture	arm64
summary			"A native WebKit browser"
description		"Summit is a WebKit browser for Haiku and air/OS, with tabs, extensions, developer tools and site permissions. Built from Summit $(git -C "$SUMMIT_SRC" rev-parse --short HEAD) against the summit_webkit engine."
packager		"air/OS"
vendor			"Summit project"
copyrights {
	"2026 KunanyiOS contributors"
	"2013-2025 Niels Lohmann"
}
licenses {
	"MIT"
}
provides {
	summit = ${version%-*}
	app:Summit = ${version%-*}
}
requires {
	haiku >= r1~beta6
	summit_webkit >= ${SUMMIT_WEBKIT_VERSION%-*}
}
INFO
	DROPPED="(none)"
	add_attributes "$BUILDDIR/Summit.rsrc" apps/Summit
	finish_package summit apps/Summit
}

main() {
	check_prerequisites
	mkdir -p "$APPBUILD" "$OUT" "$TMPDIR"
	: > "$APPBUILD/.summary"
	setup_host_tools
	setup_dependency_farm
	local app
	for app in airshot clipper burrow kiri lcdmonitor amp turbochook natter airtop summit_webkit summit \
			airpins rpi_installer; do
		[[ ( $app == airpins || $app == rpi_installer ) && ${#SELECTED[@]} -eq 0 ]] && continue
		wanted "$app" && "build_$app"
	done
	note summary
	column -t -s $'\t' "$APPBUILD/.summary"
}

main
