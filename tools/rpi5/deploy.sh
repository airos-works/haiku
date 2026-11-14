#!/bin/bash
# Install test builds of system add-ons on the Pi 5 running air/OS, in
# place of the haiku package's: copies them to /boot/system/non-packaged and
# blocks the packaged entries (a packaged add-on otherwise wins), which
# takes a restart.
#
#   deploy.sh <local file>=<path below add-ons> ... [--restart]
#
# e.g. deploy.sh build/.../broadcomfmac=kernel/drivers/bin/broadcomfmac
# A driver in kernel/drivers/bin also needs its dev/ link: give it as
#   =kernel/drivers/bin/<name>:dev/net
# The block list goes into /boot/system/settings/packages, appended once
# per entry: the image ships that file, so it is never rewritten here.
set -euo pipefail
STATE=${RPI5_STATE:-/mnt/HaikuWork/rpi5/state}
pi() { ssh -F "$STATE/ssh_config" rpi5 "$@"; }
restart=false
blocked=()
for argument in "$@"; do
	if [ "$argument" = --restart ]; then restart=true; continue; fi
	file=${argument%%=*}
	target=${argument#*=}
	devdir=
	case $target in *:*) devdir=${target#*:}; target=${target%%:*};; esac
	name=$(basename "$target")
	dest=/boot/system/non-packaged/add-ons/$target
	pi "mkdir -p '$(dirname "$dest")'"
	# staged outside the add-ons tree: devfs loads any file that appears in
	# a drivers directory at once, a half-written one included
	cat "$file" | pi "cat > /tmp/deploy.$name && chmod 755 /tmp/deploy.$name && mv /tmp/deploy.$name '$dest'"
	echo "$name -> $dest ($(sha256sum "$file" | cut -c1-12))"
	blocked+=("add-ons/$target")
	if [ -n "$devdir" ]; then
		depth=$(echo "kernel/drivers/$devdir" | tr '/' '\n' | wc -l)
		up=$(printf '../%.0s' $(seq 1 $((depth - 2))))
		link=/boot/system/non-packaged/add-ons/kernel/drivers/$devdir/$name
		pi "mkdir -p '$(dirname "$link")' && ln -sf '${up}bin/$name' '$link'"
		blocked+=("add-ons/kernel/drivers/$devdir/$name")
	fi
done
for entry in "${blocked[@]}"; do
	pi "grep -q '^# deploy.sh $entry\$' /boot/system/settings/packages 2>/dev/null || printf '\n# deploy.sh %s\nPackage haiku {\n\tBlockedEntries {\n\t\t%s\n\t}\n}\n' '$entry' '$entry' >> /boot/system/settings/packages"
done
# the lab cuts the power to recover from hangs: have it all on the disk
pi sync
if $restart; then
	pi 'shutdown -r' >/dev/null 2>&1 &
	sleep 3
	echo "restarting"
fi
