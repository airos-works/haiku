#!/usr/bin/env bash
# Supply the arm64 bootstrap repository's missing image libraries:
# libjpeg-turbo, libpng and libwebp. Without them the arm64 build leaves out
# the JPEG, PNG and WebP translators, so nothing that reads pictures through
# the Translation Kit (ShowImage, Tracker, Amp's covers, ...) opens a JPEG or
# PNG, and the USB webcam add-on has no MJPEG.
#
# The packages carry HaikuPorts' names and provides (lib:libjpeg,
# lib:libpng16, lib:libwebp, ...), so haiku_datatranslators' requirements
# resolve against them. libpng comes from the air/OS fork
# (github.com/jmgasper/libpng, branch airos-v1.6.44), the others from their
# pinned release archives. Packages stay zlib-compressed like zstd's.
set -euo pipefail
umask 022
TOOLS_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$TOOLS_DIR/../rock5-itx/env.sh"
ROOT=${RPI4_IMAGE_LIBS_ROOT:-/mnt/HaikuWork/rpi4/image-libs}
BUILD=${RPI4_BUILD:-/mnt/HaikuWork/rpi4/build}
CROSS=${RPI4_CROSS:-/mnt/HaikuWork/build/arm64/cross-tools-arm64/bin/aarch64-unknown-haiku-}
SYSROOT=${RPI4_SYSROOT:-/mnt/HaikuWork/build/summit-arm64/sysroot}
LIBPNG_FORK=${RPI4_LIBPNG_FORK:-/mnt/HaikuWork/src/forks/libpng}
PACKAGE=$BUILD/objects/linux/x86_64/release/tools/package/package
OUT=$ROOT/packages
REVISION=1

JPEG_VERSION=3.0.4
JPEG_SHA256=99130559e7d62e8d695f2c0eaeef912c5828d5b84a0537dcb24c9678c9d5b76b
JPEG_URL=https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/$JPEG_VERSION/libjpeg-turbo-$JPEG_VERSION.tar.gz
PNG_VERSION=1.6.44
PNG_BRANCH=airos-v$PNG_VERSION
PNG_COMMIT=f5e92d76973a7a53f517579bc95d61483bf108c0
PNG_REPOSITORY=https://github.com/jmgasper/libpng.git
WEBP_VERSION=1.4.0
WEBP_SHA256=61f873ec69e3be1b99535634340d5bde750b2e4447caa1db9f61be3fd49ab1e5
WEBP_URL=https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-$WEBP_VERSION.tar.gz

mkdir -p "$ROOT" "$OUT"
[[ -x $PACKAGE ]] || { echo "No host package tool at $PACKAGE" >&2; exit 1; }

