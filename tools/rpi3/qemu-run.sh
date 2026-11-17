#!/usr/bin/env bash
# Boot the Raspberry Pi loader in QEMU's raspi3b machine (BCM2837).
#   qemu-run.sh [seconds] [extra kernel arguments]
# Prints the serial log (PL011) when the time is up. The boot archive is
# passed as the initrd if it was built.
#   RPI3_BUILD=<dir>            build directory (default /mnt/HaikuWork/rpi3/build)
#   RPI3_SD=<image>             attach an SD card image (padded to a power of two)
#   RPI3_SCREENSHOT=<file.png>  save the frame buffer at the end
#   RPI3_QEMU_ARGS=...          more QEMU arguments
#   RPI3_QEMU_TB=multi          QEMU's normal translation blocks (faster)
set -euo pipefail
BUILD=${RPI3_BUILD:-/mnt/HaikuWork/rpi3/build}
FIRMWARE=${RPI3_FIRMWARE:-/mnt/HaikuWork/rpi3/firmware-all}
QEMU=${QEMU:-/mnt/HaikuWork/build/qemu-10.2.0/qemu-system-aarch64}
SECONDS_TO_RUN=${1:-20}
shift || true
LOADER=$BUILD/objects/haiku/arm64/release/system/boot/rpi/haiku_loader.rpi
ARCHIVE=${RPI3_ARCHIVE:-$BUILD/airos-boot.tgz}
LOG=$(mktemp "${TMPDIR:-/mnt/HaikuWork/tmp}/rpi3-qemu.XXXXXX")
trap 'rm -f "$LOG" "$LOG.ppm" "$LOG.dtb"' EXIT
# The firmware would fix the tree up: the console alias (the PL011, as with
# the image's miniuart-bt overlay), the memory size and the core clock.
cp "$FIRMWARE/bcm2710-rpi-3-b.dtb" "$LOG.dtb"
fdtput -t s "$LOG.dtb" /aliases serial0 /soc/serial@7e201000
fdtput -t s "$LOG.dtb" /aliases serial1 /soc/serial@7e215040
fdtput -t x "$LOG.dtb" /memory@0 reg 0 0x3c000000
args=(-M raspi3b -kernel "$LOADER" -dtb "$LOG.dtb"
    -append "$*" -serial "file:$LOG" -serial null -display none -monitor stdio)
[ "${RPI3_QEMU_TB:-}" = multi ] || args+=(-accel tcg,one-insn-per-tb=on)
[ -f "$ARCHIVE" ] && args+=(-initrd "$ARCHIVE")
[ -n "${RPI3_SD:-}" ] && args+=(-drive "file=$RPI3_SD,if=sd,format=raw")
{
    sleep "$SECONDS_TO_RUN"
    [ -n "${RPI3_SCREENSHOT:-}" ] && echo "screendump $LOG.ppm" && sleep 1
    echo quit
} | "$QEMU" "${args[@]}" ${RPI3_QEMU_ARGS:-} >/dev/null 2>&1 || true
if [ -n "${RPI3_SCREENSHOT:-}" ] && [ -f "$LOG.ppm" ]; then
    python3 -c "from PIL import Image; import sys; Image.open(sys.argv[1]).save(sys.argv[2])" \
        "$LOG.ppm" "$RPI3_SCREENSHOT"
fi
cat "$LOG"
