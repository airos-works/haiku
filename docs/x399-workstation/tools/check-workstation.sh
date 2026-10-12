#!/bin/bash
# Check the workstation against what it is supposed to do, one line per thing.
#
# Every check here exists because something once looked fine and was not: a
# build that never installed, a driver the kernel never loaded, a screen saver
# that quietly turned the GPU path off, a Vulkan driver only visible to a shell
# with the right variables set. So nothing is run with anything set in its
# environment, and each check asks the machine rather than trusting that a
# thing was deployed.
#
# usage: check-workstation.sh
set -u
SSH="ssh -F /mnt/HaikuWork/x399/ssh/config -o ConnectTimeout=10 ws-haiku"

$SSH 'set -u
	pass=0; fail=0
	say() { # say <ok|no> <what> <detail>
		if [ "$1" = ok ]; then pass=$((pass+1)); printf "  yes  %-34s %s\n" "$2" "$3"
		else fail=$((fail+1)); printf "  NO   %-34s %s\n" "$2" "$3"; fi
	}

	echo "the machine"
	# sysinfo counts threads, and this part has sixteen cores with two each.
	threads=$(sysinfo | grep -c "^CPU #")
	[ "$threads" -ge 32 ] && say ok "sixteen cores" "$threads threads" \
		|| say no "sixteen cores" "$threads threads"

	# df prints a block of lines here rather than a table.
	boot=$(df /boot | grep "^ *Device:" | awk "{print \$2}")
	case "$boot" in
		*nvme*) say ok "booted from the NVMe drive" "$boot";;
		*) say no "booted from the NVMe drive" "${boot:-unknown}";;
	esac

	# There is a space after the colon, and the KVM presents a second
	# interface, so report every address that is not the loopback.
	ips=$(ifconfig | grep -o "inet addr: [0-9.]*" | awk "{print \$3}" \
		| grep -v "^127\." | tr "\n" " ")
	[ -n "$ips" ] && say ok "network" "$ips" || say no "network" "no address"

	echo "graphics"
	# Ask the device tree, not the syslog: the syslog is trimmed as it grows
	# and the driver announces itself only once, early.
	# The WX 5100 on amdgpu_drm (docs AMDGPU-DRM.md), or the GTX 1070 that
	# was in its place until 2026-10-10.
	if [ -e /dev/graphics/amdgpu_drm_0 ] && [ -e /dev/dri/renderD128 ]; then
		say ok "the GPU driver started" "$(ls /dev/graphics /dev/dri | grep -v ":$" | tr "\n" " ")"
	elif [ -e /dev/graphics/nvidia0 ]; then
		say ok "the GPU driver started" "$(ls /dev/graphics | tr "\n" " ")"
	else
		say no "the GPU driver started" "no amdgpu_drm (or nvidia) device"
	fi

	mode=$(screenmode -s 2>/dev/null)
	[ -n "$mode" ] && say ok "a display is being driven" "$mode" \
		|| say no "a display is being driven" "no mode"

	# Nothing set in the environment: this is how a program started from the
	# Deskbar sees the machine.
	vk=$(env -u VK_ICD_FILENAMES -u LD_LIBRARY_PATH /boot/home/tests/vkbench submit 2>&1 | grep -o "NVIDIA[^(]*\|AMD Radeon[^(]*([A-Z]* [A-Z0-9]*)" | head -1)
	[ -n "$vk" ] && say ok "Vulkan finds the GPU by itself" "$vk" \
		|| say no "Vulkan finds the GPU by itself" "falls back to software"

	blanker=$(ps | grep -c "[s]creen_blanker")
	[ "$blanker" -eq 0 ] && say ok "the screen saver is not hiding things" "" \
		|| say no "the screen saver is not hiding things" "kill it before measuring"

	rm -f /tmp/check-gl.log
	(cd /boot/home/tests && env -u VK_ICD_FILENAMES -u LD_LIBRARY_PATH \
		GLTEST_SIZE=800x600 ./gltest 2 2 >/tmp/check-gl.log 2>&1 &)
	sleep 12
	gl=$(grep -ao "renderer:.*" /tmp/check-gl.log | head -1)
	case "$gl" in
		*zink*|*radeonsi*) say ok "OpenGL runs on the GPU" "$(grep -ao "[0-9.]* fps.*" /tmp/check-gl.log)";;
		*) say no "OpenGL runs on the GPU" "${gl:-nothing rendered}";;
	esac
	grep -aq "present into the screen: 1" /tmp/check-gl.log 2>/dev/null
	ps | grep "[g]ltest" | awk "{print \$2}" | xargs -r kill -9 2>/dev/null

	r=$(cd /boot/home/tests && ./retracetest 2 2>/dev/null | tail -1)
	case "$r" in
		*"a second"*) say ok "the display reports its blanks" "$r";;
		*) say no "the display reports its blanks" "${r:-nothing}";;
	esac

	echo "displays"
	# screenmode -d asks app_server for the monitors it arranges; each line
	# of the short form is one monitor: number, connector, enabled,
	# connected, x, y, scale, width, height, refresh, main, and the number
	# of the monitor it mirrors (0 for none).
	displays=$(screenmode -d -s 2>/dev/null)
	count=$(echo "$displays" | grep -c "^[0-9]")
	[ "$count" -ge 1 ] && say ok "app_server arranges the monitors" \
		"$(echo "$displays" | awk "{printf \"%s@%s%% \", \$2, \$7}")" \
		|| say no "app_server arranges the monitors" "no display listed"

	# The union of the monitors is what the desktop is; a desktop that does
	# not match means a classic mode was set behind the back of the layout.
	# (screenmode -s would print the frame buffer, which is larger when the
	# desktop is drawn at high density.)
	desk=$(screenmode -d 2>/dev/null | sed -n "s/^Desktop: \([0-9]*\) x \([0-9]*\).*/\1x\2/p")
	union=$(echo "$displays" | awk "\$3==1 && \$12+0==0 {r=\$5+int((\$8*100+\$7/2)/\$7); b=\$6+int((\$9*100+\$7/2)/\$7); if (r>w) w=r; if (b>h) h=b} END {print w\"x\"h}")
	[ "$desk" = "$union" ] && say ok "the desktop is the monitors" "$desk" \
		|| say no "the desktop is the monitors" "desktop $desk, monitors $union"

	# The accelerant writes what it programmed when a layout is applied.
	# It does so once, when the desktop starts, and the syslog is put aside
	# when it is full: look in the one before as well.
	lay=$(cat /var/log/syslog.1 /var/log/syslog 2>/dev/null \
		| grep -a "nvidia_rm: layout:" | tail -1 | sed "s/.*layout: //")
	# amdgpu_kms writes one line per monitor it lights
	[ -n "$lay" ] || lay=$(cat /var/log/syslog.1 /var/log/syslog 2>/dev/null \
		| grep -a "amdgpu_kms: .*@.*, region" | tail -2 \
		| sed "s/.*amdgpu_kms: \([^:]*\): \([^,]*\), region \([0-9,]*\).*/\1 \2 at \3/" | tr "\n" " ")
	[ -n "$lay" ] && say ok "the card was given a layout" "$lay" \
		|| say no "the card was given a layout" "nothing in the syslog"

	echo "sound"
	outputs=$(cd /boot/home/tests && ./audioout 2>/dev/null | grep -c "^[0-9]")
	[ "$outputs" -ge 2 ] && say ok "both outputs are there" "$outputs" \
		|| say no "both outputs are there" "$outputs"
	for i in 0 1; do
		(cd /boot/home/tests && ./audioout $i >/dev/null 2>&1)
		sleep 1
		rate=$(cd /boot/home/tests && ./soundtest 1.5 440 2>/dev/null | grep -o "[0-9]* frames a second" | tail -1)
		[ -n "$rate" ] && say ok "output $i clocks its stream" "$rate" \
			|| say no "output $i clocks its stream" "nothing came back"
	done
	(cd /boot/home/tests && ./audioout 0 >/dev/null 2>&1)

	echo "bluetooth"
	# The radio is a bootloader until the driver hands it its firmware, and
	# it answers nothing at all before that - so the thing worth asking is
	# whether the stack can get the adapter to say who it is, which needs the
	# firmware, the driver, the kernel modules and the server all working.
	fw=$(ls /boot/system/non-packaged/data/firmware/h2generic/*.bin \
		2>/dev/null | head -1)
	[ -n "$fw" ] && say ok "the radio firmware is installed" "$(basename $fw)" \
		|| say no "the radio firmware is installed" "not found"

	srv=$(ps | grep -c "[b]luetooth_server")
	[ "$srv" -ge 1 ] && say ok "the bluetooth server is running" "" \
		|| say no "the bluetooth server is running" "nothing started it"

	# bttest prints the adapter address once the stack has claimed it. This is
	# the one that matters: the radio answers nothing at all until the driver
	# has given it its firmware, so an address here means the whole path from
	# the firmware file through the driver and the kernel modules to the
	# server is working. Asking the syslog whether the firmware loaded would
	# be saying the same thing less reliably, since a log does not survive
	# everything the machine does.
	addr=$(cd /boot/home/tests 2>/dev/null \
		&& timeout 60 ./bttest 1 2>/dev/null \
		| sed -n "s/^adapter [0-9-]*: //p" | head -1)
	[ -n "$addr" ] && say ok "the stack can reach the adapter" "$addr" \
		|| say no "the stack can reach the adapter" "no answer"

	echo "network shares"
	# The add-on is in a package of its own, and so is what it runs in.
	[ -e /system/add-ons/userlandfs/smbfs ] \
		&& [ -e /system/servers/userlandfs_server ] \
		&& say ok "the file system for shares is installed" "" \
		|| say no "the file system for shares is installed" \
			"smbfs or userland_fs is missing"

	# The mount server has the list. A share that is to be there when the
	# system starts and is not is what this is about; one that is mounted by
	# hand is nobody'"'"'s business.
	shares=$(cd /boot/home/tests && ./sharectl list 2>/dev/null)
	wanted=$(echo "$shares" | awk -F"\t" "\$3 == \"startup\"" | grep -c .)
	missing=$(echo "$shares" | awk -F"\t" \
		"\$3 == \"startup\" && \$4 == \"unmounted\" {print \$1 \": \" \$5}")
	if [ "$wanted" -eq 0 ]; then
		say ok "the shares are mounted" "none is set up"
	elif [ -z "$missing" ]; then
		say ok "the shares are mounted" \
			"$(echo "$shares" | awk -F"\t" "\$3 == \"startup\" {printf \"%s \", \$4}")"
	else
		say no "the shares are mounted" "$missing"
	fi

	# Reading only: what is in a share is its owner'"'"'s.
	echo "$shares" | awk -F"\t" "\$4 != \"unmounted\" {print \$4}" \
		| while read -r point; do
			[ -n "$point" ] || continue
			out=$(cd /boot/home/tests && ./sharetest "$point" --reading 2>&1)
			echo "$out" | grep -q "^  NO " \
				&& echo "no|$point|$(echo "$out" | grep "^  NO " | head -1 | cut -c8-47)" \
				|| echo "ok|$point|$(echo "$out" | grep -o "[0-9]* MiB/s in large pieces")"
		done > /tmp/check-shares.log
	while IFS="|" read -r result point detail; do
		say $result "$point reads as a disk does" "$detail"
	done < /tmp/check-shares.log

	echo "camera"
	# A USB camera is a node of the media kit. None plugged in is fine; one
	# that is there has to deliver its pictures, all of them.
	if listusb | grep -qi "webcam\|camera"; then
		grab=$(cd /boot/home/tests && ./camgrab 640 480 3 2>&1 | tail -1)
		rate=$(echo "$grab" | sed -n "s/.*then \([0-9]*\)\.[0-9]* per second.*/\1/p")
		[ -n "$rate" ] && [ "$rate" -ge 14 ] \
			&& say ok "the camera delivers pictures" "$grab" \
			|| say no "the camera delivers pictures" "$grab"
	else
		say ok "the camera delivers pictures" "none is plugged in"
	fi

	echo "the rest"
	# one root hub per USB controller on the PCI bus (five until the card
	# change of 2026-10-10, four since)
	usb=$(listusb | grep -c RootHub)
	controllers=$(listdev 2>/dev/null | grep -c "USB controller")
	[ "$usb" -ge "$controllers" ] && [ "$usb" -ge 1 ] \
		&& say ok "every USB controller is up" "$usb root hubs for $controllers controllers" \
		|| say no "every USB controller is up" "$usb root hubs for $controllers controllers"

	echo
	echo "$pass working, $fail not"
	[ "$fail" -eq 0 ]'