fetch() { # <url> <sha256> -> archive path
    local archive=$ROOT/${1##*/}
    if [[ ! -f $archive ]]; then
        curl -fL --retry 3 -o "$archive.part" "$1" >&2
        printf '%s  %s\n' "$2" "$archive.part" | sha256sum -c - >&2
        mv "$archive.part" "$archive"
    fi
    printf '%s  %s\n' "$2" "$archive" | sha256sum -c - >&2
    printf '%s\n' "$archive"
}
JPEG_ARCHIVE=$(fetch "$JPEG_URL" "$JPEG_SHA256")
WEBP_ARCHIVE=$(fetch "$WEBP_URL" "$WEBP_SHA256")
# libpng: the fork's pinned commit, exported without the checkout's state.
if [[ ! -d $LIBPNG_FORK/.git ]]; then
    LIBPNG_FORK=$ROOT/libpng.git
    [[ -d $LIBPNG_FORK ]] || git clone --bare --filter=blob:none \
        --branch "$PNG_BRANCH" "$PNG_REPOSITORY" "$LIBPNG_FORK"
fi
git -C "$LIBPNG_FORK" cat-file -e "$PNG_COMMIT^{commit}" 2>/dev/null \
    || git -C "$LIBPNG_FORK" fetch "$PNG_REPOSITORY" "$PNG_BRANCH"
git -C "$LIBPNG_FORK" cat-file -e "$PNG_COMMIT^{commit}"

WORK=$(mktemp -d "$ROOT/work.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
tar -xzf "$JPEG_ARCHIVE" -C "$WORK"
tar -xzf "$WEBP_ARCHIVE" -C "$WORK"
git -C "$LIBPNG_FORK" archive --prefix="libpng-$PNG_VERSION/" "$PNG_COMMIT" \
    | tar -x -C "$WORK"

cat > "$WORK/haiku-arm64.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Haiku)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_SYSROOT $SYSROOT)
set(CMAKE_C_COMPILER ${CROSS}gcc)
set(CMAKE_AR ${CROSS}ar CACHE FILEPATH "")
set(CMAKE_RANLIB ${CROSS}ranlib CACHE FILEPATH "")
set(CMAKE_STRIP ${CROSS}strip CACHE FILEPATH "")
set(CMAKE_FIND_ROOT_PATH $SYSROOT/boot/system/develop $SYSROOT/boot/system)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOF

# Not CMAKE_POSITION_INDEPENDENT_CODE: it compiles executables with -fPIE,
# which Haiku links as shared objects, and CMake's own compiler checks fail.
# Shared libraries are -fPIC regardless.
build() { # <name> <source dir> <cmake args>...
    local name=$1 source=$2
    shift 2
    cmake -S "$source" -B "$WORK/$name-build" \
            -DCMAKE_TOOLCHAIN_FILE="$WORK/haiku-arm64.cmake" \
            -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$WORK/$name-install" \
            -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_INSTALL_INCLUDEDIR=include \
            -DBUILD_SHARED_LIBS=ON -DCMAKE_SKIP_RPATH=ON \
            -DCMAKE_SHARED_LINKER_FLAGS=-Wl,--hash-style=both "$@" > "$ROOT/$name-build.log" 2>&1 \
        && cmake --build "$WORK/$name-build" -j"$HAIKU_JOBS" >> "$ROOT/$name-build.log" 2>&1 \
        && cmake --install "$WORK/$name-build" >> "$ROOT/$name-build.log" 2>&1 \
        || { tail -30 "$ROOT/$name-build.log" >&2; exit 1; }
    grep -q -- '-- Detecting C compiler ABI info - done' "$ROOT/$name-build.log" \
        || { echo "$name: CMake could not identify the compiler" >&2; exit 1; }
}
# The libjpeg 6.2 ABI (libjpeg.so.62), as HaikuPorts ships it.
build jpeg "$WORK/libjpeg-turbo-$JPEG_VERSION" -DENABLE_STATIC=OFF \
    -DWITH_TURBOJPEG=OFF -DWITH_JPEG8=OFF
build png "$WORK/libpng-$PNG_VERSION" -DPNG_SHARED=ON -DPNG_STATIC=OFF \
    -DPNG_TESTS=OFF -DPNG_TOOLS=OFF -DPNG_ARM_NEON=on \
    -DZLIB_INCLUDE_DIR="$SYSROOT/boot/system/develop/headers" \
    -DZLIB_LIBRARY="$SYSROOT/boot/system/develop/lib/libz.so"
build webp "$WORK/libwebp-$WEBP_VERSION" -DWEBP_BUILD_ANIM_UTILS=OFF \
    -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF -DWEBP_BUILD_GIF2WEBP=OFF \
    -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF -DWEBP_BUILD_WEBPINFO=OFF \
    -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF

STAGE=$WORK/stage
# stage_libraries <name> <libraries>...: the shared libraries (real file and
# soname link) into the base package, the development link into devel; prints
# the "lib:" provides.
stage_libraries() {
    local name=$1 library real soname version
    shift
    mkdir -p "$STAGE/$name/lib" "$STAGE/${name}_devel/develop/lib"
    for library in "$@"; do
        real=$(cd "$WORK" && ls *-install/lib/"$library".so.*.*.*)
        real=${real##*/}
        version=${real#"$library".so.}
        soname=$library.so.${version%%.*}
        cp "$WORK"/*-install/lib/"$real" "$STAGE/$name/lib/"
        "${CROSS}strip" --strip-unneeded "$STAGE/$name/lib/$real"
        ln -s "$real" "$STAGE/$name/lib/$soname"
        ln -s "../../lib/$real" "$STAGE/${name}_devel/develop/lib/$library.so"
        printf 'lib:%s = %s compat >= %s\n' "$library" "$version" "${version%%.*}"
    done
}
headers() { # <package> <source dir> [<subdirectory>]
    mkdir -p "$STAGE/$1_devel/develop/headers${3:+/$3}"
    cp -R "$2"/. "$STAGE/$1_devel/develop/headers${3:+/$3}/"
}
# Into the base and the devel package: the package tool insists that every
# licence a package names, and Haiku does not ship, is inside it.
licenses() { # <package> <name>=<file>...
    local package=$1 entry stage
    shift
    for stage in "$STAGE/$package" "$STAGE/${package}_devel"; do
        mkdir -p "$stage/data/licenses"
        for entry in "$@"; do
            cp "${entry#*=}" "$stage/data/licenses/${entry%%=*}"
        done
    done
}

jpegLibs=$(stage_libraries libjpeg_turbo libjpeg)
headers libjpeg_turbo "$WORK/jpeg-install/include"
licenses libjpeg_turbo \
    "libjpeg-turbo=$WORK/libjpeg-turbo-$JPEG_VERSION/LICENSE.md" \
    "IJG=$WORK/libjpeg-turbo-$JPEG_VERSION/README.ijg"
pngLibs=$(stage_libraries libpng16 libpng16)
# png.h, pngconf.h and pnglibconf.h, also under libpng16/ as HaikuPorts has them
headers libpng16 "$WORK/png-install/include/libpng16"
headers libpng16 "$WORK/png-install/include/libpng16" libpng16
licenses libpng16 "LibPNG=$WORK/libpng-$PNG_VERSION/LICENSE"
webpLibs=$(stage_libraries libwebp libwebp libwebpdemux libwebpmux libsharpyuv)
headers libwebp "$WORK/webp-install/include"
licenses libwebp "libwebp=$WORK/libwebp-$WEBP_VERSION/COPYING" \
    "libwebp patents=$WORK/libwebp-$WEBP_VERSION/PATENTS"

devel_provides() { sed -e 's/^lib:/devel:/' <<< "$1"; }
indent() { sed -e '1!s/^/    /' <<< "$1"; }
# metadata <package> <version> <summary> <copyrights> <licenses> <url>
#     <source url> <provides> <requires>
metadata() {
    cat > "$STAGE/$1/.PackageInfo" <<EOF
name $1
version $2-$REVISION
architecture arm64
summary "$3"
description "$3, built for air/OS arm64 by tools/rpi4/build-image-libs.sh."
packager "air/OS"
vendor "air/OS"
copyrights { $4 }
licenses { $5 }
urls "$6"
source-urls "$7"
provides {
    $(indent "$8")
}
requires {
    $(indent "$9")
}
EOF
}
for package in libjpeg_turbo libpng16 libwebp; do
    case $package in
        libjpeg_turbo)
            version=$JPEG_VERSION libs=$jpegLibs
            summary="libjpeg-turbo $JPEG_VERSION JPEG codec (libjpeg 6.2 API)"
            copyrights='"1991-2020 Thomas G. Lane, Guido Vollbeding" "2009-2024 D. R. Commander"'
            licenses='"BSD (3-clause)" "IJG" "Zlib"'
            url=https://libjpeg-turbo.org source=$JPEG_URL requires= ;;
        libpng16)
            version=$PNG_VERSION libs=$pngLibs
            summary="libpng $PNG_VERSION PNG reference library"
            copyrights='"1995-2024 The PNG Reference Library Authors"'
            licenses='"LibPNG"'
            url=http://www.libpng.org/pub/png/libpng.html
            source="$PNG_REPOSITORY#$PNG_COMMIT" requires='lib:libz >= 1' ;;
        libwebp)
            version=$WEBP_VERSION libs=$webpLibs
            summary="libwebp $WEBP_VERSION WebP codec"
            copyrights='"2010-2024 Google Inc."'
            licenses='"BSD (3-clause)"'
            url=https://developers.google.com/speed/webp source=$WEBP_URL requires= ;;
    esac
    metadata "$package" "$version" "$summary" "$copyrights" "$licenses" "$url" \
        "$source" "$package = $version compat >= ${version%.*}"$'\n'"$libs" \
        "haiku >= r1~beta6${requires:+$'\n'$requires}"
    metadata "${package}_devel" "$version" "$summary, development files" \
        "$copyrights" "$licenses" "$url" "$source" \
        "${package}_devel = $version compat >= ${version%.*}"$'\n'"$(devel_provides "$libs")" \
        "$package == $version"
done

# Host tools associate attributes with inode numbers; fresh staging inodes
# must not inherit attributes belonging to earlier, deleted build outputs.
python3 - "$STAGE" "$BUILD/attributes" <<'PY'
from pathlib import Path
import shutil
import sys
stage, attributes = map(Path, sys.argv[1:])
for path in [stage, *stage.rglob('*')]:
    directory = attributes / str(path.lstat().st_ino)
    if directory.is_dir():
        shutil.rmtree(directory)
PY
{
    echo "# tools/rpi4/build-image-libs.sh, $(date -u +%Y-%m-%dT%H:%MZ)"
    echo "compiler $("${CROSS}gcc" --version | head -1)"
    echo "sysroot $SYSROOT"
    echo "libjpeg-turbo $JPEG_URL sha256 $JPEG_SHA256"
    echo "libpng $PNG_REPOSITORY $PNG_BRANCH $PNG_COMMIT"
    echo "libwebp $WEBP_URL sha256 $WEBP_SHA256"
} > "$OUT/SOURCES"
for kind in libjpeg_turbo libpng16 libwebp; do
    case $kind in
        libjpeg_turbo) version=$JPEG_VERSION ;;
        libpng16) version=$PNG_VERSION ;;
        libwebp) version=$WEBP_VERSION ;;
    esac
    for name in "$kind" "${kind}_devel"; do
        file=$OUT/$name-$version-$REVISION-arm64.hpkg
        rm -f "$file"
        "$PACKAGE" create -q -z zlib -C "$STAGE/$name" "$file"
        "$PACKAGE" list -a "$file" > "$file.manifest"
        sha256sum "$file" | tee -a "$OUT/SOURCES"
    done
done
echo "Packages for HAIKU_ARM64_IMAGE_LIBS_PACKAGES_DIR: $OUT"
