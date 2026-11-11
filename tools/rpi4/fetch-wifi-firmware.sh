#!/usr/bin/env bash
# Fetch the firmware of the Raspberry Pi 4's Wi-Fi chip (CYW43455) and of the
# Pi 400's (CYW43456: the same chip ID at revision 9, another firmware) from
# Raspberry Pi's firmware-nonfree repository, under the names the
# broadcomfmac driver asks for. Redistributable (Cypress licence), not part
# of this tree.
#   fetch-wifi-firmware.sh [Wi-Fi directory] [Bluetooth directory]
set -euo pipefail
DIR=${1:-/mnt/HaikuWork/rpi4/wifi/firmware}
BASE=https://raw.githubusercontent.com/RPi-Distro/firmware-nonfree/bookworm/debian/config/brcm80211
mkdir -p "$DIR"
fetch() {
    [ -s "$DIR/$1" ] || curl -sfL --retry 3 -o "$DIR/$1" "$BASE/$2"
}
fetch brcmfmac43455-sdio.bin cypress/cyfmac43455-sdio-standard.bin
fetch brcmfmac43455-sdio.clm_blob cypress/cyfmac43455-sdio.clm_blob
fetch brcmfmac43455-sdio.txt brcm/brcmfmac43455-sdio.txt
fetch brcmfmac43456-sdio.bin brcm/brcmfmac43456-sdio.bin
fetch brcmfmac43456-sdio.clm_blob brcm/brcmfmac43456-sdio.clm_blob
fetch brcmfmac43456-sdio.txt brcm/brcmfmac43456-sdio.txt
(cd "$DIR" && sha256sum brcmfmac4345[56]-sdio.* > SHA256SUMS)
echo "CYW43455 and CYW43456 firmware -> $DIR"

# The Bluetooth half of the chip: its patch file, for the h4bcm driver.
BT_DIR=${2:-/mnt/HaikuWork/rpi4/bluetooth}
mkdir -p "$BT_DIR"
[ -s "$BT_DIR/BCM4345C0.hcd" ] || curl -sfL --retry 3 -o "$BT_DIR/BCM4345C0.hcd" \
    https://github.com/RPi-Distro/bluez-firmware/raw/bookworm/debian/firmware/broadcom/BCM4345C0.hcd
(cd "$BT_DIR" && sha256sum BCM4345C0.hcd > SHA256SUMS)
echo "BCM4345C0 patch -> $BT_DIR"
