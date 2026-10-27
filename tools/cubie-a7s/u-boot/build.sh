#!/usr/bin/env bash
# Build the air/OS U-Boot for the Cubie A7S: Radxa's A733 U-Boot (radxa/u-boot,
# branch u-boot-dlan17, commit 9da4b3e8) with 0001-sunxi-chainload.patch and
# chainload.config, so that the BSP U-Boot 2018 of Radxa's SD card boot chain
# can start it with booti and it provides UEFI to Haiku's loader.
#
#   build.sh [output u-boot.bin]
#
# UBOOT_SRC     source tree, cloned if missing (default
#               /mnt/HaikuWork/cubie/src/u-boot-radxa)
# UBOOT_BUILD   build directory (default /mnt/HaikuWork/cubie/build-uboot)
# CROSS_COMPILE aarch64 Linux GCC prefix (default: the cubie toolchain
#               wrappers if present, else aarch64-linux-gnu-)
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SRC=${UBOOT_SRC:-/mnt/HaikuWork/cubie/src/u-boot-radxa}
OUT=${UBOOT_BUILD:-/mnt/HaikuWork/cubie/build-uboot}
BASE=9da4b3e82d64
if [[ -z ${CROSS_COMPILE:-} ]]; then
	if [[ -x /mnt/HaikuWork/cubie/toolchain/bin/aarch64-linux-gnu-gcc ]]; then
		CROSS_COMPILE=/mnt/HaikuWork/cubie/toolchain/bin/aarch64-linux-gnu-
	else
		CROSS_COMPILE=aarch64-linux-gnu-
	fi
fi
export CROSS_COMPILE

if [[ ! -d $SRC/.git ]]; then
	git clone -q --branch u-boot-dlan17 https://github.com/radxa/u-boot "$SRC"
fi
subject=$(sed -n 's/^Subject: \[PATCH\] //p' "$HERE/0001-sunxi-chainload.patch")
if [[ $(git -C "$SRC" log -1 --format=%s) != "$subject" ]]; then
	git -C "$SRC" checkout -q -B airos-chainload "$BASE"
	git -C "$SRC" -c user.name=air/OS -c user.email=airos@localhost \
		am -q "$HERE/0001-sunxi-chainload.patch"
fi

mkdir -p "$OUT"
make -s -C "$SRC" O="$OUT" radxa-cubie-a7s_defconfig > "$OUT/config.log" 2>&1
cat "$HERE/chainload.config" >> "$OUT/.config"
make -s -C "$SRC" O="$OUT" olddefconfig >> "$OUT/config.log" 2>&1
# NO_PYTHON: binman and pylibfdt are not needed for u-boot.bin
make -s -C "$SRC" O="$OUT" -j"$(nproc)" NO_PYTHON=1 u-boot.bin \
	> "$OUT/build.log" 2>&1 || { tail -20 "$OUT/build.log" >&2; exit 1; }
if [[ -n ${1:-} ]]; then
	cp "$OUT/u-boot.bin" "$1"
fi
echo "$OUT/u-boot.bin"
