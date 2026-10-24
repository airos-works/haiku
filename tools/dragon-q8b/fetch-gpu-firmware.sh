#!/usr/bin/env bash
# Fetch the Dragon Q8B GPU and video codec firmware (SC8280XP, Adreno 690,
# Iris) from linux-firmware at a pinned commit:
#
#   qcom/a660_sqe.fw                  the command processor's microcode (v1.12)
#   qcom/a660_gmu.bin                 the GPU management unit's (v3.01.0A)
#   qcom/sc8280xp/qcdxkmsuc8280.mbn   the zap shader, signed for the
#                                     SC8280XP (linux-firmware's
#                                     LENOVO/21BX copy, the same file as
#                                     Radxa's Dragon image)
#   qcom/vpu/vpu20_p4.mbn             the video codec's (SM8250's IRIS2,
#                                     VIDEO.VPU.1.0-00119), which the
#                                     SC8280XP authenticates and runs; the
#                                     X13s' qcvss8280.mbn boots but its
#                                     watchdog fires once decoding starts
#   LICENSE.qcom, NOTICE.qcom         their licence: redistributable
#
# The adreno and qcom_iris drivers load them from data/firmware/qcom. The firmware is not
# kept in git; tools/dragon-q8b/mesa/build.sh userland puts these files in
# the GPU user land (FIRMWARE=<directory>).
#
#   fetch-gpu-firmware.sh [directory]
#                    (default /mnt/HaikuWork/dragon/firmware)
# Prints the directory holding the files.
set -euo pipefail
COMMIT=bf992df7a95b5e19bb8f8546eb4d7c53ab2598d0
BASE=https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain
DIR=${1:-/mnt/HaikuWork/dragon/firmware}

# destination: source in linux-firmware
declare -A SOURCES=(
	[qcom/a660_sqe.fw]=qcom/a660_sqe.fw
	[qcom/a660_gmu.bin]=qcom/a660_gmu.bin
	[qcom/sc8280xp/qcdxkmsuc8280.mbn]=qcom/sc8280xp/LENOVO/21BX/qcdxkmsuc8280.mbn
	[qcom/vpu/vpu20_p4.mbn]=qcom/vpu/vpu20_p4.mbn
	[LICENSE.qcom]=LICENSES/LICENSE.qcom
	[NOTICE.qcom]=LICENSES/NOTICE.qcom
)
declare -A SHA256=(
	[qcom/a660_sqe.fw]=d222f3fe290ef0516ee0ec43082596bad2df0fcbc2e0bbb26987623cef90cf76
	[qcom/a660_gmu.bin]=8acab7b417d9ebde89a1de9ae1e2c261d352fcab122e31ecd580cec9fe2ae5e7
	[qcom/sc8280xp/qcdxkmsuc8280.mbn]=d7b8cc648c2f020c0fb7770b2d844b40688e99fb541e45514533049611b34d5c
	[qcom/vpu/vpu20_p4.mbn]=a66e86e34dc69a870fe631c73ed4a93d9fcfea6f3f2a4ac02cb118cb505bbbb2
	[LICENSE.qcom]=be904cd28cb292b80cdb6cf412ab0d9159d431671e987ad433c1f62e0988a9bc
	[NOTICE.qcom]=fa43e1b9a13b341a07adca9dbe73d0f9072d7966fdfe811c01f0dd2872d7309a
)

WORK=$(mktemp -d "${TMPDIR:-/tmp}/dragon-gpu-firmware.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

for file in "${!SOURCES[@]}"; do
	if [[ -f $DIR/$file ]] \
		&& echo "${SHA256[$file]}  $DIR/$file" | sha256sum -c --quiet 2>/dev/null; then
		continue
	fi
	mkdir -p "$WORK/$(dirname "$file")" "$DIR/$(dirname "$file")"
	curl -sSfL --retry 3 -o "$WORK/$file" "$BASE/${SOURCES[$file]}?id=$COMMIT"
	if ! echo "${SHA256[$file]}  $WORK/$file" | sha256sum -c --quiet; then
		echo "$file: not the pinned version (sha256 differs)" >&2
		exit 1
	fi
	mv "$WORK/$file" "$DIR/$file"
done
echo "$DIR"
