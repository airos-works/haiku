#!/bin/sh
# Several NVENC conversions at once, each compared with the source.
#   conc.sh [count] [clip of 30 s or more]
cd "$(dirname "$0")"
mkdir -p out
n=${1:-3}
SRC=${2:-bbb90s.mp4}
i=1
while [ $i -le $n ]; do
	./mediaconvert --video h264 --container mkv --end 30 --height $((1080 - i * 120)) "$SRC" out/c$i.mkv > out/c$i.log 2>&1 &
	i=$((i + 1))
done
wait
i=1
while [ $i -le $n ]; do
	echo "c$i: $(grep '^video:' out/c$i.log | cut -c1-60) | $(grep '^done' out/c$i.log) | $(grep -c -i 'fail\|error' out/c$i.log) errors | $(./mediaconvert --compare "$SRC" out/c$i.mkv | tail -1)"
	i=$((i + 1))
done
