#!/bin/sh
# Refresh the workstation's package repository cache by hand.
#
# Only for a system whose libbnetapi has no SSL (built before haiku master
# 8207ef3028 from a profile-less `jam haiku.hpkg`): there `pkgman refresh` and
# every pkgman download fail with "Operation not supported". (This used to be
# blamed on repo.checksum vs repo.sha256; that was wrong.) Check with
# `readelf -d /boot/system/lib/libbnetapi.so | grep libssl`. pkgman is happy
# with a cache file that was put there by other means, so fetch the index with
# curl and drop it in place.
#
# Run this on the workstation, not here.
set -e

CACHE=/system/cache/package-repositories
HAIKUPORTS=https://haikuports-repository.cdn.haiku-os.org/master/x86_64/current

curl -fsSL -o "$CACHE/HaikuPorts.new" "$HAIKUPORTS/repo"
mv -f "$CACHE/HaikuPorts.new" "$CACHE/HaikuPorts"
echo "HaikuPorts index refreshed: $(wc -c < "$CACHE/HaikuPorts") bytes"

# On such a system pkgman's own downloader hits the same wall, so installs need the .hpkg
# fetched first:  curl -fsSLO $HAIKUPORTS/packages/<name>-<version>-x86_64.hpkg
#                 pkgman install ./<name>-<version>-x86_64.hpkg
