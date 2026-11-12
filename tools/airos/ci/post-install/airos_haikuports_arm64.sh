#!/bin/sh
# air/OS, arm64 (airos-works/haiku#118): HaikuPorts has no arm64 repository,
# so air/OS serves HaikuPorts' arm64 build packages (gcc, binutils, make, ...)
# at https://airos.works/repo/arm64, which images since November 2026 name.
# An older install's HaikuPorts entry still names HaikuPorts' server, where
# every download fails ("Resource not found"): point it at air/OS's, and drop
# the cached package list, which names packages that are no longer anywhere,
# so that HaikuDepot and pkgman fetch the new one.
#
# The airos_release package runs this after each update System Updater
# installs; once the entry is changed it does nothing.

config=/boot/system/settings/package-repositories/HaikuPorts
cache=/boot/system/cache/package-repositories/HaikuPorts
url=https://airos.works/repo/arm64

[ -f "$config" ] || exit 0
grep -q '^baseurl=https://eu.hpkg.haiku-os.org/haikuports/master/arm64/' \
	"$config" || exit 0

sed "s|^baseurl=.*|baseurl=$url|" "$config" > "$config.new" \
	&& mv "$config.new" "$config" || exit 1
rm -f "$cache"
echo "airos_haikuports_arm64: HaikuPorts now from $url" > /dev/dprintf
