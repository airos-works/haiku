#!/usr/bin/env bash
# Fetch the Cubie A7S GPU firmware (PowerVR BXM-4-64 MC1, BVNC
# 36.56.104.183) from Imagination's linux-firmware tree, branch "powervr":
#
#   rogue_36.56.104.183_v1.fw   the firmware for the GPU's MIPS core
#                               (FW 1.1, build 6976702)
#   LICENSE.powervr             its licence: redistribution in binary form
#                               with this notice, no reverse engineering
#
# The firmware is not kept in git. The image build (UserBuildConfig) puts
# both files under /boot/system/non-packaged/data/firmware/powervr when
# they are here, and leaves them out otherwise.
#
#   fetch-gpu-firmware.sh [directory]
#                    (default /mnt/HaikuWork/cubie/firmware/powervr)
# Prints the directory holding the two files.
set -euo pipefail
BASE=https://gitlab.freedesktop.org/imagination/linux-firmware/-/raw/powervr
DIR=${1:-/mnt/HaikuWork/cubie/firmware/powervr}

FILES=(rogue_36.56.104.183_v1.fw LICENSE.powervr)
declare -A URLS=(
	[rogue_36.56.104.183_v1.fw]=$BASE/powervr/rogue_36.56.104.183_v1.fw
	[LICENSE.powervr]=$BASE/LICENSE.powervr
)
declare -A SHA256=(
	[rogue_36.56.104.183_v1.fw]=1db1c399c17401d1f79d46c880db81c724d748c784d4639433b076aba2f9c0d2
	[LICENSE.powervr]=1c9aa6bd6703a7ce1cdb879542fa1d8aca115a327bd819b193c971de9c53f402
)

mkdir -p "$DIR"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/cubie-gpu-firmware.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

for file in "${FILES[@]}"; do
	if [[ -f $DIR/$file ]] \
		&& echo "${SHA256[$file]}  $DIR/$file" | sha256sum -c --quiet 2>/dev/null; then
		continue
	fi
	curl -sSfL -o "$WORK/$file" "${URLS[$file]}"
	if ! echo "${SHA256[$file]}  $WORK/$file" | sha256sum -c --quiet; then
		echo "$file: not the pinned version (sha256 differs)" >&2
		exit 1
	fi
	mv "$WORK/$file" "$DIR/$file"
done
echo "$DIR"
