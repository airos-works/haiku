#!/usr/bin/env bash
# Stages the AIC8800D80 (Quectel FCU760K) firmware for the Cubie A7S image:
# the files the aic8800wifi driver loads in normal mode, unchanged, with
# AICSemi's licence text and a note of where they came from. The image takes
# them from the staging directory (tools/cubie-a7s/UserBuildConfig); none of
# them belongs in git.
#
#   stage-aic8800-firmware.sh [source dir] [licence file] [staging dir]
#
# The source is Radxa's aic8800-firmware package for the board
# (5.0+git20260123.5f7be68d-5, files identical to AICSemi's SDK V5.0
# 2026_0123_5f7be68d).
set -euo pipefail
SOURCE=${1:-/mnt/HaikuWork/cubie/src/aic8800-board/lib/firmware/aic8800_fw/USB/aic8800D80}
LICENCE=${2:-/mnt/HaikuWork/cubie/evidence/wifi/LICENSE.aic}
STAGE=${3:-/mnt/HaikuWork/cubie/firmware/aic8800wifi}

files=(
	fw_patch_table_8800d80_u02.bin
	fw_adid_8800d80_u02.bin
	fw_patch_8800d80_u02.bin
	fw_patch_8800d80_u02_ext0.bin
	fmacfw_8800d80_u02.bin
	fmacfw_8800d80_h_u02.bin
	aic_userconfig_8800d80.txt
)

mkdir -p "$STAGE"
for file in "${files[@]}"; do
	cp -p "$SOURCE/$file" "$STAGE/$file"
done
cp -p "$LICENCE" "$STAGE/LICENSE.aic"

{
	echo "AIC8800D80 firmware for the aic8800wifi driver, unmodified."
	echo "Source: Radxa aic8800-firmware 5.0+git20260123.5f7be68d-5"
	echo "(AICSemi SDK V5.0 2026_0123_5f7be68d). Licence: LICENSE.aic."
	echo
	(cd "$STAGE" && md5sum "${files[@]}")
} > "$STAGE/PROVENANCE"
echo "staged ${#files[@]} files in $STAGE"
