#!/usr/bin/env bash
# Collect the Cubie A7S's hardware facts from the Debian recovery system
# (Radxa's BSP kernel) into an evidence directory, as the reference for
# Haiku's drivers.
#   linux-survey.sh <output directory>
set -euo pipefail
OUT=$1
CUBIE_STATE=${CUBIE_STATE:-/mnt/HaikuWork/cubie/state}
SSH=(ssh -F "$CUBIE_STATE/ssh_config" cubie-recovery)
mkdir -p "$OUT"
run() { # <file> <command>
	"${SSH[@]}" "$2" > "$OUT/$1" 2>&1 || true
}
run uname.txt 'uname -a; cat /etc/os-release; cat /proc/cmdline'
run dmesg.txt 'dmesg'
run iomem.txt 'cat /proc/iomem'
run interrupts.txt 'cat /proc/interrupts'
run cpuinfo.txt 'cat /proc/cpuinfo; lscpu'
run meminfo.txt 'cat /proc/meminfo; free -m'
run block.txt 'lsblk -o NAME,SIZE,TYPE,MODEL,SERIAL,PARTLABEL,MOUNTPOINT; ls -l /dev/disk/by-partlabel; for d in /sys/block/mmcblk*; do echo $d; cat $d/device/type $d/device/name $d/device/cid 2>/dev/null; done'
run usb.txt 'lsusb; lsusb -t; cat /sys/kernel/debug/usb/devices 2>/dev/null'
run net.txt 'ip -d link; ip addr; ethtool eth0 2>/dev/null; ethtool -i eth0 2>/dev/null; iw dev 2>/dev/null'
run modules.txt 'lsmod'
run drm.txt 'ls -l /sys/class/drm; for c in /sys/class/drm/card*-*; do echo $c; cat $c/status $c/modes 2>/dev/null; done; cat /sys/kernel/debug/dri/*/state 2>/dev/null | head -200'
run typec.txt 'ls -lR /sys/class/typec 2>/dev/null; for f in /sys/class/typec/*/*; do [ -f $f ] && echo "$f: $(cat $f 2>/dev/null)"; done; cat /sys/kernel/debug/usb/tcpm* 2>/dev/null'
run gpu.txt 'ls /sys/class/misc 2>/dev/null; dmesg | grep -i -E "pvr|gpu|img|rogue|bvnc"; cat /sys/kernel/debug/pvr/*/version 2>/dev/null; ls /dev/dri 2>/dev/null'
run clocks.txt 'cat /sys/kernel/debug/clk/clk_summary 2>/dev/null'
run pinctrl.txt 'cat /sys/kernel/debug/pinctrl/*/pinmux-pins 2>/dev/null'
run devicetree.txt 'dtc -I fs -O dts /proc/device-tree 2>/dev/null || ls -R /proc/device-tree | head -500'
ls -la "$OUT"
