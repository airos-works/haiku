#!/bin/bash
# Set or clear loader words in the SD card's cmdline.txt from the recovery
# OS, then start the SD card's air/OS again: for a test driver in
# /boot/system/non-packaged that brings the system down at every start.
#
#   safe-boot.sh add airos.noaddons     start without user add-ons
#   safe-boot.sh remove airos.noaddons
#   safe-boot.sh put <local file> <path on the FAT partition>
#
# Power through Home Assistant (tools/rpi4/power.py with RPI4_STATE), the
# recovery OS from the KVM's virtual disk (EEPROM boot order: USB first).
set -euo pipefail
action=$1; word=$2; shift 2 || true
STATE=${RPI5_STATE:-/mnt/HaikuWork/rpi5/state}
TOOLS=$(cd "$(dirname "$0")" && pwd)
export RPI4_STATE=$STATE
power() { python3 "$TOOLS/../rpi4/power.py" "$@" >/dev/null; }
recovery() { ssh -F "$STATE/ssh_config" rpi5-recovery "$@"; }

power off; sleep 3
"$TOOLS/lab.sh" lun /data/rpi5-recovery.img
power on
for i in $(seq 1 60); do recovery true 2>/dev/null && break; sleep 5; done
if [ "$action" = put ]; then
	recovery "mkdir -p /mnt/sd && mount /dev/mmcblk0p1 /mnt/sd && mkdir -p \$(dirname /mnt/sd/$1)"
	cat "$word" | recovery "cat > /mnt/sd/$1 && sync && ls -la /mnt/sd/$1 && umount /mnt/sd"
else
recovery "set -e; mkdir -p /mnt/sd; mount /dev/mmcblk0p1 /mnt/sd
	words=\$(cat /mnt/sd/cmdline.txt | tr -s ' \n' ' ')
	words=\$(echo \" \$words \" | sed 's/ $word / /g')
	[ '$action' = add ] && words=\"\$words $word\"
	echo \$words > /mnt/sd/cmdline.txt
	echo \"cmdline.txt: \$(cat /mnt/sd/cmdline.txt)\"
	umount /mnt/sd; sync"
fi
# free the virtual disk (the gadget otherwise says it is busy)
recovery 'umount /media/usb 2>/dev/null; echo 1 > /sys/block/sda/device/delete' || true
power off; sleep 3
"$TOOLS/lab.sh" lun /data/rpi5-idle.img
power on
echo "starting the SD card's air/OS"
