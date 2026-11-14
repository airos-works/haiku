#!/bin/bash
# Raspberry Pi 5 lab helpers (NanoKVM at 192.168.1.39; serial: the KVM's
# ttyS1 is wired to the Pi 5's GPIO 14/15, the RP1's UART0).
#
#   lab.sh serial-start          capture the serial line into the KVM's /tmp/pi5-serial.log
#   lab.sh serial [bytes]        print the end of the capture (default 20000 bytes)
#   lab.sh serial-send <text>    write text to the Pi's console (no newline added)
#   lab.sh lun <kvm file>|none   put a file of the KVM's /data in the virtual USB disk
#   lab.sh upload <file> <name>  copy an image to the KVM's /data/<name> (checked)
#   lab.sh update-boot <file> <name>  the same for the first 65 MiB only (the
#                                boot partition): the running system's BFS stays
#   lab.sh update <file> <name>  overwrite /data/<name> in place, same size: the
#                                virtual disk stays attached. Swapping the LUN while
#                                the Pi's EEPROM reads it wedges the EEPROM for good
#   lab.sh shot <jpg>            screenshot of HDMI0
#
# Credentials stay in $RPI5_STATE (default /mnt/HaikuWork/rpi5/state):
# ssh_config (hosts kvm5, rpi5-recovery), nanokvm.password, env.sh.
set -euo pipefail
STATE=${RPI5_STATE:-/mnt/HaikuWork/rpi5/state}
kvm_ssh() { ssh -F "$STATE/ssh_config" kvm5 "$@"; }
LUN=/sys/kernel/config/usb_gadget/g0/functions/mass_storage.disk0/lun.0

case ${1:-} in
serial-start)
	kvm_ssh "killall -q cat; stty -F /dev/ttyS1 115200 raw -echo -crtscts;
		nohup cat /dev/ttyS1 >> /tmp/pi5-serial.log 2>/dev/null </dev/null &
		echo capturing" ;;
serial)
	kvm_ssh "tail -c ${2:-20000} /tmp/pi5-serial.log" | tr -d '\000' ;;
serial-clear)
	kvm_ssh ": > /tmp/pi5-serial.log" ;;
serial-send)
	kvm_ssh "printf '%s' '$2' > /dev/ttyS1" ;;
lun)
	# The gadget refuses a new file while the Pi holds the medium ("Resource
	# busy", even after Linux deleted the disk): then unbind the gadget for
	# a moment, which also disconnects the KVM's keyboard and mouse.
	file=$2; [ "$file" = none ] && file=
	kvm_ssh "G=/sys/kernel/config/usb_gadget/g0
		if ! { echo > $LUN/file && echo 0 > $LUN/cdrom && echo 0 > $LUN/ro \\
				&& echo '$file' > $LUN/file; } 2>/dev/null; then
			U=\$(cat \$G/UDC); echo > \$G/UDC; sleep 1
			echo 0 > $LUN/ro; echo '$file' > $LUN/file
			echo \$U > \$G/UDC
		fi
		echo '$file' > /boot/usb.disk0; sync
		echo \"LUN: \$(cat $LUN/file)\"" ;;
upload)
	sum=$(sha256sum "$2" | cut -d' ' -f1)
	gzip -1 -c "$2" | kvm_ssh "gunzip -c > /data/$3.part && mv /data/$3.part /data/$3 && sync"
	remote=$(kvm_ssh "sha256sum /data/$3" | cut -d' ' -f1)
	[ "$sum" = "$remote" ] || { echo "checksum mismatch" >&2; exit 1; }
	echo "uploaded /data/$3 $sum" ;;
update)
	sum=$(sha256sum "$2" | cut -d' ' -f1)
	gzip -1 -c "$2" | kvm_ssh "gunzip -c > /data/$3.new && dd if=/data/$3.new of=/data/$3 bs=1M conv=notrunc 2>/dev/null && rm /data/$3.new && sync"
	remote=$(kvm_ssh "sha256sum /data/$3" | cut -d' ' -f1)
	[ "$sum" = "$remote" ] || { echo "checksum mismatch" >&2; exit 1; }
	echo "updated /data/$3 in place $sum" ;;
update-boot)
	head -c $((65 * 1048576)) "$2" | gzip -1 | kvm_ssh "gunzip -c > /data/$3.boot && dd if=/data/$3.boot of=/data/$3 bs=1M conv=notrunc 2>/dev/null && rm /data/$3.boot && sync"
	echo "updated the boot partition of /data/$3" ;;
shot)
	. "$STATE/env.sh"
	/mnt/HaikuWork/rpi4/venv/bin/python /mnt/HaikuWork/src/haiku/tools/rock5-itx/nanokvm.py screenshot "$2" ;;
*)
	sed -n '2,13p' "$0"; exit 1 ;;
esac
