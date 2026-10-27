#!/bin/bash
# Restore the saved EFI loader from a make-linux-trial.py bundle, on Linux.
# Arguments: FAT UUID, original-loader SHA-256, trial-loader SHA-256.
set -euo pipefail
[[ $# == 3 && $(id -u) == 0 ]]
[[ $1 =~ ^[[:xdigit:]]{4}-[[:xdigit:]]{4}$ ]]
[[ $2 =~ ^[0-9a-f]{64}$ && $3 =~ ^[0-9a-f]{64}$ ]]

device=$(findfs "UUID=$1")
[[ -b $device && $device != *$'\n'* ]]
mountpoint=$(mktemp -d /run/wx5100-esp.XXXXXX)
staged=
mounted=0
cleanup() {
    if [[ -n $staged ]]; then rm -f -- "$staged"; fi
    if [[ $mounted == 1 ]]; then umount "$mountpoint"; fi
    rmdir "$mountpoint"
}
trap cleanup EXIT
mount -o rw,nosuid,nodev,noexec "$device" "$mountpoint"
mounted=1
active="$mountpoint/EFI/BOOT/BOOTX64.EFI"
backup="$mountpoint/EFI/WX5100/HAIKU.EFI"
digest() { sha256sum "$1" | cut -d ' ' -f 1; }
[[ $(digest "$backup") == "$2" ]]
current=$(digest "$active")
if [[ $current == "$2" ]]; then
    echo 'PASS: original Haiku EFI loader already restored'
    exit 0
fi
[[ $current == "$3" ]]
grep -qx 'wx_trial=consumed' "$mountpoint/EFI/WX5100/GRUBENV"
staged=$(mktemp "$mountpoint/EFI/BOOT/WXRESTORE.XXXXXX")
cp -- "$backup" "$staged"
[[ $(digest "$staged") == "$2" ]]
sync -f "$staged"
mv -f -- "$staged" "$active"
staged=
sync -f "$mountpoint"
[[ $(digest "$active") == "$2" ]]
echo 'PASS: original Haiku EFI loader restored and verified'
