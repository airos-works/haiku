#!/usr/bin/env bash
# Build air/OS's device tree for the Radxa Cubie A7S from data/dts/cubie-a7s
# (Radxa U-Boot's A733 sources plus the nodes Haiku's drivers use).
#   build-dtb.sh <output.dtb>
# U-Boot's distro boot hands dtb/allwinner/sun60i-a733-cubie-a7s.dtb on the
# ESP to an EFI program; the lab's boot.scr uses airos/cubie-a7s.dtb.
set -euo pipefail
OUT=$1
SRC=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../data/dts/cubie-a7s" && pwd)
cpp -nostdinc -undef -D__DTS__ -x assembler-with-cpp -I "$SRC/include" -I "$SRC" \
	"$SRC/sun60i-a733-cubie-a7s.dts" \
	| dtc -q -I dts -O dtb -@ -o "$OUT" -
echo "$OUT: $(stat -c %s "$OUT") bytes"
