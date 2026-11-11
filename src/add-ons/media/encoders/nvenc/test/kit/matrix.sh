#!/bin/sh
# H.264 through MediaConverter's command line on the card, each output
# decoded again and compared with its source (mediaconvert --compare).
#
#   matrix.sh sizes|rc|sources [clip]
#
# Run it in a directory holding `mediaconvert` (from the image, or a build)
# and a 1080p clip of 12 seconds or more (default bbb12s.mp4). `sources`
# takes more clips from SOURCES. Results go to out/ and are printed: the
# encoder's line, the speed, what the file is and the PSNR. The first line
# must name the card (not "CPU fallback"); a PSNR far under the others or
# "frame counts differ" is a failure.
cd "$(dirname "$0")"
mkdir -p out
SRC=${2:-bbb12s.mp4}
run() {
	name=$1; shift
	src=$1; shift
	start=$(date +%s)
	./mediaconvert --video h264 --container mp4 --end 12 "$@" "$src" out/$name.mp4 > out/$name.log 2>&1
	rc=$?
	end=$(date +%s)
	vline=$(grep '^video:' out/$name.log | head -1)
	speed=$(grep '^progress 100' out/$name.log | tail -1)
	info=$(./mediaconvert --info out/$name.mp4 2>&1 | grep 'video #0' | head -1)
	size=$(./mediaconvert --info out/$name.mp4 2>&1 | head -1 | sed 's/.*0:[0-9]*, //')
	cmp=$(./mediaconvert --compare "$src" out/$name.mp4 2>&1 | grep -E 'frames|error' | tail -1)
	echo "== $name rc=$rc $((end-start))s | $vline"
	echo "   $speed"
	echo "   $info | $size"
	echo "   $cmp"
}
case "$1" in
sizes)
	for height in 2160 1440 720 576 480 406 270 144 90 33; do
		run h$height "$SRC" --height $height
	done
	;;
rc)
	run q0 "$SRC" --quality 0
	run q50 "$SRC" --quality 50
	run q100 "$SRC" --quality 100
	run b1000 "$SRC" --bitrate 1000
	run b4000 "$SRC" --bitrate 4000
	run b12000 "$SRC" --bitrate 12000
	run s0 "$SRC" --speed 0
	run s4 "$SRC" --speed 4
	run cpu "$SRC" --cpu
	;;
sources)
	n=0
	for source in $SOURCES; do
		n=$((n + 1))
		run source$n "$source"
	done
	run fps60 "$SRC" --fps 60000
	run gray "$SRC" --gray
	;;
*)
	echo "usage: $0 sizes|rc|sources [clip]" >&2
	exit 2
	;;
esac
