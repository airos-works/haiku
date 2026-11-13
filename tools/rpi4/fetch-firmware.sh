#!/usr/bin/env bash
# Fetch the Raspberry Pi firmware files that go on the boot partition, at a
# pinned tag of https://github.com/raspberrypi/firmware, and record their
# checksums. They are redistributable (LICENCE.broadcom) but not part of this
# tree.
#   fetch-firmware.sh [directory]
# The default directory is /mnt/HaikuWork/rpi4/firmware/<tag>; point
# HAIKU_RPI_FIRMWARE_DIR (tools/rpi4/UserBuildConfig) at it.
set -euo pipefail
TAG=${RPI_FIRMWARE_TAG:-1.20260915}
DIR=${1:-/mnt/HaikuWork/rpi4/firmware/$TAG}
BASE=https://raw.githubusercontent.com/raspberrypi/firmware/$TAG/boot
FILES=(
    start4.elf fixup4.dat LICENCE.broadcom
    bcm2711-rpi-4-b.dtb bcm2711-rpi-400.dtb bcm2711-rpi-cm4.dtb
    # Raspberry Pi 5, 500 and CM5: the EEPROM bootloader reads config.txt
    # and starts the loader itself; it only needs their device trees.
    bcm2712-rpi-5-b.dtb bcm2712d0-rpi-5-b.dtb bcm2712-d-rpi-5-b.dtb
    bcm2712-rpi-500.dtb bcm2712-rpi-cm5-cm5io.dtb bcm2712-rpi-cm5l-cm5io.dtb
    bcm2712-rpi-cm5-cm4io.dtb bcm2712-rpi-cm5l-cm4io.dtb
    overlays/overlay_map.dtb overlays/disable-bt.dtbo overlays/miniuart-bt.dtbo
    overlays/vc4-kms-v3d-pi4.dtbo
)
mkdir -p "$DIR/overlays"
for file in "${FILES[@]}"; do
    [ -s "$DIR/$file" ] || curl -sfL --retry 3 -o "$DIR/$file" "$BASE/$file"
done
(cd "$DIR" && sha256sum "${FILES[@]}" > SHA256SUMS)
echo "$TAG -> $DIR"
