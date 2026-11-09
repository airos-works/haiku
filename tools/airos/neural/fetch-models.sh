#!/bin/sh
# fetch-models.sh - download the standard public test models of the Neural
# Kit: what Lookout, the neural command and the kit's tests are tried with,
# and the speech models of BSpeechRecognizer.
#
# Usage:
#   tools/airos/neural/fetch-models.sh DIRECTORY [SET...]
#
# Puts the models where the kit's programs look for them below a data
# directory. The sets (default: vision):
#
#   vision    DIRECTORY/neural/models/
#     mobilenet_v1_1.0_224_quant.tflite   classification, 8 bit quantized
#     mobilenet_v2_1.0_224.tflite         classification, float
#     labels.txt                          the 1001 ImageNet class names of both
#     ssd_mobilenet_v1_quant.tflite       detection (COCO), 8 bit quantized
#     ssd_mobilenet_v1_quant.txt          its 90 class names ("???" first)
#
#   speech    DIRECTORY/neural/models/whisper-tiny.en/
#     Whisper "tiny.en" (English) as BSpeechRecognizer takes it:
#     encoder.tflite, decoder.tflite, vocabulary.bin, mel_filters.bin,
#     speech.conf, LICENSE (about 150 MB)
#   speech-multilingual
#             DIRECTORY/neural/models/whisper-tiny/
#     the same for Whisper "tiny", which knows 99 languages
#
#   all       all of the above
#
# DIRECTORY is a data directory of the machine (on air/OS:
# /boot/home/config/non-packaged/data for one user, or the data directory of
# a package or an image that is being put together). Lookout lists what is
# in neural/models of every data directory; BSpeechRecognizer takes the
# first speech model it finds there.
#
# Files that are there already and have the right checksum are kept, so the
# script can be run again; the archives (about 120 MB) are downloaded to a
# temporary directory and deleted afterwards.
#
# The speech models are not downloaded as they are used: OpenAI publishes
# PyTorch checkpoints, and whisper_convert.py (beside this script) makes
# the kit's files of them, which takes Python 3 with numpy, flatbuffers and
# ai-edge-litert (pip install numpy flatbuffers ai-edge-litert; PYTHON
# names the interpreter, default python3) and so a Linux host: copy the
# resulting directory to the machine. A model directory that is there
# already (it has a speech.conf) is kept. Where someone has put such a
# directory up as an archive, SPEECH_ARCHIVE_URL=<url of the .tar.gz>
# takes that instead of converting (for one speech set at a time).
#
# Needs: curl, tar, unzip, sha256sum (all part of air/OS and of any Linux).
#
# Licence of the files: the vision models and label files are published by
# the TensorFlow authors (Google) under the Apache License, Version 2.0
# (https://www.apache.org/licenses/LICENSE-2.0); the MobileNet and SSD
# MobileNet models were trained on ImageNet (ILSVRC-2012) and COCO. Whisper
# (the checkpoints, the vocabularies and the code) is published by OpenAI
# under the MIT License, (c) 2022 OpenAI; the licence text is put beside
# the converted model as LICENSE. This script only downloads the files
# from where they are published; it does not copy them into the source
# tree.

set -e

BASE=https://storage.googleapis.com/download.tensorflow.org/models

if [ $# -lt 1 ] || [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
	sed -n '2,/^$/s/^# \{0,1\}//p' "$0"
	exit 2
fi

here=$(cd "$(dirname "$0")" && pwd)
target="$1/neural/models"
shift
mkdir -p "$target"

vision=
speech=
if [ $# -eq 0 ]; then
	vision=yes
fi
for set in "$@"; do
	case "$set" in
		vision)					vision=yes ;;
		speech)					speech="$speech tiny.en" ;;
		speech-multilingual)	speech="$speech tiny" ;;
		all)					vision=yes; speech="tiny.en tiny" ;;
		*)
			echo "fetch-models: no set \"$set\" (vision, speech," \
				"speech-multilingual, all)" >&2
			exit 2
			;;
	esac
done

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

# Whisper, as of 2026-11: the checkpoints are named by their SHA-256; the
# other files are those of the release v20250625.
WHISPER_MODELS=https://openaipublic.azureedge.net/main/whisper/models
WHISPER_SOURCE=https://raw.githubusercontent.com/openai/whisper/v20250625
TINY_EN=d3dd57d32accea0b295c96e26691aa14d8822fac7d9d27d5dc00b4ca2826dd03
TINY=65147644a518d12f04e32d6f3b26facc3f8dd46e5390956a9424a650c0ce22b9
GPT2_VOCABULARY=306cd27f03c1a714eca7108e03d66b7dc042abe8c258b44c199a7ed9838dd930
VOCABULARY_ALL=b34b360dbb493e781e479794586d661700670d65564001f23024971d1f2fa126
MEL_FILTERS=7450ae70723a5ef9d341e3cee628c7cb0177f36ce42c44b7ed2bf3325f0f6d4c
WHISPER_LICENSE=b5d65a59060e68c4ff940e1eddfa6f94b2d68fdf58ed7f4dd57721c997e35e9d

