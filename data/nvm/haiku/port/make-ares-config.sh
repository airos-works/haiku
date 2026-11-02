#!/bin/sh
# Haiku: make-ares-config.sh X.Y.Z [out-dir] - run c-ares' own cmake
# configure for release X.Y.Z on Haiku and keep the result as
# <out-dir>/ares_config-X.Y.Z.h (port-haiku.py installs it as
# deps/cares/config/haiku/ares_config.h of every Node release bundling it).
set -e
V=$1
OUT=$(cd "${2:-.}" && pwd)
WORK=${TMPDIR:-/tmp}/ares-config-$V
rm -rf "$WORK"
mkdir -p "$WORK"
cd "$WORK"
TAG=v$V
case "$V" in
	1.[12][0-9].*) TAG=cares-$(echo "$V" | tr . _) ;;  # v1.30.0 and later: vX.Y.Z
esac
curl -sfL -o c-ares.tar.gz "https://github.com/c-ares/c-ares/releases/download/$TAG/c-ares-$V.tar.gz"
tar xzf c-ares.tar.gz
cmake -S "c-ares-$V" -B build -DCARES_STATIC=ON -DCARES_SHARED=OFF -DCARES_BUILD_TOOLS=OFF \
	-DCMAKE_POLICY_VERSION_MINIMUM=3.5 > cmake.log 2>&1 || { tail -20 cmake.log; exit 1; }
CONFIG=$(find build -name ares_config.h | head -1)
[ -n "$CONFIG" ] || { echo "no ares_config.h from c-ares $V"; exit 1; }
# c-ares before 1.18 finds the argument types of recv, recvfrom and send on
# Haiku but leaves HAVE_RECV, HAVE_RECVFROM and HAVE_SEND undefined (its
# check does not link libnetwork); setup_once.h then stops the build.
for f in RECV RECVFROM SEND; do
	if grep -q "^#define ${f}_TYPE_ARG1 " "$CONFIG"; then
		sed -i "s|^/\* #undef HAVE_${f} \*/\$|#define HAVE_${f} 1|" "$CONFIG"
	fi
done
cp "$CONFIG" "$OUT/ares_config-$V.h"
cd /
rm -rf "$WORK"
echo "$OUT/ares_config-$V.h"
