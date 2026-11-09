#!/bin/sh
# fetch-models.sh - download the standard public test models of the Neural
# Kit: what Lookout, the neural command and the kit's tests are tried with.
#
# Usage:
#   tools/airos/neural/fetch-models.sh DIRECTORY
#
# Puts the models where the kit's programs look for them below a data
# directory:
#
#   DIRECTORY/neural/models/
#     mobilenet_v1_1.0_224_quant.tflite   classification, 8 bit quantized
#     mobilenet_v2_1.0_224.tflite         classification, float
#     labels.txt                          the 1001 ImageNet class names of both
#     ssd_mobilenet_v1_quant.tflite       detection (COCO), 8 bit quantized
#     ssd_mobilenet_v1_quant.txt          its 90 class names ("???" first)
#
# DIRECTORY is a data directory of the machine (on air/OS:
# /boot/home/config/non-packaged/data for one user, or the data directory of
# a package or an image that is being put together). Lookout lists what is
# in neural/models of every data directory.
#
# Files that are there already and have the right checksum are kept, so the
# script can be run again; the archives (about 120 MB) are downloaded to a
# temporary directory and deleted afterwards.
#
# Needs: curl, tar, unzip, sha256sum (all part of air/OS and of any Linux).
#
# Licence of the files: the models and label files are published by the
# TensorFlow authors (Google) under the Apache License, Version 2.0
# (https://www.apache.org/licenses/LICENSE-2.0); the MobileNet and SSD
# MobileNet models were trained on ImageNet (ILSVRC-2012) and COCO. This
# script only downloads them from where they are published; it does not
# copy them into the source tree.

set -e

BASE=https://storage.googleapis.com/download.tensorflow.org/models

if [ $# -ne 1 ] || [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
	sed -n '2,/^$/s/^# \{0,1\}//p' "$0"
	exit 2
fi

target="$1/neural/models"
mkdir -p "$target"

work=$(mktemp -d "${TMPDIR:-/tmp}/neural-models.XXXXXX")
trap 'rm -rf "$work"' EXIT

# has_file NAME SHA256: whether NAME is there already, and intact
has_file()
{
	[ -f "$target/$1" ] || return 1
	[ "$(sha256sum "$target/$1" | cut -d' ' -f1)" = "$2" ]
}

# install_file SOURCE NAME SHA256: checks what was unpacked and moves it in
install_file()
{
	sum=$(sha256sum "$1" | cut -d' ' -f1)
	if [ "$sum" != "$3" ]; then
		echo "fetch-models: $2 has checksum $sum, expected $3" >&2
		exit 1
	fi
	mv -f "$1" "$target/$2"
	chmod 644 "$target/$2"
	echo "  $2"
}

# fetch URL: downloads an archive into the work directory and unpacks it
# into "$work/unpacked"
fetch()
{
	archive="$work/$(basename "$1")"
	rm -rf "$work/unpacked"
	mkdir "$work/unpacked"
	echo "downloading $1"
	curl --fail --location --silent --show-error --retry 3 -o "$archive" "$1"
	case "$archive" in
		*.zip)	unzip -q -o "$archive" -d "$work/unpacked" ;;
		*)		tar -xzf "$archive" -C "$work/unpacked" ;;
	esac
	rm -f "$archive"
}

# find_file NAME: where the archive had the file (some have a directory)
find_file()
{
	file=$(find "$work/unpacked" -name "$1" -type f | head -n 1)
	if [ -z "$file" ]; then
		echo "fetch-models: no $1 in the archive" >&2
		exit 1
	fi
	echo "$file"
}

# SHA-256 of the files as published (as of 2026-11)
V1_QUANT=ecc3a67c47c5a609ec35f6a58a7d97532834e43df4cb7d3f1204a8164b7d20dd
V2_FLOAT=9f3bc29e38e90842a852bfed957dbf5e36f2d97a91dd17736b1e5c0aca8d3303
IMAGENET_LABELS=536feacc519de3d418de26b2effb4d75694a8c4c0063e36499a46fa8061e2da9
SSD_QUANT=e4b118e5e4531945de2e659742c7c590f7536f8d0ed26d135abcfe83b4779d13
SSD_LABELS=c7e79c855f73cbba9f33d649d60e1676eb0a974021a41696d1ac0d4b7f7e0211

echo "models in $target:"

if ! has_file mobilenet_v1_1.0_224_quant.tflite $V1_QUANT; then
	fetch $BASE/mobilenet_v1_2018_08_02/mobilenet_v1_1.0_224_quant.tgz
	install_file "$(find_file mobilenet_v1_1.0_224_quant.tflite)" \
		mobilenet_v1_1.0_224_quant.tflite $V1_QUANT
fi

if ! has_file mobilenet_v2_1.0_224.tflite $V2_FLOAT; then
	fetch $BASE/tflite_11_05_08/mobilenet_v2_1.0_224.tgz
	install_file "$(find_file mobilenet_v2_1.0_224.tflite)" \
		mobilenet_v2_1.0_224.tflite $V2_FLOAT
fi

if ! has_file labels.txt $IMAGENET_LABELS; then
	fetch $BASE/tflite/mobilenet_v1_1.0_224_quant_and_labels.zip
	install_file "$(find_file labels_mobilenet_quant_v1_224.txt)" \
		labels.txt $IMAGENET_LABELS
fi

if ! has_file ssd_mobilenet_v1_quant.tflite $SSD_QUANT \
	|| ! has_file ssd_mobilenet_v1_quant.txt $SSD_LABELS; then
	fetch $BASE/tflite/coco_ssd_mobilenet_v1_1.0_quant_2018_06_29.zip
	install_file "$(find_file detect.tflite)" \
		ssd_mobilenet_v1_quant.tflite $SSD_QUANT
	install_file "$(find_file labelmap.txt)" \
		ssd_mobilenet_v1_quant.txt $SSD_LABELS
fi

echo "done."