# download URL FILE SHA256: one file into the work directory, checked
download()
{
	echo "downloading $1"
	curl --fail --location --silent --show-error --retry 3 -o "$work/$2" "$1"
	sum=$(sha256sum "$work/$2" | cut -d' ' -f1)
	if [ "$sum" != "$3" ]; then
		echo "fetch-models: $2 has checksum $sum, expected $3" >&2
		exit 1
	fi
}

# speech_model NAME: Whisper NAME (tiny.en, tiny) as a model directory
speech_model()
{
	directory="$target/whisper-$1"
	if [ -f "$directory/speech.conf" ] && [ -s "$directory/encoder.tflite" ] \
		&& [ -s "$directory/decoder.tflite" ]; then
		echo "  whisper-$1 (kept)"
		return
	fi

	if [ -n "$SPEECH_ARCHIVE_URL" ]; then
		# a model directory someone converted before
		echo "downloading $SPEECH_ARCHIVE_URL"
		curl --fail --location --silent --show-error --retry 3 \
			-o "$work/speech.tar.gz" "$SPEECH_ARCHIVE_URL"
		rm -rf "$work/unpacked"
		mkdir "$work/unpacked"
		tar -xzf "$work/speech.tar.gz" -C "$work/unpacked"
		rm -f "$work/speech.tar.gz"
		description=$(find "$work/unpacked" -name speech.conf -type f \
			| head -n 1)
		if [ -z "$description" ]; then
			echo "fetch-models: no model directory in the archive" >&2
			exit 1
		fi
		mkdir -p "$directory"
		for file in encoder.tflite decoder.tflite vocabulary.bin \
				mel_filters.bin speech.conf LICENSE; do
			mv -f "$(dirname "$description")/$file" "$directory/$file"
			chmod 644 "$directory/$file"
		done
		echo "  whisper-$1"
		return
	fi

	python=${PYTHON:-python3}
	if ! "$python" -c "import numpy, flatbuffers, ai_edge_litert" \
			2> /dev/null; then
		echo "fetch-models: the speech models are converted with Python:" \
			"$python lacks numpy, flatbuffers or ai-edge-litert (see the" \
			"top of this script)" >&2
		exit 1
	fi

	if [ "$1" = "tiny.en" ]; then
		download $WHISPER_MODELS/$TINY_EN/tiny.en.pt tiny.en.pt $TINY_EN
		download $WHISPER_SOURCE/whisper/assets/gpt2.tiktoken \
			vocabulary.tiktoken $GPT2_VOCABULARY
	else
		download $WHISPER_MODELS/$TINY/tiny.pt tiny.pt $TINY
		download $WHISPER_SOURCE/whisper/assets/multilingual.tiktoken \
			vocabulary.tiktoken $VOCABULARY_ALL
	fi
	download $WHISPER_SOURCE/whisper/assets/mel_filters.npz mel_filters.npz \
		$MEL_FILTERS
	download $WHISPER_SOURCE/LICENSE LICENSE.whisper $WHISPER_LICENSE

	# converted and checked against the checkpoint in the work directory,
	# moved in only when that went well
	echo "converting Whisper $1"
	"$python" "$here/whisper_convert.py" --checkpoint "$work/$1.pt" \
		--vocabulary "$work/vocabulary.tiktoken" \
		--mel-filters "$work/mel_filters.npz" \
		--license "$work/LICENSE.whisper" --name "$1" --check \
		--output "$work/whisper-$1" > "$work/convert.log" 2>&1 || {
		cat "$work/convert.log" >&2
		echo "fetch-models: converting Whisper $1 failed" >&2
		exit 1
	}
	grep "agree" "$work/convert.log" || true
	rm -f "$work/$1.pt"
	mkdir -p "$directory"
	for file in encoder.tflite decoder.tflite vocabulary.bin mel_filters.bin \
			speech.conf LICENSE; do
		mv -f "$work/whisper-$1/$file" "$directory/$file"
		chmod 644 "$directory/$file"
	done
	echo "  whisper-$1"
}

echo "models in $target:"

for name in $speech; do
	speech_model "$name"
done

if [ -z "$vision" ]; then
	echo "done."
	exit 0
fi

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
