#!/bin/sh
# Build a Node.js release tarball on Haiku (x86_64) from a patched source tree.
#
#   build-node.sh <source-dir> [out-dir]
#
# <source-dir> is an extracted node-vX.Y.Z tree already ported with
# data/nvm/haiku/port/port-haiku.py (Node.js 10 and 11 need python2.7 on
# PATH, 12 to 19 python3.10 or 3.11). Everything is
# bundled (OpenSSL, ICU, libuv, c-ares, nghttp2, brotli, zlib), so the result
# only needs libroot, libbsd, libnetwork and gcc_syslibs at run time.
# Produces <out-dir>/node-vX.Y.Z-haiku-x64.tar.{gz,xz} in the nodejs.org layout.
#
# Environment: JOBS (default: CPU count), CONFIG_FLAGS (extra ./configure flags).
set -e
SRC=$(cd "$1" && pwd)
OUT=${2:-$HOME/dist}
JOBS=${JOBS:-$(nproc)}
cd "$SRC"
VERSION=$(sed -n 's/^#define NODE_MAJOR_VERSION \([0-9]*\)/\1/p;s/^#define NODE_MINOR_VERSION \([0-9]*\)/\1/p;s/^#define NODE_PATCH_VERSION \([0-9]*\)/\1/p' src/node_version.h | paste -sd.)
NAME=node-v$VERSION-haiku-x64
echo "== $NAME: configure ($(date))"
./configure --prefix=/ --dest-os=haiku --dest-cpu=x64 $CONFIG_FLAGS
echo "== $NAME: make -j$JOBS ($(date))"
make -j"$JOBS"
echo "== $NAME: install ($(date))"
STAGE=$SRC/out/stage
rm -rf "$STAGE"
make -j"$JOBS" install DESTDIR="$STAGE/$NAME" PORTABLE=1
cp README.md LICENSE CHANGELOG.md "$STAGE/$NAME/"
mkdir -p "$OUT"
( cd "$STAGE" && tar -cf "$OUT/$NAME.tar" "$NAME" )
gzip -c -f -9 "$OUT/$NAME.tar" > "$OUT/$NAME.tar.gz"
xz -c -f -9e -T0 "$OUT/$NAME.tar" > "$OUT/$NAME.tar.xz"
rm -f "$OUT/$NAME.tar"
( cd "$OUT" && sha256sum "$NAME.tar.gz" "$NAME.tar.xz" )
echo "== $NAME: done ($(date))"
