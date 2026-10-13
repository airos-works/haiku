#!/bin/sh
# X399 lab: drop experimental driver links at boot unless explicitly kept.
# x399-keep-drivers keeps them permanently, x399-keep-drivers.once for one boot.
# A staged nvidia_rm.new replaces the driver before it is first loaded.
keep=/boot/system/settings/x399-keep-drivers
np=/boot/system/non-packaged/add-ons
base=$np/kernel/drivers/dev
# Also the user's non-packaged drivers (mt7922wifi): a driver that is loaded
# must never have its binary replaced - devfs reloads it underneath itself,
# which has stopped the machine - so new builds are staged as .new and swap
# in here, before anything opens them.
unp=/boot/home/config/non-packaged/add-ons
for f in $np/kernel/drivers/bin/*.new $np/accelerants/*.new \
		$unp/kernel/drivers/bin/*.new; do
	[ -e "$f" ] && mv -f "$f" "${f%.new}"
done
sync
if [ ! -e "$keep" ] && [ ! -e "$keep.once" ]; then
	rm -f $base/graphics/nvidia_rm $base/nvidia_rm_modeset
	rm -f $np/accelerants/nvidia_rm.accelerant
fi
rm -f "$keep.once"

# Experimental kernel driver settings apply to one boot only. The boot loader
# has already handed this boot its copy; moving the file now keeps it out of
# the next one, so a boot that the setting brings down is followed by one
# without it. A file named <name>.keep beside it keeps it.
for name in xhci; do
	x=/boot/home/config/settings/kernel/drivers/$name
	if [ -e $x ] && [ ! -e $x.keep ]; then
		mv -f $x $x.used-once
		sync
	fi
done

# amdgpu_drm: a new build waits outside the drivers' directories (devfs would
# start a new file in them as a second driver for the GPU) until it can be
# swapped in here, before app_server opens the display
# (src/forks/linux-drm haiku/tools/x399-install.sh).
s=/boot/home/amdgpu-drm/amdgpu_drm.new
if [ -e $s ]; then
	mv -f $s $unp/kernel/drivers/bin/amdgpu_drm
	sync
fi
# amdgpu_drm bring-up: the lab build is never loaded at boot
rm -f $unp/kernel/drivers/dev/amdgpu_test/amdgpu_drm
sync
