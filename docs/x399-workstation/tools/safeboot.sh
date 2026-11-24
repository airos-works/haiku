#!/bin/bash
# Boot the workstation with user add-ons disabled, driven entirely over the
# NanoKVM (which has no video): force it off, power it on, catch the Haiku
# boot loader with SPACE, tick "Disable user add-ons" and continue booting.
#
# For when an add-on in a non-packaged directory stops the machine during
# boot. Nothing here needs the screen; the key sequence is the loader's own
# menu order, verified on this machine 2026-09-19.
#
# usage: safeboot.sh
set -uo pipefail
X399=/mnt/HaikuWork/x399
K="/mnt/HaikuWork/nanokvm/.venv/bin/python $X399/tools/kvm.py"
export NANOKVM_URL=${NANOKVM_URL:-http://192.168.1.9}
export NANOKVM_SESSION=${NANOKVM_SESSION:-$X399/state/nanokvm-session.json}
[ -n "${NANOKVM_PASSWORD:-}" ] || export NANOKVM_PASSWORD=$(cat $X399/state/nanokvm-password)

led() {
	$K status 2>/dev/null | python3 -c \
		'import sys,json; print(int(json.load(sys.stdin)["/api/vm/gpio"]["pwr"]))' \
		2>/dev/null || echo 1
}

if [ "$(led)" = 1 ]; then
	$K power power --duration 6000 >/dev/null || true
	n=0
	for i in $(seq 40); do
		if [ "$(led)" = 0 ]; then n=$((n + 1)); [ $n -ge 8 ] && break; else n=0; fi
		sleep 1
	done
	[ $n -ge 8 ] || { echo "workstation did not power off" >&2; exit 1; }
fi
sleep 5
$K power power --duration 800 >/dev/null
echo "powered on, waiting for the boot loader"

for i in $(seq 45); do
	$K key SPACE >/dev/null 2>&1
	sleep 1
done

key() { $K key "$1" >/dev/null 2>&1; sleep 1; }
key DOWN; key ENTER		# Select safe mode options
key DOWN; key ENTER		# [x] Disable user add-ons
key ESC
key DOWN; key DOWN; key DOWN; key DOWN
key ENTER			# Continue booting
echo "asked to boot without user add-ons"
