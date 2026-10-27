# Shell functions for the Cubie A7S lab card's ESP, used through the Debian
# recovery system over ssh. Source this from bash; it needs $CUBIE_STATE.
#
# The ESP is the partition labelled "efi". Radxa's Debian automounts it at
# /boot/efi; on other systems it is mounted at /mnt/esp. lab_esp_mount
# prints the mount point.

CUBIE_STATE=${CUBIE_STATE:-/mnt/HaikuWork/cubie/state}
LAB_SSH=(ssh -F "$CUBIE_STATE/ssh_config" cubie-recovery)

lab_esp_mount() {
	"${LAB_SSH[@]}" 'set -e
		if grep -q "[[:space:]]/boot/efi[[:space:]]" /etc/fstab; then
			ls /boot/efi >/dev/null
			echo /boot/efi
		else
			mkdir -p /mnt/esp
			mountpoint -q /mnt/esp || mount /dev/disk/by-partlabel/efi /mnt/esp
			echo /mnt/esp
		fi'
}

# lab_esp_put <local file> <path on the ESP>: copy, read back, compare
lab_esp_put() {
	local esp want have
	esp=$(lab_esp_mount)
	"${LAB_SSH[@]}" "mkdir -p \"\$(dirname '$esp/$2')\" && cat > '$esp/$2.new'" < "$1"
	want=$(sha256sum < "$1" | cut -d' ' -f1)
	have=$("${LAB_SSH[@]}" "sha256sum < '$esp/$2.new'" | cut -d' ' -f1)
	if [[ $want != "$have" ]]; then
		echo "$2 read back $have, expected $want" >&2
		return 1
	fi
	"${LAB_SSH[@]}" "mv '$esp/$2.new' '$esp/$2' && sync"
}

# lab_restart_once: have the next boot start the air/OS U-Boot (and Haiku)
# once, then reboot
lab_restart_once() {
	local esp
	esp=$(lab_esp_mount)
	"${LAB_SSH[@]}" "printf 1 > '$esp/airos-once' && sync; systemctl reboot" \
		|| true
}
