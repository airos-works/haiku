#!/usr/bin/env bash
# deploy-inplace.sh: swap haiku.hpkg (and the aic8800wifi driver) and the
# image's Mesa files on the Cubie's running air/OS lab install, keeping home,
# settings and Bluetooth bonds, then reboot into it through Debian
# (lab_restart_once). Run under board-lock.sh.
#
# BUILD (default cubie/build-wifi), BOARD (192.168.1.118), HOST (this
# machine as the board sees it), SKIP_PKG=1 for the driver only, SKIP_MESA=1
# to leave Mesa alone, MESA_DIR for a Mesa other than the pinned release.
#
# The package goes in under the installed package's file name: packagefs
# activates what is in packages/, and keeping the name keeps the activated
# state's bookkeeping simple. Originals go to /boot/home/bthid/backup once.
# Each staged file is synced before the swap: a 35 MB package and its copy
# flushed at once hung the SD write-back once (2026-10-12), the board died
# and the staged file read back different after the power cycle.
set -euo pipefail
BUILD=${BUILD:-/mnt/HaikuWork/cubie/build-wifi}
PKG=$BUILD/objects/haiku/arm64/packaging/packages/haiku.hpkg
WIFI=$(find "$BUILD"/objects/haiku/arm64/release/add-ons/kernel/drivers \
	-name aic8800wifi -type f | head -1)
BOARD=${BOARD:-192.168.1.118}
HOST=${HOST:-192.168.1.64}
WIFIPATH=/boot/system/non-packaged/add-ons/kernel/drivers/bin/aic8800wifi
STAGE=/boot/home/bthid/stage
BACKUP=/boot/home/bthid/backup
TOOLS=$(cd "$(dirname "$0")" && pwd)
export RPI4_STATE=${RPI4_STATE:-/mnt/HaikuWork/cubie/state}
SHELL_PY=/mnt/HaikuWork/rpi4/haiku/tools/rpi4/shell.py
SERVE_ONCE=/mnt/HaikuWork/retrace/serve-once.py
# The image's Mesa (CUBIE_MESA_RELEASE in UserBuildConfig): zink, the PowerVR
# Vulkan driver, the EGL vendor file and the GPU test programs are
# non-packaged files, outside haiku.hpkg.
MESA_RELEASE=$(sed -n 's/^CUBIE_MESA_RELEASE ?= \([0-9a-f]*\) ;.*/\1/p' \
	"$TOOLS/UserBuildConfig")
MESA_DIR=${MESA_DIR:-/mnt/HaikuWork/cubie/mesa/release/$MESA_RELEASE}
mesa_files=()
if [[ -z ${SKIP_MESA:-} ]]; then
	[[ -f $MESA_DIR/MANIFEST ]] || { echo "no Mesa release in $MESA_DIR" >&2; exit 1; }
	for f in libEGL_mesa.so.0 libvulkan_powervr_mesa.so libvulkan.so.1; do
		mesa_files+=("$f /boot/system/non-packaged/lib")
	done
	mesa_files+=("10_mesa.json /boot/system/non-packaged/add-ons/opengl/egl_vendor.d")
	for f in pvr_vkprobe pvr_vkfill pvr_vkfence pvr_vktriangle pvr_vkhang \
			pvr_vkbench pvr_present pvr_copybench pvr_glprobe pvr_glbench \
			pvr_glreset pvr_glpresent pvr_glcomposite pvr_glxfb pvr_glconform \
			tls_generation_check libtls_generation_check.so; do
		[[ -e $MESA_DIR/$f ]] && mesa_files+=("$f /boot/system/non-packaged/bin")
	done
	echo "mesa    $MESA_DIR (${#mesa_files[@]} files)"
fi

b() { timeout "${2:-120}" python3 "$SHELL_PY" "$BOARD" "$1"; }

# The minimum profile writes the same haiku.hpkg path; it lacks the desktop.
if [[ -z ${SKIP_PKG:-} ]] && [[ $("$BUILD"/objects/linux/x86_64/release/tools/package/package \
		list -p "$PKG" | grep -c 'lib/libbluetooth.so$') == 0 ]]; then
	echo "$PKG is not a full @cubie-airos package" >&2
	exit 1
