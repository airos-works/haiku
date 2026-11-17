#!/bin/bash
# Raspberry Pi 3 lab helpers (NanoKVM-PCIe at 192.168.1.33; its ttyS1 is
# meant to be wired to the Pi's GPIO 14/15, the PL011 console).
#
#   lab.sh serial-start          capture the serial line into the KVM's /tmp/pi3-serial.log
#   lab.sh serial [bytes]        print the end of the capture (default 20000 bytes)
#   lab.sh serial-clear          empty the capture
#   lab.sh serial-send <text>    write text to the Pi's console (no newline added)
#   lab.sh gadget msd|full       the KVM's USB gadget: the disk only (the Pi 3's
#                                bootcode.bin boots it only so), or the stock
#                                composite (keyboard, mouse, disk, RNDIS)
#   lab.sh lun <kvm file>|none   put a file of the KVM's /data in the virtual USB disk
#   lab.sh upload <file> <name>  copy an image to the KVM's /data/<name> (checked)
#   lab.sh power on|off|cycle|status   the Home Assistant plug (shared with
#                                the Dragon Q8B: switch.attic_rack_power)
#   lab.sh shot <jpg>            screenshot of the HDMI output
#
# The Pi 3's boot ROM does not boot the KVM's disk; an SD card with only
# bootcode.bin (and an empty "timeout" file) does, with the gadget in "msd"
# mode (docs/rpi3/BOOT.md).
#
# Credentials stay in $RPI3_STATE (default /mnt/HaikuWork/rpi3/state):
# ssh_config (host kvm3), nanokvm.password, env.sh, homeassistant.json/token.
set -euo pipefail
STATE=${RPI3_STATE:-/mnt/HaikuWork/rpi3/state}
HERE=$(cd "$(dirname "$0")" && pwd)
kvm_ssh() {
	local try status
	for try in 1 2 3 4 5; do
		ssh -F "$STATE/ssh_config" kvm3 "$@"
		status=$?
		[ $status -ne 255 ] && return $status
		sleep 3
	done
	return $status
}
LUN=/sys/kernel/config/usb_gadget/g0/functions/mass_storage.disk0/lun.0

gadget_script='G=/sys/kernel/config/usb_gadget/g0
cd $G || exit 1
U=$(ls /sys/class/udc | head -1)
echo > UDC 2>/dev/null
sleep 1
for f in rndis.usb0 hid.GS0 hid.GS1 hid.GS2 mass_storage.disk0; do rm -f configs/c.1/$f; done
rm -f os_desc/c.1
case "$1" in
msd)
	echo 0x00 > bDeviceClass; echo 0x00 > bDeviceSubClass; echo 0x00 > bDeviceProtocol
	ln -s functions/mass_storage.disk0 configs/c.1/ ;;
full)
	echo 0xEF > bDeviceClass; echo 0x02 > bDeviceSubClass; echo 0x01 > bDeviceProtocol
	[ -d functions/rndis.usb0 ] && { ln -s functions/rndis.usb0 configs/c.1/; ln -s configs/c.1 os_desc; }
	for f in hid.GS0 hid.GS1 hid.GS2 mass_storage.disk0; do [ -d functions/$f ] && ln -s functions/$f configs/c.1/; done ;;
esac
echo $U > UDC
echo "gadget $1: $(ls configs/c.1 | grep -v -e MaxPower -e bmAttributes -e strings | tr "\n" " ")"'

case ${1:-} in
serial-start)
	kvm_ssh "killall -q cat; stty -F /dev/ttyS1 115200 raw -echo -crtscts;
		nohup cat /dev/ttyS1 >> /tmp/pi3-serial.log 2>/dev/null </dev/null >/dev/null &
		echo capturing" ;;
serial)
	kvm_ssh "tail -c ${2:-20000} /tmp/pi3-serial.log" | tr -d '\000' ;;
serial-clear)
	kvm_ssh ": > /tmp/pi3-serial.log" ;;
serial-send)
	kvm_ssh "printf '%s' '$2' > /dev/ttyS1" ;;
gadget)
	case ${2:-} in msd|full) ;; *) echo "gadget msd|full" >&2; exit 1 ;; esac
	kvm_ssh "sh -s $2" <<<"$gadget_script" ;;
lun)
	file=$2; [ "$file" = none ] && file=
	kvm_ssh "echo > $LUN/file 2>/dev/null; echo 0 > $LUN/cdrom; echo 0 > $LUN/ro;
		echo '$file' > $LUN/file && echo '$file' > /boot/usb.disk0; sync
		echo \"LUN: \$(cat $LUN/file)\"" ;;
upload)
	sum=$(sha256sum "$2" | cut -d' ' -f1)
	gzip -1 -c "$2" | kvm_ssh "gunzip -c > /data/$3.part && mv /data/$3.part /data/$3 && sync"
	remote=$(kvm_ssh "sha256sum /data/$3" | cut -d' ' -f1)
	[ "$sum" = "$remote" ] || { echo "checksum mismatch" >&2; exit 1; }
	echo "uploaded /data/$3 $sum" ;;
power)
	RPI4_STATE=$STATE RPI4_OFF_SECONDS=${RPI3_OFF_SECONDS:-8} \
		python3 "$HERE/../rpi4/power.py" "${2:-status}" ;;
shot)
	. "$STATE/env.sh"
	/mnt/HaikuWork/rpi4/venv/bin/python "$HERE/../rock5-itx/nanokvm.py" screenshot "$2" ;;
*)
	sed -n '2,24p' "$0"; exit 1 ;;
esac