fi
OLDPKG=$(b "ls /boot/system/packages" | grep -o '^haiku-r1[^ ]*\.hpkg' | head -1)
[[ -n $OLDPKG ]] || { echo "no haiku package on the board" >&2; exit 1; }
echo "package $PKG ($(sha256sum < "$PKG" | cut -c1-16)) as $OLDPKG"
echo "wifi    $WIFI ($(sha256sum < "$WIFI" | cut -c1-16))"
b "mkdir -p $STAGE $BACKUP; [ -e $BACKUP/$OLDPKG ] || cp /boot/system/packages/$OLDPKG $BACKUP/; [ -e $BACKUP/aic8800wifi ] || cp $WIFIPATH $BACKUP/; sync"

send() {
	local file=$1 name=$2 port=$3
	python3 "$SERVE_ONCE" "$port" "$file" &
	local server=$!
	sleep 1
	b "cubie_fetch $HOST $port $STAGE/$name" 300
	wait $server || true
	local want have
	want=$(sha256sum < "$file" | cut -d' ' -f1)
	have=$(b "sha256sum $STAGE/$name" | grep -o '^[0-9a-f]\{64\}' | head -1)
	if [[ $want != "$have" ]]; then
		echo "$name: board has $have, expected $want" >&2
		exit 1
	fi
	b "sync" 600
	echo "$name staged, sha256 ok, synced"
}
[[ -n ${SKIP_PKG:-} ]] || send "$PKG" haiku.hpkg 17781
send "$WIFI" aic8800wifi 17782
port=17783
swap_mesa=true
for entry in "${mesa_files[@]}"; do
	name=${entry%% *}
	dir=${entry#* }
	b "[ -e $BACKUP/$name ] || [ ! -e $dir/$name ] || cp $dir/$name $BACKUP/; sync"
	send "$MESA_DIR/$name" "$name" $port
	port=$((port + 1))
	swap_mesa="$swap_mesa && mkdir -p $dir && cp $STAGE/$name $dir/$name.new && chmod 755 $dir/$name.new && mv -f $dir/$name.new $dir/$name"
done

swap_wifi="cp $STAGE/aic8800wifi $WIFIPATH.new && mv -f $WIFIPATH.new $WIFIPATH"
if [[ -n ${SKIP_PKG:-} ]]; then
	b "$swap_wifi && sync && sha256sum $WIFIPATH" 300
else
	b "cp $STAGE/haiku.hpkg /boot/system/packages/$OLDPKG.new && mv -f /boot/system/packages/$OLDPKG.new /boot/system/packages/$OLDPKG && $swap_wifi && sync && sha256sum /boot/system/packages/$OLDPKG $WIFIPATH" 900
fi
if (( ${#mesa_files[@]} )); then
	b "$swap_mesa && sync && ls -l /boot/system/non-packaged/lib/libEGL_mesa.so.0" 600
	echo "Mesa $MESA_RELEASE installed"
fi
echo "rebooting (a warm reboot ends in Debian, then air/OS once)"
b "shutdown -r" 20 || true
debian=
for i in $(seq 1 72); do
	sleep 5
	if timeout 5 ssh -F "$RPI4_STATE/ssh_config" -o ConnectTimeout=3 \
			cubie-recovery true 2>/dev/null; then
		echo "Debian up after $((i * 5)) s"
		debian=1
		break
	fi
done
if [[ -z $debian ]]; then
	if b "ps" 10 2>/dev/null | grep -q "shutdown process"; then
		echo "air/OS is still shutting down: an application is probably" \
			"asking something (\"Save changes?\"); answer it, then run" \
			"lab_restart_once (lab-esp.sh) once Debian is up" >&2
	else
		echo "Debian did not come up; power cycle, then lab_restart_once" >&2
	fi
	exit 2
fi
# shellcheck source=lab-esp.sh
source "$TOOLS/lab-esp.sh"
lab_restart_once
sleep 15
for i in $(seq 1 60); do
	sleep 5
	if ping -c1 -W1 "$BOARD" >/dev/null 2>&1; then
		echo "air/OS up after $((i * 5 + 15)) s"
		exit 0
	fi
done
echo "air/OS did not come back; power cycle, then lab_restart_once" >&2
exit 1
